#include <xcb/xcb.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <vector>

#include "mistercast/interfaces.hpp"
#include "x11_display.hpp"
#include "x11_source_catalog.hpp"
#ifdef MISTERCAST_HAVE_RANDR
#include <xcb/randr.h>
#endif

namespace mistercast {
namespace {
class X11SourceCatalog {
  X11DisplayConnection display_;

  xcb_atom_t atom(const char* name) const {
    const auto cookie =
        xcb_intern_atom(display_.connection(), 0, std::strlen(name), name);
    auto* reply =
        xcb_intern_atom_reply(display_.connection(), cookie, nullptr);
    const auto result = reply ? reply->atom : xcb_atom_t{XCB_ATOM_NONE};
    free(reply);
    return result;
  }

  static std::string propertyString(const xcb_get_property_reply_t* reply) {
    if (!reply || xcb_get_property_value_length(reply) <= 0) return {};
    return {static_cast<const char*>(xcb_get_property_value(reply)),
            size_t(xcb_get_property_value_length(reply))};
  }

 public:
  bool connect(std::string& error) { return display_.connect(error); }

  std::vector<CaptureWindow> windows() const {
    const auto clientList = atom("_NET_CLIENT_LIST_STACKING");
    const auto netName = atom("_NET_WM_NAME");
    const auto utf8 = atom("UTF8_STRING");
    const auto pidAtom = atom("_NET_WM_PID");
    std::vector<xcb_window_t> ids;
    if (clientList) {
      auto* reply = xcb_get_property_reply(
          display_.connection(),
          xcb_get_property(display_.connection(), 0, display_.screen()->root,
                           clientList, XCB_ATOM_WINDOW, 0, 4096),
          nullptr);
      if (reply) {
        const auto count = xcb_get_property_value_length(reply) /
                           int(sizeof(xcb_window_t));
        if (count > 0) {
          auto* values =
              static_cast<xcb_window_t*>(xcb_get_property_value(reply));
          ids.assign(values, values + count);
        }
        free(reply);
      }
    }
    if (ids.empty()) {
      auto* tree = xcb_query_tree_reply(
          display_.connection(),
          xcb_query_tree(display_.connection(), display_.screen()->root),
          nullptr);
      if (tree) {
        const auto count = xcb_query_tree_children_length(tree);
        if (count > 0) {
          auto* children = xcb_query_tree_children(tree);
          ids.assign(children, children + count);
        }
        free(tree);
      }
    }

    // Send every per-window request first so the synchronous chooser pays one
    // server round trip rather than four or five per candidate.
    struct Queries {
      xcb_window_t id;
      xcb_get_window_attributes_cookie_t attributes;
      xcb_get_geometry_cookie_t geometry;
      xcb_get_property_cookie_t pid;
      xcb_get_property_cookie_t netName;
      xcb_get_property_cookie_t wmName;
    };
    std::vector<Queries> queries;
    queries.reserve(ids.size());
    for (const auto id : ids) {
      queries.push_back(
          {id,
           xcb_get_window_attributes(display_.connection(), id),
           xcb_get_geometry(display_.connection(), id),
           pidAtom ? xcb_get_property(display_.connection(), 0, id, pidAtom,
                                      XCB_ATOM_CARDINAL, 0, 1)
                   : xcb_get_property_cookie_t{},
           netName ? xcb_get_property(display_.connection(), 0, id, netName,
                                      utf8, 0, 1024)
                   : xcb_get_property_cookie_t{},
           xcb_get_property(display_.connection(), 0, id, XCB_ATOM_WM_NAME,
                            XCB_ATOM_STRING, 0, 1024)});
    }

    std::vector<CaptureWindow> result;
    result.reserve(queries.size());
    for (const auto& query : queries) {
      auto* attributes = xcb_get_window_attributes_reply(
          display_.connection(), query.attributes, nullptr);
      auto* geometry =
          xcb_get_geometry_reply(display_.connection(), query.geometry, nullptr);
      auto* pid = pidAtom
                      ? xcb_get_property_reply(display_.connection(), query.pid,
                                               nullptr)
                      : nullptr;
      auto* netTitle = netName ? xcb_get_property_reply(display_.connection(),
                                                        query.netName, nullptr)
                               : nullptr;
      auto* wmTitle =
          xcb_get_property_reply(display_.connection(), query.wmName, nullptr);
      const bool ownWindow =
          pid && xcb_get_property_value_length(pid) == sizeof(uint32_t) &&
          *static_cast<uint32_t*>(xcb_get_property_value(pid)) ==
              static_cast<uint32_t>(getpid());
      auto title = propertyString(netTitle);
      if (title.empty()) title = propertyString(wmTitle);
      if (attributes && geometry && !ownWindow && !title.empty() &&
          attributes->map_state == XCB_MAP_STATE_VIEWABLE && geometry->width &&
          geometry->height)
        result.push_back({query.id, std::move(title), geometry->width,
                          geometry->height});
      free(attributes);
      free(geometry);
      free(pid);
      free(netTitle);
      free(wmTitle);
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
      return a.title < b.title;
    });
    return result;
  }

  std::vector<Monitor> monitors() const {
    return enumerateX11Monitors(display_.connection(), display_.screen());
  }
};
}  // namespace

std::vector<Monitor> enumerateX11Monitors(xcb_connection_t* connection,
                                          xcb_screen_t* screen) {
  std::vector<Monitor> result;
#ifdef MISTERCAST_HAVE_RANDR
  auto* reply = xcb_randr_get_monitors_reply(
      connection, xcb_randr_get_monitors(connection, screen->root, 1), nullptr);
  if (reply) {
    auto monitors = xcb_randr_get_monitors_monitors_iterator(reply);
    for (; monitors.rem; xcb_randr_monitor_info_next(&monitors)) {
      const auto* monitor = monitors.data;
      auto* nameReply = xcb_get_atom_name_reply(
          connection, xcb_get_atom_name(connection, monitor->name), nullptr);
      std::string name =
          nameReply ? std::string(xcb_get_atom_name_name(nameReply),
                                  xcb_get_atom_name_name_length(nameReply))
                    : "monitor";
      free(nameReply);
      result.push_back({name, monitor->x, monitor->y, monitor->width,
                        monitor->height, bool(monitor->primary)});
    }
    free(reply);
  }
#endif
  if (result.empty())
    result.push_back({"X11-screen-0", 0, 0, screen->width_in_pixels,
                      screen->height_in_pixels, true});
  return result;
}

std::vector<Monitor> x11Monitors(std::string& error) {
  X11SourceCatalog catalog;
  if (!catalog.connect(error)) return {};
  return catalog.monitors();
}

std::vector<CaptureWindow> x11CaptureWindows(std::string& error) {
  X11SourceCatalog catalog;
  if (!catalog.connect(error)) return {};
  return catalog.windows();
}
}  // namespace mistercast
