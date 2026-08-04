#pragma once

#include <xcb/xcb.h>

#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace mistercast::test {

// Round-trips a request, so everything queued before it has been processed by the
// server before the test looks at the result.
inline void sync(xcb_connection_t* connection) {
  free(xcb_get_input_focus_reply(connection, xcb_get_input_focus(connection),
                                 nullptr));
}

// A mapped window with a legacy WM_NAME title. Xvfb has no window manager and no
// other clients, so any test that needs something capturable has to create it.
inline xcb_window_t createWindow(xcb_connection_t* connection,
                                 xcb_screen_t* screen, uint16_t width,
                                 uint16_t height, std::string_view title,
                                 bool map = true) {
  const auto window = xcb_generate_id(connection);
  const uint32_t background[] = {screen->black_pixel};
  xcb_create_window(connection, screen->root_depth, window, screen->root, 0, 0,
                    width, height, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                    screen->root_visual, XCB_CW_BACK_PIXEL, background);
  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window,
                      XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, title.size(),
                      title.data());
  if (map) xcb_map_window(connection, window);
  return window;
}

inline void fill(xcb_connection_t* connection, xcb_drawable_t drawable,
                 xcb_gcontext_t gc, uint32_t color, uint16_t width,
                 uint16_t height) {
  xcb_change_gc(connection, gc, XCB_GC_FOREGROUND, &color);
  const xcb_rectangle_t rectangle{0, 0, width, height};
  xcb_poly_fill_rectangle(connection, drawable, gc, 1, &rectangle);
  sync(connection);
}

// A set of windows on their own connection, for the window chooser to find.
class ForeignWindows {
 public:
  explicit ForeignWindows(const std::vector<std::string>& titles) {
    connection_ = xcb_connect(nullptr, nullptr);
    if (!connection_ || xcb_connection_has_error(connection_)) return;
    auto* screen = xcb_setup_roots_iterator(xcb_get_setup(connection_)).data;
    if (!screen) return;
    for (const auto& title : titles)
      windows_.push_back(createWindow(connection_, screen, 64, 48, title));
    sync(connection_);
    ready_ = true;
  }

  ~ForeignWindows() {
    if (!connection_) return;
    for (const auto window : windows_) xcb_destroy_window(connection_, window);
    xcb_disconnect(connection_);
  }

  ForeignWindows(const ForeignWindows&) = delete;
  ForeignWindows& operator=(const ForeignWindows&) = delete;

  bool ready() const noexcept { return ready_; }
  uint32_t first() const noexcept {
    return windows_.empty() ? 0 : windows_.front();
  }

 private:
  xcb_connection_t* connection_{};
  std::vector<xcb_window_t> windows_;
  bool ready_{false};
};

}  // namespace mistercast::test
