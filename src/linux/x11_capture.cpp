#include <sys/ipc.h>
#include <sys/shm.h>
#include <xcb/shm.h>
#include <xcb/composite.h>
#include <xcb/xcb.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unistd.h>

#include "mistercast/interfaces.hpp"
#include "mistercast/transform.hpp"
#ifdef MISTERCAST_HAVE_RANDR
#include <xcb/randr.h>
#endif

namespace mistercast {
class X11Capture final : public IVideoCapture {
  xcb_connection_t* connection_{};
  xcb_screen_t* screen_{};
  xcb_drawable_t drawable_{};
  xcb_window_t selectedWindow_{};
  xcb_pixmap_t windowPixmap_{};
  bool windowMode_{};
  bool windowViewable_{};
  bool compositePixmapAvailable_{};
  Monitor selected_;
  CropRect region_{};
  ErrorCallback error_;
  uint64_t sequence_{};
  std::atomic<bool> running_{false};
  mutable std::mutex mutex_;
  int shmId_{-1};
  uint8_t* shmData_{};
  xcb_shm_seg_t shmSeg_{};
  size_t shmSize_{};
  bool useShm_{};
  bool shmAttempted_{};
  // Pixel layout of the root visual, resolved once at start() instead of
  // rescanning every visual and pixmap format on every captured frame.
  uint32_t redMask_{0xff0000}, greenMask_{0xff00}, blueMask_{0xff};
  uint8_t depth_{}, bitsPerPixel_{32};
  bool lsbFirst_{true};
  uint32_t consecutiveFailures_{}, shmFailures_{};
  uint32_t windowGeometryPoll_{};
  // Roughly two seconds of retries at the capture rate before giving up.
  static constexpr uint32_t kMaxConsecutiveFailures = 120, kMaxShmFailures = 5;
  void resolvePixelFormat(uint8_t depth, xcb_visualid_t visual = 0) {
    auto* setup = xcb_get_setup(connection_);
    lsbFirst_ = setup->image_byte_order == XCB_IMAGE_ORDER_LSB_FIRST;
    depth_ = depth;
    for (auto v = xcb_screen_allowed_depths_iterator(screen_); v.rem;
         xcb_depth_next(&v))
      for (auto q = xcb_depth_visuals_iterator(v.data); q.rem;
           xcb_visualtype_next(&q))
        if (q.data->visual_id == (visual ? visual : screen_->root_visual)) {
          redMask_ = q.data->red_mask;
          greenMask_ = q.data->green_mask;
          blueMask_ = q.data->blue_mask;
        }
    bitsPerPixel_ = 32;
    for (auto f = xcb_setup_pixmap_formats_iterator(setup); f.rem;
         xcb_format_next(&f))
      if (f.data->depth == depth_) bitsPerPixel_ = f.data->bits_per_pixel;
  }
  CropRect clampRegion(const CropRect& r) const {
    if (!r.width || !r.height) return {0, 0, selected_.width, selected_.height};
    CropRect c;
    c.width = std::min<uint32_t>(r.width, selected_.width);
    c.height = std::min<uint32_t>(r.height, selected_.height);
    c.x = std::min<uint32_t>(r.x, selected_.width - c.width);
    c.y = std::min<uint32_t>(r.y, selected_.height - c.height);
    return c;
  }
  void closeShm() {
    if (connection_ && shmSeg_) xcb_shm_detach(connection_, shmSeg_);
    if (shmData_) shmdt(shmData_);
    if (shmId_ >= 0) shmctl(shmId_, IPC_RMID, nullptr);
    shmId_ = -1;
    shmData_ = nullptr;
    shmSeg_ = 0;
    shmSize_ = 0;
    useShm_ = false;
  }
  void closeWindowPixmap() {
    if (connection_ && windowPixmap_)
      xcb_free_pixmap(connection_, windowPixmap_);
    windowPixmap_ = 0;
  }
  void invalidateWindowDrawable() {
    closeWindowPixmap();
    drawable_ = 0;
    windowViewable_ = false;
  }
  void close() {
    closeShm();
    closeWindowPixmap();
    if (connection_) {
      xcb_disconnect(connection_);
      connection_ = nullptr;
      screen_ = nullptr;
    }
    drawable_ = 0;
    windowViewable_ = false;
    compositePixmapAvailable_ = false;
    shmAttempted_ = false;
  }
  bool connect(std::string& error) {
    if (connection_) return true;
    const char* d = std::getenv("DISPLAY");
    if (!d || !*d) {
      error = "X11/Xorg required: DISPLAY is not set";
      return false;
    }
    int index = 0;
    connection_ = xcb_connect(nullptr, &index);
    if (!connection_ || xcb_connection_has_error(connection_)) {
      close();
      error =
          "cannot connect to X11 display; native Wayland capture is not "
          "supported";
      return false;
    }
    auto it = xcb_setup_roots_iterator(xcb_get_setup(connection_));
    while (index-- && it.rem) xcb_screen_next(&it);
    screen_ = it.data;
    if (!screen_) {
      close();
      error = "X11 display has no screen";
      return false;
    }
    const auto* composite =
        xcb_get_extension_data(connection_, &xcb_composite_id);
    if (composite && composite->present) {
      auto* version = xcb_composite_query_version_reply(
          connection_, xcb_composite_query_version(connection_, 0, 4), nullptr);
      compositePixmapAvailable_ =
          version && (version->major_version > 0 || version->minor_version >= 2);
      free(version);
    }
    return true;
  }

