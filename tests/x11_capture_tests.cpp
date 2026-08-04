#include <xcb/composite.h>
#include <xcb/xcb.h>

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

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

xcb_atom_t atom(xcb_connection_t* connection, std::string_view name) {
  auto* reply = xcb_intern_atom_reply(
      connection,
      xcb_intern_atom(connection, 0, uint16_t(name.size()), name.data()),
      nullptr);
  const auto result = reply ? reply->atom : xcb_atom_t{XCB_ATOM_NONE};
  free(reply);
  return result;
}

xcb_window_t createWindow(xcb_connection_t* connection, xcb_screen_t* screen,
                          uint16_t width, uint16_t height,
                          std::string_view title, bool map = true) {
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

void fill(xcb_connection_t* connection, xcb_drawable_t drawable,
          xcb_gcontext_t gc, uint32_t color, uint16_t width, uint16_t height) {
  xcb_change_gc(connection, gc, XCB_GC_FOREGROUND, &color);
  const xcb_rectangle_t rectangle{0, 0, width, height};
  xcb_poly_fill_rectangle(connection, drawable, gc, 1, &rectangle);
  sync(connection);
}

void checkCatalog(xcb_connection_t* connection, xcb_screen_t* screen) {
  const auto visible =
      createWindow(connection, screen, 40, 30, "catalog-visible");
  const auto own = createWindow(connection, screen, 20, 20, "catalog-own");
  const auto hidden =
      createWindow(connection, screen, 20, 20, "catalog-hidden", false);
  const uint32_t pid = uint32_t(getpid());
  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, own,
                      atom(connection, "_NET_WM_PID"), XCB_ATOM_CARDINAL, 32, 1,
                      &pid);
  const xcb_window_t clients[] = {visible, own, hidden};
  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, screen->root,
                      atom(connection, "_NET_CLIENT_LIST_STACKING"),
                      XCB_ATOM_WINDOW, 32, 3, clients);
  sync(connection);

  std::string error;
  const auto windows = x11CaptureWindows(error);
  CHECK(error.empty());
  const auto contains = [&](std::string_view title) {
    return std::any_of(windows.begin(), windows.end(), [&](const auto& window) {
      return window.title == title;
    });
  };
  CHECK(contains("catalog-visible"));
  CHECK(!contains("catalog-own"));
  CHECK(!contains("catalog-hidden"));

  xcb_delete_property(connection, screen->root,
                      atom(connection, "_NET_CLIENT_LIST_STACKING"));
  for (const auto window : clients) xcb_destroy_window(connection, window);
  sync(connection);
}

void checkDirectFallbackAndResize(xcb_connection_t* connection,
                                  xcb_screen_t* screen) {
  const auto window =
      createWindow(connection, screen, 32, 32, "direct-fallback");
  const auto gc = xcb_generate_id(connection);
  xcb_create_gc(connection, gc, window, 0, nullptr);
  fill(connection, window, gc, 0x00ff0000, 32, 32);

  auto capture = makeX11Capture({false, false});
  const WindowCaptureSource source{window};
  CHECK(capture->start(source, [](SessionError error) {
    std::cerr << error.component << ": " << error.message << '\n';
  }));
  capture->setRegion({0, 0, 32, 32});
  Frame frame;
  CHECK(capture->next(frame, std::chrono::milliseconds(0)));
  CHECK(frame.bgra.size() >= 4 && frame.bgra[0] == 0 && frame.bgra[1] == 0 &&
        frame.bgra[2] == 255);

  const uint32_t size[] = {48, 24};
  xcb_configure_window(connection, window,
                       XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, size);
  fill(connection, window, gc, 0x000000ff, 48, 24);
  for (int attempt = 0; attempt < 30; ++attempt)
    capture->next(frame, std::chrono::milliseconds(0));
  const auto resized = capture->selectedGeometry();
  CHECK(resized.width == 48 && resized.height == 24);
  capture->setRegion({0, 0, 48, 24});
  CHECK(capture->next(frame, std::chrono::milliseconds(0)));
  CHECK(frame.width == 48 && frame.height == 24 && frame.bgra[0] == 255 &&
        frame.bgra[1] == 0 && frame.bgra[2] == 0);

  capture->stop();
  xcb_free_gc(connection, gc);
  xcb_destroy_window(connection, window);
  sync(connection);
}

