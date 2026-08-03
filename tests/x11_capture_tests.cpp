#include <xcb/composite.h>
#include <xcb/xcb.h>

#include <cstdlib>
#include <iostream>

#include "mistercast/interfaces.hpp"

using namespace mistercast;

namespace {
int failures;
#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << __FILE__ << ':' << __LINE__ << ": check failed: "          \
                << #condition << '\n';                                         \
      ++failures;                                                              \
    }                                                                          \
  } while (false)

void sync(xcb_connection_t* connection) {
  free(xcb_get_input_focus_reply(connection, xcb_get_input_focus(connection),
                                 nullptr));
}

void fill(xcb_connection_t* connection, xcb_drawable_t drawable, xcb_gcontext_t gc,
          uint32_t color) {
  xcb_change_gc(connection, gc, XCB_GC_FOREGROUND, &color);
  const xcb_rectangle_t rectangle{0, 0, 32, 32};
  xcb_poly_fill_rectangle(connection, drawable, gc, 1, &rectangle);
  sync(connection);
}
}  // namespace

int main() {
  auto* connection = xcb_connect(nullptr, nullptr);
  if (!connection || xcb_connection_has_error(connection)) {
    std::cerr << "X11 capture test skipped: DISPLAY is unavailable\n";
    if (connection) xcb_disconnect(connection);
    return 0;
  }
  auto* screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
  const auto* composite =
      xcb_get_extension_data(connection, &xcb_composite_id);
  if (!screen || !composite || !composite->present) {
    std::cerr << "X11 capture test skipped: XComposite is unavailable\n";
    xcb_disconnect(connection);
    return 0;
  }
  auto* version = xcb_composite_query_version_reply(
      connection, xcb_composite_query_version(connection, 0, 4), nullptr);
  if (!version || (version->major_version == 0 && version->minor_version < 2)) {
    std::cerr << "X11 capture test skipped: XComposite 0.2 is unavailable\n";
    free(version);
    xcb_disconnect(connection);
    return 0;
  }
  free(version);

  const auto window = xcb_generate_id(connection);
  const uint32_t background[] = {screen->black_pixel};
  xcb_create_window(connection, screen->root_depth, window, screen->root, 0, 0,
                    32, 32, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                    screen->root_visual, XCB_CW_BACK_PIXEL, background);
  auto* redirectError = xcb_request_check(
      connection, xcb_composite_redirect_window_checked(
                      connection, window, XCB_COMPOSITE_REDIRECT_MANUAL));
  CHECK(!redirectError);
  free(redirectError);
  xcb_map_window(connection, window);
  const auto gc = xcb_generate_id(connection);
  const uint32_t foreground[] = {0x00ff0000};
  xcb_create_gc(connection, gc, window, XCB_GC_FOREGROUND, foreground);
  fill(connection, window, gc, 0x00ff0000);

  auto capture = makeX11Capture();
  SourceOptions source;
  source.captureMode = CaptureMode::Window;
  source.window = CaptureWindow{window, "lifecycle-test", 32, 32};
  CHECK(capture->start(source, [](SessionError error) {
    std::cerr << error.component << ": " << error.message << '\n';
  }));
  capture->setRegion({0, 0, 32, 32});
  Frame frame;
  CHECK(capture->next(frame, std::chrono::milliseconds(0)));
  CHECK(frame.bgra.size() >= 4 && frame.bgra[0] == 0 && frame.bgra[1] == 0 &&
        frame.bgra[2] == 255);

  // A named Composite pixmap remains readable after unmap. Capture must still
  // reject it once the periodic source refresh observes that it is stale.
  xcb_unmap_window(connection, window);
  sync(connection);
  for (int frameIndex = 1; frameIndex < 29; ++frameIndex)
    CHECK(capture->next(frame, std::chrono::milliseconds(0)));
  CHECK(!capture->next(frame, std::chrono::milliseconds(0)));

  // Mapping at the same dimensions allocates new Composite backing storage.
  // Recovery must name that new pixmap rather than resume the stale one.
  xcb_map_window(connection, window);
  fill(connection, window, gc, 0x000000ff);
  CHECK(!capture->next(frame, std::chrono::milliseconds(0)));
  CHECK(capture->next(frame, std::chrono::milliseconds(0)));
  CHECK(frame.bgra.size() >= 4 && frame.bgra[0] == 255 && frame.bgra[1] == 0 &&
        frame.bgra[2] == 0);

  capture->stop();
  xcb_disconnect(connection);
  return failures ? 1 : 0;
}