  bool setupShm() {
    shmAttempted_ = true;
    auto version = xcb_shm_query_version_reply(
        connection_, xcb_shm_query_version(connection_), nullptr);
    if (!version) return false;
    free(version);
    shmSize_ = size_t(selected_.width) * selected_.height * 4;
    shmId_ = shmget(IPC_PRIVATE, shmSize_, IPC_CREAT | 0600);
    if (shmId_ >= 0) {
      auto* p = shmat(shmId_, nullptr, 0);
      if (p != reinterpret_cast<void*>(-1)) {
        shmData_ = static_cast<uint8_t*>(p);
        shmSeg_ = xcb_generate_id(connection_);
        auto cookie = xcb_shm_attach_checked(connection_, shmSeg_, shmId_, 0);
        auto* attachError = xcb_request_check(connection_, cookie);
        if (!attachError) useShm_ = true;
        free(attachError);
      }
    }
    if (!useShm_) closeShm();
    return useShm_;
  }
  // Re-resolves the selected source after a failed capture or periodic window
  // check. Geometry changes invalidate the drawable and shared segment;
  // recovering here keeps transient changes from ending the session.
  bool recoverLocked() {
    const bool connectionLost =
        connection_ && xcb_connection_has_error(connection_);
    if (connectionLost) close();
    if (windowMode_) {
      std::string ignored;
      if (!connect(ignored)) return false;
      auto geometry = xcb_get_geometry_reply(
          connection_, xcb_get_geometry(connection_, selectedWindow_), nullptr);
      auto attributes = xcb_get_window_attributes_reply(
          connection_, xcb_get_window_attributes(connection_, selectedWindow_),
          nullptr);
      if (!geometry || !attributes ||
          attributes->map_state != XCB_MAP_STATE_VIEWABLE) {
        free(geometry);
        free(attributes);
        invalidateWindowDrawable();
        return false;
      }
      const bool resized = geometry->width != selected_.width ||
                           geometry->height != selected_.height;
      const bool remapped = !windowViewable_;
      windowViewable_ = true;
      selected_.width = geometry->width;
      selected_.height = geometry->height;
      selected_.x = selected_.y = 0;
      if (compositePixmapAvailable_ && (resized || remapped)) {
        closeWindowPixmap();
        const auto pixmap = xcb_generate_id(connection_);
        auto* pixmapError = xcb_request_check(
            connection_, xcb_composite_name_window_pixmap_checked(
                             connection_, selectedWindow_, pixmap));
        if (!pixmapError) windowPixmap_ = pixmap;
        free(pixmapError);
      }
      // XComposite gives an isolated backing pixmap even while another window
      // overlaps the selected one. Fall back to the drawable on X servers
      // where the window is not redirected (for example, no compositor).
      drawable_ = windowPixmap_ ? windowPixmap_ : selectedWindow_;
      region_ = clampRegion(region_);
      if (resized) {
        closeShm();
        setupShm();
      } else if (!shmAttempted_)
        setupShm();
      resolvePixelFormat(geometry->depth, attributes->visual);
      free(geometry);
      free(attributes);
      return true;
    }
    std::string ignored;
    auto ms = monitorsLocked(ignored);
    auto it = std::find_if(ms.begin(), ms.end(), [&](const Monitor& m) {
      return m.name == selected_.name;
    });
    if (it == ms.end()) return false;
    const bool moved = it->width != selected_.width ||
                       it->height != selected_.height || it->x != selected_.x ||
                       it->y != selected_.y;
    selected_ = *it;
    drawable_ = screen_->root;
    region_ = clampRegion(region_);
    if (moved || connectionLost) {
      closeShm();
      setupShm();
      resolvePixelFormat(screen_ ? screen_->root_depth : depth_);
    } else if (!shmAttempted_)
      setupShm();
    return true;
  }
  std::vector<Monitor> monitorsLocked(std::string& error) {
    if (!connect(error)) return {};
    std::vector<Monitor> result;
#ifdef MISTERCAST_HAVE_RANDR
    auto ck = xcb_randr_get_monitors(connection_, screen_->root, 1);
    auto* r = xcb_randr_get_monitors_reply(connection_, ck, nullptr);
    if (r) {
      auto it = xcb_randr_get_monitors_monitors_iterator(r);
      for (; it.rem; xcb_randr_monitor_info_next(&it)) {
        auto* m = it.data;
        auto nc = xcb_get_atom_name(connection_, m->name);
        auto* nr = xcb_get_atom_name_reply(connection_, nc, nullptr);
        std::string name = nr ? std::string(xcb_get_atom_name_name(nr),
                                            xcb_get_atom_name_name_length(nr))
                              : "monitor";
        free(nr);
        result.push_back(
            {name, m->x, m->y, m->width, m->height, bool(m->primary)});
      }
      free(r);
    }
#endif
    if (result.empty())
      result.push_back({"X11-screen-0", 0, 0, screen_->width_in_pixels,
                        screen_->height_in_pixels, true});
    return result;
  }