void checkCompositeLifecycle(xcb_connection_t* connection,
                             xcb_screen_t* screen) {
  const auto* composite = xcb_get_extension_data(connection, &xcb_composite_id);
  if (!composite || !composite->present) return;
  auto* version = xcb_composite_query_version_reply(
      connection, xcb_composite_query_version(connection, 0, 4), nullptr);
  const bool available =
      version && (version->major_version > 0 || version->minor_version >= 2);
  free(version);
  if (!available) return;

  const auto window = createWindow(connection, screen, 32, 32, "composite");
  auto* redirectError = xcb_request_check(
      connection, xcb_composite_redirect_window_checked(
                      connection, window, XCB_COMPOSITE_REDIRECT_MANUAL));
  CHECK(!redirectError);
  free(redirectError);
  const auto gc = xcb_generate_id(connection);
  xcb_create_gc(connection, gc, window, 0, nullptr);
  fill(connection, window, gc, 0x00ff0000, 32, 32);

  auto capture = makeX11Capture({false, true});
  CHECK(capture->start(WindowCaptureSource{window},
                       [](SessionError error) {
                         std::cerr << error.component << ": " << error.message
                                   << '\n';
                       }));
  capture->setRegion({0, 0, 32, 32});
  Frame frame;
  CHECK(capture->next(frame, std::chrono::milliseconds(0)));
  xcb_unmap_window(connection, window);
  sync(connection);
  CHECK(capture->next(frame, std::chrono::milliseconds(0)));
  bool rejectedStalePixmap = false;
  for (int attempt = 0; attempt < 120 && !rejectedStalePixmap; ++attempt)
    rejectedStalePixmap =
        !capture->next(frame, std::chrono::milliseconds(0));
  CHECK(rejectedStalePixmap);

  xcb_map_window(connection, window);
  fill(connection, window, gc, 0x000000ff, 32, 32);
  CHECK(capture->next(frame, std::chrono::milliseconds(0)));
  // Reach the periodic refresh that must bind a fresh Composite pixmap for the
  // remapped window. Without this, the first frame can use the raw fallback.
  for (int attempt = 0; attempt < 29; ++attempt)
    CHECK(capture->next(frame, std::chrono::milliseconds(0)));
  CHECK(frame.bgra.size() >= 4 && frame.bgra[0] == 255 && frame.bgra[1] == 0 &&
        frame.bgra[2] == 0);

  // A named pixmap remains readable immediately after unmap. The raw-window
  // fallback cannot satisfy this assertion; periodic refresh later rejects it.
  xcb_unmap_window(connection, window);
  sync(connection);
  CHECK(capture->next(frame, std::chrono::milliseconds(0)));
  rejectedStalePixmap = false;
  for (int attempt = 0; attempt < 120 && !rejectedStalePixmap; ++attempt)
    rejectedStalePixmap =
        !capture->next(frame, std::chrono::milliseconds(0));
  CHECK(rejectedStalePixmap);
  capture->stop();
  xcb_free_gc(connection, gc);
  xcb_destroy_window(connection, window);
  sync(connection);
}

void checkConnectionLossExpiresWindow() {
  int displayPipe[2];
  CHECK(pipe(displayPipe) == 0);
  const auto server = fork();
  CHECK(server >= 0);
  if (server < 0) {
    close(displayPipe[0]);
    close(displayPipe[1]);
    return;
  }
  if (server == 0) {
    close(displayPipe[0]);
    const auto descriptor = std::to_string(displayPipe[1]);
    execlp("Xvfb", "Xvfb", "-displayfd", descriptor.c_str(), "-screen", "0",
           "64x64x24", "-nolisten", "tcp", nullptr);
    _exit(127);
  }
  close(displayPipe[1]);
  char number[16]{};
  const auto length = read(displayPipe[0], number, sizeof(number) - 1);
  close(displayPipe[0]);
  CHECK(length > 0);
  if (length <= 0) {
    waitpid(server, nullptr, 0);
    return;
  }
  number[length] = '\0';
  std::string nestedDisplay = ":" + std::string(number);
  while (!nestedDisplay.empty() &&
         (nestedDisplay.back() == '\n' || nestedDisplay.back() == '\r'))
    nestedDisplay.pop_back();
  const char* currentDisplay = std::getenv("DISPLAY");
  const std::string originalDisplay = currentDisplay ? currentDisplay : "";
  setenv("DISPLAY", nestedDisplay.c_str(), 1);

  auto* connection = xcb_connect(nullptr, nullptr);
  CHECK(connection && !xcb_connection_has_error(connection));
  if (connection && !xcb_connection_has_error(connection)) {
    auto* screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
    const auto window = createWindow(connection, screen, 32, 32, "expires");
    const auto gc = xcb_generate_id(connection);
    xcb_create_gc(connection, gc, window, 0, nullptr);
    fill(connection, window, gc, 0x00ff0000, 32, 32);
    auto capture = makeX11Capture({false, false});
    CHECK(capture->start(WindowCaptureSource{window}, {}));
    capture->setRegion({0, 0, 32, 32});
    Frame frame;
    CHECK(capture->next(frame, std::chrono::milliseconds(0)));

    kill(server, SIGTERM);
    waitpid(server, nullptr, 0);

    const auto replacementServer = fork();
    CHECK(replacementServer >= 0);
    if (replacementServer == 0) {
      execlp("Xvfb", "Xvfb", nestedDisplay.c_str(), "-screen", "0",
             "64x64x24", "-nolisten", "tcp", nullptr);
      _exit(127);
    }
    xcb_connection_t* replacementConnection = nullptr;
    for (int attempt = 0; attempt < 100; ++attempt) {
      replacementConnection = xcb_connect(nullptr, nullptr);
      if (replacementConnection &&
          !xcb_connection_has_error(replacementConnection))
        break;
      if (replacementConnection) xcb_disconnect(replacementConnection);
      replacementConnection = nullptr;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(replacementConnection);
    if (replacementConnection) {
      auto* replacementScreen =
          xcb_setup_roots_iterator(xcb_get_setup(replacementConnection)).data;
      const auto replacementWindow = createWindow(
          replacementConnection, replacementScreen, 48, 24, "replacement");
      CHECK(replacementWindow == window);
      const auto replacementGc = xcb_generate_id(replacementConnection);
      xcb_create_gc(replacementConnection, replacementGc, replacementWindow, 0,
                    nullptr);
      fill(replacementConnection, replacementWindow, replacementGc, 0x0000ff00,
           48, 24);
    }

    CHECK(!capture->next(frame, std::chrono::milliseconds(0)));
    // The replacement server deliberately reused the numeric ID for different
    // content and geometry. Every later attempt must stay terminal.
    for (int attempt = 0; attempt < 4; ++attempt)
      CHECK(!capture->next(frame, std::chrono::milliseconds(0)));
    const auto geometry = capture->selectedGeometry();
    CHECK(geometry.width == 32 && geometry.height == 32);
    capture->stop();
    xcb_disconnect(connection);
    if (replacementConnection) xcb_disconnect(replacementConnection);
    if (replacementServer > 0) {
      kill(replacementServer, SIGTERM);
      waitpid(replacementServer, nullptr, 0);
    }
  } else {
    kill(server, SIGTERM);
    waitpid(server, nullptr, 0);
    if (connection) xcb_disconnect(connection);
  }
  if (originalDisplay.empty())
    unsetenv("DISPLAY");
  else
    setenv("DISPLAY", originalDisplay.c_str(), 1);
}
}  // namespace

int main() {
  auto* connection = xcb_connect(nullptr, nullptr);
  if (!connection || xcb_connection_has_error(connection)) {
    std::cerr << "X11 capture test skipped: DISPLAY is unavailable\n";
    if (connection) xcb_disconnect(connection);
    return 77;
  }
  auto* screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
  if (!screen) {
    xcb_disconnect(connection);
    return 77;
  }
  checkCatalog(connection, screen);
  checkDirectFallbackAndResize(connection, screen);
  checkCompositeLifecycle(connection, screen);
  checkConnectionLossExpiresWindow();
  xcb_disconnect(connection);
  return failures ? 1 : 0;
}