  xcb_atom_t atom(const char* name) {
    const auto cookie = xcb_intern_atom(connection_, 0, std::strlen(name), name);
    auto* reply = xcb_intern_atom_reply(connection_, cookie, nullptr);
    const xcb_atom_t result = reply ? reply->atom : xcb_atom_t{XCB_ATOM_NONE};
    free(reply);
    return result;
  }

  static std::string propertyString(const xcb_get_property_reply_t* reply) {
    if (!reply || xcb_get_property_value_length(reply) <= 0) return {};
    return {static_cast<const char*>(xcb_get_property_value(reply)),
            size_t(xcb_get_property_value_length(reply))};
  }

  std::vector<CaptureWindow> windowsLocked(std::string& error) {
    if (!connect(error)) return {};
    const auto clientList = atom("_NET_CLIENT_LIST_STACKING");
    const auto netName = atom("_NET_WM_NAME");
    const auto utf8 = atom("UTF8_STRING");
    const auto pidAtom = atom("_NET_WM_PID");
    std::vector<xcb_window_t> ids;
    if (clientList) {
      auto* reply = xcb_get_property_reply(
          connection_, xcb_get_property(connection_, 0, screen_->root,
                                        clientList, XCB_ATOM_WINDOW, 0, 4096),
          nullptr);
      if (reply) {
        auto* values = static_cast<xcb_window_t*>(xcb_get_property_value(reply));
        const auto count = xcb_get_property_value_length(reply) / sizeof(*values);
        ids.assign(values, values + count);
        free(reply);
      }
    }
    if (ids.empty()) {
      auto* tree = xcb_query_tree_reply(
          connection_, xcb_query_tree(connection_, screen_->root), nullptr);
      if (tree) {
        auto* children = xcb_query_tree_children(tree);
        ids.assign(children, children + xcb_query_tree_children_length(tree));
        free(tree);
      }
    }
    // XCB requests are asynchronous. Issue every per-window query first so
    // discovery costs one server round trip instead of four or five per window;
    // this function is called synchronously by the Qt window chooser.
    struct WindowQueries {
      xcb_window_t id;
      xcb_get_window_attributes_cookie_t attributes;
      xcb_get_geometry_cookie_t geometry;
      xcb_get_property_cookie_t pid;
      xcb_get_property_cookie_t netName;
      xcb_get_property_cookie_t wmName;
    };
    std::vector<WindowQueries> queries;
    queries.reserve(ids.size());
    for (const auto id : ids) {
      queries.push_back(
          {id,
           xcb_get_window_attributes(connection_, id),
           xcb_get_geometry(connection_, id),
           pidAtom ? xcb_get_property(connection_, 0, id, pidAtom,
                                      XCB_ATOM_CARDINAL, 0, 1)
                   : xcb_get_property_cookie_t{},
           netName ? xcb_get_property(connection_, 0, id, netName, utf8, 0, 1024)
                   : xcb_get_property_cookie_t{},
           xcb_get_property(connection_, 0, id, XCB_ATOM_WM_NAME,
                            XCB_ATOM_STRING, 0, 1024)});
    }
    std::vector<CaptureWindow> result;
    result.reserve(queries.size());
    for (const auto& query : queries) {
      auto* attributes = xcb_get_window_attributes_reply(
          connection_, query.attributes, nullptr);
      auto* geometry =
          xcb_get_geometry_reply(connection_, query.geometry, nullptr);
      auto* pid = pidAtom
                      ? xcb_get_property_reply(connection_, query.pid, nullptr)
                      : nullptr;
      auto* netTitle = netName ? xcb_get_property_reply(connection_, query.netName,
                                                        nullptr)
                               : nullptr;
      auto* wmTitle =
          xcb_get_property_reply(connection_, query.wmName, nullptr);
      bool ownWindow = false;
      if (pid && xcb_get_property_value_length(pid) == sizeof(uint32_t))
        ownWindow = *static_cast<uint32_t*>(xcb_get_property_value(pid)) ==
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

 public:
  ~X11Capture() override { stop(); }
  std::vector<Monitor> monitors(std::string& error) override {
    std::lock_guard<std::mutex> l(mutex_);
    return monitorsLocked(error);
  }
  std::vector<CaptureWindow> windows(std::string& error) override {
    std::lock_guard<std::mutex> l(mutex_);
    return windowsLocked(error);
  }
  bool start(const SourceOptions& source, ErrorCallback cb) override {
    if (source.captureMode == CaptureMode::Window) {
      std::lock_guard<std::mutex> l(mutex_);
      std::string e;
      if (!connect(e)) {
        if (cb) cb({"video", e, "Log into an Xorg session and set DISPLAY."});
        return false;
      }
      if (!source.window || !source.window->id) {
        if (cb)
          cb({"video", "no window was selected",
              "Choose an open, visible window and try again."});
        return false;
      }
      selectedWindow_ = source.window->id;
      selected_.name = source.window->title;
      windowMode_ = true;
      windowViewable_ = false;
      error_ = std::move(cb);
      if (!recoverLocked()) {
        if (error_)
          error_({"video", "selected window is unavailable",
                  "Choose an open, visible window and try again."});
        return false;
      }
      region_ = {0, 0, selected_.width, selected_.height};
      consecutiveFailures_ = 0;
      windowGeometryPoll_ = 0;
      running_ = true;
      return true;
    }
    const auto& name = source.monitor;
    std::string e;
    auto ms = monitors(e);
    if (ms.empty()) {
      if (cb) cb({"video", e, "Log into an Xorg session and set DISPLAY."});
      return false;
    }
    auto it = name.empty() ? std::find_if(ms.begin(), ms.end(),
                                          [](auto& m) { return m.primary; })
                           : std::find_if(ms.begin(), ms.end(), [&](auto& m) {
                               return m.name == name;
                             });
    if (it == ms.end()) {
      if (cb)
        cb({"video", "selected monitor is unavailable",
            "Run 'mistercast list-monitors'."});
      return false;
    }
    selected_ = *it;
    drawable_ = screen_->root;
    selectedWindow_ = 0;
    windowMode_ = false;
    windowViewable_ = false;
    region_ = {0, 0, selected_.width, selected_.height};
    error_ = std::move(cb);
    resolvePixelFormat(screen_->root_depth);
    setupShm();
    consecutiveFailures_ = 0;
    running_ = true;
    return true;
  }
  SourceGeometry selectedGeometry() const override {
    std::lock_guard<std::mutex> l(mutex_);
    return {selected_.width, selected_.height};
  }
  void setRegion(const CropRect& region) override {
    std::lock_guard<std::mutex> l(mutex_);
    region_ = clampRegion(region);
  }
  bool next(Frame& out, std::chrono::milliseconds timeout) override {
    (void)timeout;
    if (!running_) return false;
    std::lock_guard<std::mutex> l(mutex_);
    // A growing window would otherwise keep producing the old-sized region
    // without an X error. Poll at a low rate so resize tracking does not add a
    // synchronous X11 round-trip to every captured frame.
    if (windowMode_ && ++windowGeometryPoll_ >= 30) {
      windowGeometryPoll_ = 0;
      if (!recoverLocked()) return captureFailedLocked();
    }
    if (captureLocked(out)) {
      consecutiveFailures_ = 0;
      out.sequence = ++sequence_;
      return true;
    }
    // A resize, rotation, replug, or X server restart invalidates the cached
    // geometry and the shared segment. Recover and let the caller retry, so a
    // transient fault costs frames instead of the whole session; only a fault
    // that persists for roughly two seconds is reported as fatal.
    recoverLocked();
    return captureFailedLocked();
  }
  bool captureFailedLocked() {
    if (++consecutiveFailures_ >= kMaxConsecutiveFailures) {
      running_ = false;
      if (error_)
        error_({"video", "X11 capture kept failing",
                windowMode_ ? "Restore or reselect the shared window."
                            : "Check the monitor selection and X11 session."});
    }
    return false;
  }
  bool captureLocked(Frame& out) {
    if (!connection_ || xcb_connection_has_error(connection_)) return false;
    xcb_generic_error_t* xe = nullptr;
    uint8_t* data = nullptr;
    size_t len = 0;
    uint8_t depth = depth_;
    xcb_get_image_reply_t* normal = nullptr;
    xcb_shm_get_image_reply_t* shared = nullptr;
    // Only the crop region is transferred. Pulling the whole monitor and
    // cropping afterwards cost 6 ms per 4K frame where a small crop costs
    // microseconds, and that cost sat in front of every blit.
    const int16_t x = int16_t((windowMode_ ? 0 : selected_.x) + region_.x);
    const int16_t y = int16_t((windowMode_ ? 0 : selected_.y) + region_.y);
    const uint16_t width = uint16_t(region_.width);
    const uint16_t height = uint16_t(region_.height);
    if (useShm_) {
      auto ck =
          xcb_shm_get_image(connection_, drawable_, x, y, width, height,
                            ~0u, XCB_IMAGE_FORMAT_Z_PIXMAP, shmSeg_, 0);
      shared = xcb_shm_get_image_reply(connection_, ck, &xe);
      if (shared) {
        depth = shared->depth;
        len = std::min<size_t>(shared->size, shmSize_);
        data = shmData_;
        shmFailures_ = 0;
      } else {
        free(xe);
        xe = nullptr;
        // One failed request does not mean MIT-SHM is unusable: a stale region
        // after a resize fails the same way. Fall back for this frame and only
        // settle on the slow path once it keeps failing.
        if (++shmFailures_ >= kMaxShmFailures) closeShm();
      }
    }
    if (!data) {
      auto ck = xcb_get_image(connection_, XCB_IMAGE_FORMAT_Z_PIXMAP,
                              drawable_, x, y, width, height, ~0u);
      normal = xcb_get_image_reply(connection_, ck, &xe);
      if (normal) {
        depth = normal->depth;
        len = xcb_get_image_data_length(normal);
        data = xcb_get_image_data(normal);
      }
    }
    if (!data) {
      free(xe);
      free(shared);
      free(normal);
      return false;
    }
    if (depth != depth_) resolvePixelFormat(depth);
    uint32_t stride = height ? uint32_t(len) / height : 0;
    std::string e;
    bool ok =
        normalizeToBgra(data, len, width, height, stride, bitsPerPixel_,
                        redMask_, greenMask_, blueMask_, lsbFirst_, out, e);
    free(shared);
    free(normal);
    return ok;
  }
  void stop() noexcept override {
    running_ = false;
    std::lock_guard<std::mutex> l(mutex_);
    close();
  }
};
std::unique_ptr<IVideoCapture> makeX11Capture() {
  return std::make_unique<X11Capture>();
}
}  // namespace mistercast
