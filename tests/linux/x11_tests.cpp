#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <xcb/composite.h>
#include <xcb/xcb.h>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "linux/x11_display.hpp"
#include "linux/x11_source_catalog.hpp"
#include "mistercast/interfaces.hpp"
#include "support/nested_xvfb.hpp"
#include "support/temp_directory.hpp"

using namespace mistercast;
using mistercast::test::NestedXvfb;
using mistercast::test::ScopedEnvironment;
using testing::HasSubstr;

namespace {

// The suite runs under an X server (xvfb-run in CI), so these helpers create
// real windows on it and capture them through the production code path rather
// than substituting a fake X server.
//
// Xvfb, unlike a real Xorg server, intermittently refuses a connection when a
// process opens several in quick succession (measured at roughly one in four
// with no pause, and never with a 20 ms gap). Production makes one connection
// per capture session and correctly reports a failure as actionable, so the
// retries below belong to the harness, not to the code under test.
constexpr int kConnectAttempts = 6;
constexpr auto kConnectPause = std::chrono::milliseconds(25);

xcb_connection_t* connectForTest() {
  for (int attempt = 0; attempt < kConnectAttempts; ++attempt) {
    if (attempt) std::this_thread::sleep_for(kConnectPause);
    auto* candidate = xcb_connect(nullptr, nullptr);
    if (candidate && !xcb_connection_has_error(candidate)) return candidate;
    if (candidate) xcb_disconnect(candidate);
  }
  return nullptr;
}

bool connectForTest(X11DisplayConnection& connection, std::string& error) {
  for (int attempt = 0; attempt < kConnectAttempts; ++attempt) {
    if (attempt) std::this_thread::sleep_for(kConnectPause);
    if (connection.connect(error)) return true;
  }
  return false;
}

// Starting a capture opens a connection too, so the same allowance applies.
bool startForTest(IVideoCapture& capture, const CaptureSource& source,
                  ErrorCallback callback = {}) {
  for (int attempt = 0; attempt < kConnectAttempts; ++attempt) {
    if (attempt) std::this_thread::sleep_for(kConnectPause);
    if (capture.start(source, callback)) return true;
  }
  return false;
}
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

bool compositeNamedPixmapsAvailable(xcb_connection_t* connection) {
  const auto* composite = xcb_get_extension_data(connection, &xcb_composite_id);
  if (!composite || !composite->present) return false;
  auto* version = xcb_composite_query_version_reply(
      connection, xcb_composite_query_version(connection, 0, 4), nullptr);
  const bool available =
      version && (version->major_version > 0 || version->minor_version >= 2);
  free(version);
  return available;
}

// Owns one connection and the windows a test created on it.
class X11 : public testing::Test {
 protected:
  void SetUp() override {
    connection = connectForTest();
    ASSERT_NE(connection, nullptr);
    screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
    ASSERT_NE(screen, nullptr);
  }

  void TearDown() override {
    for (const auto gc : contexts) xcb_free_gc(connection, gc);
    for (const auto window : windows) xcb_destroy_window(connection, window);
    if (connection) {
      sync(connection);
      xcb_disconnect(connection);
    }
  }

  xcb_window_t window(uint16_t width, uint16_t height, std::string_view title,
                      bool map = true) {
    const auto id = createWindow(connection, screen, width, height, title, map);
    windows.push_back(id);
    return id;
  }

  xcb_gcontext_t context(xcb_drawable_t drawable) {
    const auto gc = xcb_generate_id(connection);
    xcb_create_gc(connection, gc, drawable, 0, nullptr);
    contexts.push_back(gc);
    return gc;
  }

  xcb_connection_t* connection{};
  xcb_screen_t* screen{};
  std::vector<xcb_window_t> windows;
  std::vector<xcb_gcontext_t> contexts;
};

// ------------------------------------------------------- display connection

TEST(X11Display, RefusesToConnectWithoutADisplayVariable) {
  const ScopedEnvironment display("DISPLAY", nullptr);
  X11DisplayConnection connection;
  std::string error;
  EXPECT_FALSE(connection.connect(error));
  EXPECT_THAT(error, HasSubstr("DISPLAY is not set"));
  EXPECT_EQ(connection.connection(), nullptr);
  EXPECT_FALSE(connection.lost());

  const ScopedEnvironment empty("DISPLAY", "");
  error.clear();
  EXPECT_FALSE(connection.connect(error));
  EXPECT_THAT(error, HasSubstr("DISPLAY is not set"));
}

TEST(X11Display, ExplainsThatNativeWaylandIsNotSupported) {
  const ScopedEnvironment display("DISPLAY", ":99999");
  X11DisplayConnection connection;
  std::string error;
  EXPECT_FALSE(connection.connect(error));
  EXPECT_THAT(error, HasSubstr("native Wayland capture is not supported"));
  EXPECT_EQ(connection.screen(), nullptr);
}

TEST(X11Display, ConnectsOnceAndIsReusable) {
  X11DisplayConnection connection;
  std::string error;
  ASSERT_TRUE(connectForTest(connection, error)) << error;
  auto* first = connection.connection();
  ASSERT_NE(first, nullptr);
  EXPECT_NE(connection.screen(), nullptr);
  EXPECT_FALSE(connection.lost());

  EXPECT_TRUE(connection.connect(error));
  EXPECT_EQ(connection.connection(), first) << "an open connection is reused";

  connection.reset();
  EXPECT_EQ(connection.connection(), nullptr);
  EXPECT_EQ(connection.screen(), nullptr);
  connection.reset() /* idempotent */;
  ASSERT_TRUE(connectForTest(connection, error)) << error;
  EXPECT_NE(connection.connection(), nullptr);
}

TEST(X11Display, IsMovableSoTheOwningCaptureCanBeRelocated) {
  X11DisplayConnection source;
  std::string error;
  ASSERT_TRUE(connectForTest(source, error)) << error;
  auto* raw = source.connection();

  X11DisplayConnection moved(std::move(source));
  EXPECT_EQ(moved.connection(), raw);
  EXPECT_EQ(source.connection(), nullptr);

  X11DisplayConnection assigned;
  ASSERT_TRUE(connectForTest(assigned, error)) << error;
  assigned = std::move(moved);
  EXPECT_EQ(assigned.connection(), raw);
  EXPECT_EQ(moved.connection(), nullptr);

  // Self-assignment must not close the connection it is holding.
  auto& alias = assigned;
  assigned = std::move(alias);
  EXPECT_EQ(assigned.connection(), raw);
}

TEST_F(X11, EnumeratesAtLeastOneMonitorForTheScreen) {
  const auto monitors = enumerateX11Monitors(connection, screen);
  ASSERT_FALSE(monitors.empty());
  // A server may flag one monitor primary or none at all (Xvfb flags none), but
  // never more than one.
  EXPECT_LE(std::count_if(monitors.begin(), monitors.end(),
                          [](const Monitor& monitor) { return monitor.primary; }),
            1);
  for (const auto& monitor : monitors) {
    EXPECT_FALSE(monitor.name.empty());
    EXPECT_GT(monitor.width, 0);
    EXPECT_GT(monitor.height, 0);
  }
}

TEST(X11Monitors, ReportTheDisplayProblemRatherThanAnEmptyList) {
  const ScopedEnvironment display("DISPLAY", nullptr);
  std::string error;
  EXPECT_TRUE(x11Monitors(error).empty());
  EXPECT_THAT(error, HasSubstr("DISPLAY"));

  error.clear();
  EXPECT_TRUE(x11CaptureWindows(error).empty());
  EXPECT_THAT(error, HasSubstr("DISPLAY"));
}

TEST_F(X11, ListsMonitorsThroughThePublicEntryPoint) {
  std::string error;
  const auto monitors = x11Monitors(error);
  EXPECT_THAT(error, testing::IsEmpty());
  EXPECT_FALSE(monitors.empty());
}

// -------------------------------------------------------------- window catalog

TEST_F(X11, OffersOnlyVisibleForeignTitledWindowsForCapture) {
  const auto visible = window(40, 30, "catalog-visible");
  const auto own = window(20, 20, "catalog-own");
  const auto hidden = window(20, 20, "catalog-hidden", /*map=*/false);
  const auto untitled = window(20, 20, "");
  const uint32_t pid = uint32_t(getpid());
  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, own,
                      atom(connection, "_NET_WM_PID"), XCB_ATOM_CARDINAL, 32, 1,
                      &pid);
  const xcb_window_t clients[] = {visible, own, hidden, untitled};
  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, screen->root,
                      atom(connection, "_NET_CLIENT_LIST_STACKING"),
                      XCB_ATOM_WINDOW, 32, 4, clients);
  sync(connection);

  std::string error;
  const auto candidates = x11CaptureWindows(error);
  EXPECT_THAT(error, testing::IsEmpty());
  std::vector<std::string> titles;
  for (const auto& candidate : candidates) titles.push_back(candidate.title);

  EXPECT_THAT(titles, testing::Contains("catalog-visible"));
  EXPECT_THAT(titles, testing::Not(testing::Contains("catalog-own")))
      << "MiSTerCast must not offer to capture itself";
  EXPECT_THAT(titles, testing::Not(testing::Contains("catalog-hidden")))
      << "a minimized or unmapped window cannot be captured";
  EXPECT_THAT(titles, testing::Not(testing::Contains("")));
  EXPECT_TRUE(std::is_sorted(titles.begin(), titles.end()))
      << "the chooser presents windows in title order";

  for (const auto& candidate : candidates)
    if (candidate.title == "catalog-visible") {
      EXPECT_EQ(candidate.id, visible);
      EXPECT_EQ(candidate.width, 40);
      EXPECT_EQ(candidate.height, 30);
    }

  xcb_delete_property(connection, screen->root,
                      atom(connection, "_NET_CLIENT_LIST_STACKING"));
  sync(connection);
}

TEST_F(X11, AnEmptyUtf8TitleFallsBackToTheLegacyName) {
  const auto target = window(40, 24, "legacy-fallback");
  // A zero-length _NET_WM_NAME is present but unusable, so WM_NAME has to win.
  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, target,
                      atom(connection, "_NET_WM_NAME"),
                      atom(connection, "UTF8_STRING"), 8, 0, nullptr);
  const xcb_window_t clients[] = {target};
  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, screen->root,
                      atom(connection, "_NET_CLIENT_LIST_STACKING"),
                      XCB_ATOM_WINDOW, 32, 1, clients);
  sync(connection);

  std::string error;
  std::vector<std::string> titles;
  for (const auto& candidate : x11CaptureWindows(error))
    titles.push_back(candidate.title);
  EXPECT_THAT(titles, testing::Contains("legacy-fallback"));

  xcb_delete_property(connection, screen->root,
                      atom(connection, "_NET_CLIENT_LIST_STACKING"));
  sync(connection);
}

TEST_F(X11, FallsBackToTheWindowTreeWhenTheManagerListIsEmpty) {
  const auto target = window(56, 40, "empty-list-fallback");
  // The property exists but names no windows, which is what a window manager
  // publishes before it has adopted anything.
  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, screen->root,
                      atom(connection, "_NET_CLIENT_LIST_STACKING"),
                      XCB_ATOM_WINDOW, 32, 0, nullptr);
  sync(connection);

  std::string error;
  std::vector<std::string> titles;
  for (const auto& candidate : x11CaptureWindows(error))
    titles.push_back(candidate.title);
  EXPECT_THAT(error, testing::IsEmpty());
  EXPECT_THAT(titles, testing::Contains("empty-list-fallback"));
  (void)target;

  xcb_delete_property(connection, screen->root,
                      atom(connection, "_NET_CLIENT_LIST_STACKING"));
  sync(connection);
}

TEST_F(X11, PrefersTheUtf8WindowTitleOverTheLegacyOne) {
  const auto target = window(32, 32, "legacy-name");
  const std::string modern = "modern-name";
  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, target,
                      atom(connection, "_NET_WM_NAME"),
                      atom(connection, "UTF8_STRING"), 8, modern.size(),
                      modern.data());
  const xcb_window_t clients[] = {target};
  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, screen->root,
                      atom(connection, "_NET_CLIENT_LIST_STACKING"),
                      XCB_ATOM_WINDOW, 32, 1, clients);
  sync(connection);

  std::string error;
  std::vector<std::string> titles;
  for (const auto& candidate : x11CaptureWindows(error))
    titles.push_back(candidate.title);
  EXPECT_THAT(titles, testing::Contains(modern));
  EXPECT_THAT(titles, testing::Not(testing::Contains("legacy-name")));

  xcb_delete_property(connection, screen->root,
                      atom(connection, "_NET_CLIENT_LIST_STACKING"));
  sync(connection);
}

TEST_F(X11, FallsBackToTheWindowTreeWithoutAWindowManagerHint) {
  const auto target = window(48, 24, "tree-walked");
  // No _NET_CLIENT_LIST_STACKING at all: the tree has to be queried instead.
  xcb_delete_property(connection, screen->root,
                      atom(connection, "_NET_CLIENT_LIST_STACKING"));
  sync(connection);

  std::string error;
  std::vector<std::string> titles;
  for (const auto& candidate : x11CaptureWindows(error))
    titles.push_back(candidate.title);
  EXPECT_THAT(error, testing::IsEmpty());
  EXPECT_THAT(titles, testing::Contains("tree-walked"));
  (void)target;
}

TEST(X11Display, SelectsTheScreenNamedByTheDisplayVariable) {
  // Two screens of one server, so the screen index in DISPLAY has something to
  // choose between. Xorg calls these separate screens; RandR monitor names are a
  // different concept entirely.
  NestedXvfb nested({"-screen", "0", "64x64x24", "-screen", "1", "48x32x24"});
  if (!nested.ready()) GTEST_SKIP() << "a nested Xvfb could not be started";

  {
    const ScopedEnvironment first("DISPLAY", (nested.display() + ".0").c_str());
    X11DisplayConnection connection;
    std::string error;
    ASSERT_TRUE(connectForTest(connection, error)) << error;
    ASSERT_NE(connection.screen(), nullptr);
    EXPECT_EQ(connection.screen()->width_in_pixels, 64);
  }
  {
    const ScopedEnvironment second("DISPLAY", (nested.display() + ".1").c_str());
    X11DisplayConnection connection;
    std::string error;
    ASSERT_TRUE(connectForTest(connection, error)) << error;
    ASSERT_NE(connection.screen(), nullptr);
    EXPECT_EQ(connection.screen()->width_in_pixels, 48)
        << "the second screen has its own geometry";
  }
  {
    const ScopedEnvironment missing("DISPLAY",
                                    (nested.display() + ".7").c_str());
    X11DisplayConnection connection;
    std::string error;
    EXPECT_FALSE(connection.connect(error));
    EXPECT_THAT(error, testing::AnyOf(HasSubstr("has no screen"),
                                      HasSubstr("cannot connect")));
  }
}

// ------------------------------------------------------------- monitor capture// ------------------------------------------------------------- monitor capture

// Also covers the fallback for servers that flag no monitor primary, which is
// what Xvfb does: an empty selection must still bind a monitor.
TEST_F(X11, CapturesTheWholePrimaryMonitor) {
  std::string error;
  const auto monitors = x11Monitors(error);
  ASSERT_FALSE(monitors.empty());

  auto capture = makeX11Capture();
  ASSERT_TRUE(startForTest(*capture, MonitorCaptureSource{}));
  const auto geometry = capture->selectedGeometry();
  EXPECT_GT(geometry.width, 0);
  EXPECT_GT(geometry.height, 0);

  Frame frame;
  ASSERT_TRUE(capture->next(frame, std::chrono::milliseconds(100)));
  EXPECT_EQ(frame.width, geometry.width);
  EXPECT_EQ(frame.height, geometry.height);
  EXPECT_EQ(frame.stride, uint32_t(geometry.width) * 4);
  EXPECT_EQ(frame.bgra.size(), size_t(frame.stride) * frame.height);
  EXPECT_EQ(frame.sequence, 1u) << "frames are numbered from one";

  ASSERT_TRUE(capture->next(frame, std::chrono::milliseconds(100)));
  EXPECT_EQ(frame.sequence, 2u);

  capture->stop();
  EXPECT_FALSE(capture->next(frame, std::chrono::milliseconds(100)))
      << "a stopped capture produces nothing";
}

TEST_F(X11, CapturesAMonitorByName) {
  std::string error;
  const auto monitors = x11Monitors(error);
  ASSERT_FALSE(monitors.empty());
  auto capture = makeX11Capture();
  ASSERT_TRUE(startForTest(*capture, MonitorCaptureSource{monitors.front().name}));
  EXPECT_EQ(capture->selectedGeometry().width, monitors.front().width);
  capture->stop();
}

TEST(X11Capture, RefusesToStartWithoutADisplayAndSaysHowToFixIt) {
  const ScopedEnvironment display("DISPLAY", nullptr);
  auto capture = makeX11Capture();
  std::optional<SessionError> problem;
  EXPECT_FALSE(capture->start(MonitorCaptureSource{},
                              [&](SessionError error) { problem = error; }));
  ASSERT_TRUE(problem.has_value());
  EXPECT_EQ(problem->component, "video");
  EXPECT_THAT(problem->message, HasSubstr("DISPLAY is not set"));
  EXPECT_THAT(problem->hint, HasSubstr("Xorg session"));

  // The same failure with no callback at all must simply be reported by the
  // return value.
  EXPECT_FALSE(capture->start(WindowCaptureSource{1}, {}));
  EXPECT_EQ(capture->selectedGeometry().width, 0);
}

// A monitor capture has to survive the X server being replaced: unlike a window
// ID, a monitor name still means the same thing on the new server.
TEST(X11Capture, RebindsAMonitorAcrossAServerRestart) {
  auto nested = std::make_unique<NestedXvfb>(
      std::vector<std::string>{"-screen", "0", "64x64x24"});
  if (!nested->ready()) GTEST_SKIP() << "a nested Xvfb could not be started";
  const auto display = nested->display();

  auto capture = makeX11Capture();
  std::vector<SessionError> problems;
  ASSERT_TRUE(startForTest(*capture, MonitorCaptureSource{},
                           [&](SessionError problem) {
                             problems.push_back(problem);
                           }));
  Frame frame;
  ASSERT_TRUE(capture->next(frame, std::chrono::milliseconds(100)));
  EXPECT_EQ(frame.width, 64u);

  nested->forgetEnvironment();
  nested->terminate();
  const ScopedEnvironment same("DISPLAY", display.c_str());

  // The replacement has a different screen size, so recovery has to re-read the
  // geometry and rebuild the shared segment rather than reuse either.
  const auto replacement = fork();
  ASSERT_GE(replacement, 0);
  if (replacement == 0) {
      // Detach the server from this process's stdio and process group. A forked
      // X server that keeps the test binary's stdout open makes ctest wait for
      // EOF long after the test itself has finished.
      const int devNull = open("/dev/null", O_RDWR);
      if (devNull >= 0) {
        dup2(devNull, STDIN_FILENO);
        dup2(devNull, STDOUT_FILENO);
        dup2(devNull, STDERR_FILENO);
        if (devNull > STDERR_FILENO) close(devNull);
      }
      setsid();
    execlp("Xvfb", "Xvfb", display.c_str(), "-screen", "0", "96x72x24",
           "-nolisten", "tcp", nullptr);
    _exit(127);
  }
  bool rebound = false;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!rebound && std::chrono::steady_clock::now() < deadline) {
    capture->next(frame, std::chrono::milliseconds(20));
    rebound = capture->selectedGeometry().width == 96;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  EXPECT_TRUE(rebound) << "a monitor must be rebound to the new server";
  if (rebound) {
    capture->setRegion({0, 0, 96, 72});
    EXPECT_TRUE(capture->next(frame, std::chrono::milliseconds(100)));
    EXPECT_EQ(frame.width, 96u);
    EXPECT_THAT(problems, testing::IsEmpty())
        << "a server restart is recoverable for a monitor capture";
  }
  capture->stop();
  kill(replacement, SIGTERM);
  waitpid(replacement, nullptr, 0);
}

TEST_F(X11, ReportsAMonitorThatDoesNotExist) {
  auto capture = makeX11Capture();
  std::optional<SessionError> problem;
  EXPECT_FALSE(capture->start(MonitorCaptureSource{"HDMI-999"},
                              [&](SessionError error) { problem = error; }));
  ASSERT_TRUE(problem.has_value());
  EXPECT_EQ(problem->message, "selected monitor is unavailable");
  EXPECT_THAT(problem->hint, HasSubstr("list-monitors"));
}

TEST_F(X11, RestrictsTransfersToTheRequestedRegionAndClampsIt) {
  auto capture = makeX11Capture();
  ASSERT_TRUE(startForTest(*capture, MonitorCaptureSource{}));
  const auto geometry = capture->selectedGeometry();

  capture->setRegion({0, 0, 16, 8});
  Frame frame;
  ASSERT_TRUE(capture->next(frame, std::chrono::milliseconds(100)));
  EXPECT_EQ(frame.width, 16u);
  EXPECT_EQ(frame.height, 8u);

  // A region larger than the source is clamped rather than refused.
  capture->setRegion({0, 0, uint32_t(geometry.width) * 4,
                      uint32_t(geometry.height) * 4});
  ASSERT_TRUE(capture->next(frame, std::chrono::milliseconds(100)));
  EXPECT_EQ(frame.width, geometry.width);
  EXPECT_EQ(frame.height, geometry.height);

  // An origin past the right edge is pulled back inside.
  capture->setRegion({uint32_t(geometry.width), uint32_t(geometry.height), 8, 4});
  ASSERT_TRUE(capture->next(frame, std::chrono::milliseconds(100)));
  EXPECT_EQ(frame.width, 8u);
  EXPECT_EQ(frame.height, 4u);

  // An empty rectangle means the whole source.
  capture->setRegion({0, 0, 0, 0});
  ASSERT_TRUE(capture->next(frame, std::chrono::milliseconds(100)));
  EXPECT_EQ(frame.width, geometry.width);
  capture->stop();
}

TEST_F(X11, ProducesTheSamePixelsWithAndWithoutSharedMemory) {
  const auto target = window(32, 32, "shm-compare");
  const auto gc = context(target);
  fill(connection, target, gc, 0x00ff0000, 32, 32);

  Frame shared, direct;
  auto withShm = makeX11Capture({/*useShm=*/true, /*useComposite=*/false});
  ASSERT_TRUE(startForTest(*withShm, WindowCaptureSource{target}));
  withShm->setRegion({0, 0, 32, 32});
  ASSERT_TRUE(withShm->next(shared, std::chrono::milliseconds(100)));
  withShm->stop();

  auto withoutShm = makeX11Capture({/*useShm=*/false, /*useComposite=*/false});
  ASSERT_TRUE(startForTest(*withoutShm, WindowCaptureSource{target}));
  withoutShm->setRegion({0, 0, 32, 32});
  ASSERT_TRUE(withoutShm->next(direct, std::chrono::milliseconds(100)));
  withoutShm->stop();

  EXPECT_EQ(shared.width, direct.width);
  EXPECT_EQ(shared.height, direct.height);
  EXPECT_EQ(shared.bgra, direct.bgra)
      << "the MIT-SHM path and the get_image fallback must agree";
  ASSERT_GE(direct.bgra.size(), 4u);
  EXPECT_EQ(direct.bgra[0], 0);
  EXPECT_EQ(direct.bgra[1], 0);
  EXPECT_EQ(direct.bgra[2], 255) << "red fill, as BGRA";
}

TEST_F(X11, CapturesAWindowWhoseVisualDiffersFromTheRoot) {
  // A 32-bit ARGB window on a 24-bit root: the pixel format has to be resolved
  // from the window's own visual, not the screen's, and the generic conversion
  // path runs instead of the native row copy.
  xcb_visualid_t argbVisual = 0;
  uint8_t argbDepth = 0;
  for (auto depths = xcb_screen_allowed_depths_iterator(screen); depths.rem;
       xcb_depth_next(&depths)) {
    if (depths.data->depth != 32) continue;
    auto visuals = xcb_depth_visuals_iterator(depths.data);
    if (!visuals.rem) continue;
    argbVisual = visuals.data->visual_id;
    argbDepth = 32;
    break;
  }
  if (!argbVisual) GTEST_SKIP() << "this X server exposes no 32-bit visual";

  const auto target = xcb_generate_id(connection);
  const auto colormap = xcb_generate_id(connection);
  xcb_create_colormap(connection, XCB_COLORMAP_ALLOC_NONE, colormap,
                      screen->root, argbVisual);
  const uint32_t values[] = {0x00000000, 0x00000000, colormap};
  xcb_create_window(connection, argbDepth, target, screen->root, 0, 0, 32, 32, 0,
                    XCB_WINDOW_CLASS_INPUT_OUTPUT, argbVisual,
                    XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL | XCB_CW_COLORMAP,
                    values);
  xcb_map_window(connection, target);
  windows.push_back(target);
  const auto gc = context(target);
  fill(connection, target, gc, 0xff20c040, 32, 32);

  auto capture = makeX11Capture({/*useShm=*/false, /*useComposite=*/false});
  ASSERT_TRUE(startForTest(*capture, WindowCaptureSource{target}));
  capture->setRegion({0, 0, 32, 32});
  Frame frame;
  ASSERT_TRUE(capture->next(frame, std::chrono::milliseconds(100)));
  EXPECT_EQ(frame.width, 32u);
  EXPECT_EQ(frame.height, 32u);
  ASSERT_GE(frame.bgra.size(), 4u);
  EXPECT_EQ(frame.bgra[0], 0x40) << "blue";
  EXPECT_EQ(frame.bgra[1], 0xc0) << "green";
  EXPECT_EQ(frame.bgra[2], 0x20) << "red";
  EXPECT_EQ(frame.bgra[3], 0xff) << "the generic path writes an opaque alpha";
  capture->stop();
  xcb_free_colormap(connection, colormap);
}

// Not every X server offers the extensions the fast paths want. A server built
// without them has to fall back rather than fail, which is also what an old or
// stripped-down Xorg looks like.
TEST(X11WithoutExtensions, FallsBackWhenShmCompositeAndRandrAreAllMissing) {
  NestedXvfb nested({"-screen", "0", "80x60x24", "-extension", "MIT-SHM",
                     "-extension", "Composite", "-extension", "RANDR"});
  if (!nested.ready()) GTEST_SKIP() << "a nested Xvfb could not be started";
  auto* connection = connectForTest();
  ASSERT_NE(connection, nullptr);
  auto* screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
  ASSERT_NE(screen, nullptr);

  // With no RandR there are no monitors to enumerate, so the whole screen is
  // offered under a synthetic name.
  std::string error;
  const auto monitors = x11Monitors(error);
  ASSERT_EQ(monitors.size(), 1u) << error;
  EXPECT_EQ(monitors.front().name, "X11-screen-0");
  EXPECT_EQ(monitors.front().width, 80);
  EXPECT_EQ(monitors.front().height, 60);
  EXPECT_TRUE(monitors.front().primary);

  // MIT-SHM and Composite are both requested and both unavailable; capture has
  // to use xcb_get_image on the window itself.
  auto capture = makeX11Capture({/*useShm=*/true, /*useComposite=*/true});
  ASSERT_TRUE(startForTest(*capture, MonitorCaptureSource{}));
  Frame frame;
  ASSERT_TRUE(capture->next(frame, std::chrono::milliseconds(100)));
  EXPECT_EQ(frame.width, 80u);
  EXPECT_EQ(frame.height, 60u);
  capture->stop();

  const auto target = createWindow(connection, screen, 32, 24, "no-extensions");
  const auto gc = xcb_generate_id(connection);
  xcb_create_gc(connection, gc, target, 0, nullptr);
  fill(connection, target, gc, 0x00ff0000, 32, 24);
  ASSERT_TRUE(startForTest(*capture, WindowCaptureSource{target}));
  capture->setRegion({0, 0, 32, 24});
  ASSERT_TRUE(capture->next(frame, std::chrono::milliseconds(100)));
  EXPECT_EQ(frame.width, 32u);
  ASSERT_GE(frame.bgra.size(), 4u);
  EXPECT_EQ(frame.bgra[2], 255) << "red fill, as BGRA";
  capture->stop();

  xcb_free_gc(connection, gc);
  xcb_destroy_window(connection, target);
  xcb_disconnect(connection);
}

// -------------------------------------------------------------- window capture

TEST_F(X11, RefusesToCaptureWithoutAWindowSelection) {
  auto capture = makeX11Capture();
  std::optional<SessionError> problem;
  EXPECT_FALSE(capture->start(WindowCaptureSource{0},
                              [&](SessionError error) { problem = error; }));
  ASSERT_TRUE(problem.has_value());
  EXPECT_EQ(problem->message, "no window was selected");
}

TEST_F(X11, ReportsAWindowThatIsNotViewable) {
  const auto hidden = window(32, 32, "never-mapped", /*map=*/false);
  sync(connection);
  auto capture = makeX11Capture();
  std::optional<SessionError> problem;
  EXPECT_FALSE(capture->start(WindowCaptureSource{hidden},
                              [&](SessionError error) { problem = error; }));
  ASSERT_TRUE(problem.has_value());
  EXPECT_EQ(problem->message, "selected window is unavailable");
  EXPECT_THAT(problem->hint, HasSubstr("visible window"));
}

TEST_F(X11, TracksAWindowResizeThroughThePeriodicRefresh) {
  const auto target = window(32, 32, "direct-fallback");
  const auto gc = context(target);
  fill(connection, target, gc, 0x00ff0000, 32, 32);

  auto capture = makeX11Capture({/*useShm=*/false, /*useComposite=*/false});
  std::vector<SessionError> problems;
  ASSERT_TRUE(startForTest(*capture, WindowCaptureSource{target},
                           [&](SessionError error) {
                             problems.push_back(error);
                           }));
  capture->setRegion({0, 0, 32, 32});
  Frame frame;
  ASSERT_TRUE(capture->next(frame, std::chrono::milliseconds(0)));
  ASSERT_GE(frame.bgra.size(), 4u);
  EXPECT_EQ(frame.bgra[2], 255);

  const uint32_t size[] = {48, 24};
  xcb_configure_window(connection, target,
                       XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, size);
  fill(connection, target, gc, 0x000000ff, 48, 24);
  // The geometry poll runs every 30 frames rather than on every capture.
  for (int attempt = 0; attempt < 30; ++attempt)
    capture->next(frame, std::chrono::milliseconds(0));
  const auto resized = capture->selectedGeometry();
  EXPECT_EQ(resized.width, 48);
  EXPECT_EQ(resized.height, 24);

  capture->setRegion({0, 0, 48, 24});
  ASSERT_TRUE(capture->next(frame, std::chrono::milliseconds(0)));
  EXPECT_EQ(frame.width, 48u);
  EXPECT_EQ(frame.height, 24u);
  EXPECT_EQ(frame.bgra[0], 255) << "blue fill, as BGRA";
  EXPECT_THAT(problems, testing::IsEmpty())
      << "a resize is recoverable and must not be reported as fatal";
  capture->stop();
}

TEST_F(X11, FallsBackToTheSlowPathWhenTheSharedSegmentKeepsFailing) {
  // The shared segment is sized for the window as it was at start(). Growing the
  // window makes every shared request fail, and one failure is not proof that
  // MIT-SHM is unusable, so the fallback only becomes permanent after several.
  const auto target = window(32, 32, "shm-stale");
  const auto gc = context(target);
  fill(connection, target, gc, 0x00ff0000, 32, 32);

  auto capture = makeX11Capture({/*useShm=*/true, /*useComposite=*/false});
  std::vector<SessionError> problems;
  ASSERT_TRUE(startForTest(*capture, WindowCaptureSource{target},
                           [&](SessionError problem) {
                             problems.push_back(problem);
                           }));
  capture->setRegion({0, 0, 32, 32});
  Frame frame;
  ASSERT_TRUE(capture->next(frame, std::chrono::milliseconds(0)));

  const uint32_t size[] = {256, 192};
  xcb_configure_window(connection, target,
                       XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, size);
  fill(connection, target, gc, 0x000000ff, 256, 192);
  // Enough attempts to pass the periodic geometry poll and the shared-failure
  // allowance, whichever recovers first.
  for (int attempt = 0; attempt < 90; ++attempt)
    capture->next(frame, std::chrono::milliseconds(0));
  capture->setRegion({0, 0, 256, 192});
  ASSERT_TRUE(capture->next(frame, std::chrono::milliseconds(0)));
  EXPECT_EQ(frame.width, 256u);
  EXPECT_EQ(frame.height, 192u);
  ASSERT_GE(frame.bgra.size(), 4u);
  EXPECT_EQ(frame.bgra[0], 255) << "blue fill, as BGRA";
  EXPECT_THAT(problems, testing::IsEmpty())
      << "losing the shared segment costs frames, never the session";
  capture->stop();
}

TEST_F(X11, ReportsAFatalErrorOnceCaptureKeepsFailing) {
  const auto target = window(32, 32, "destroyed");
  const auto gc = context(target);
  fill(connection, target, gc, 0x00ff0000, 32, 32);
  auto capture = makeX11Capture({/*useShm=*/false, /*useComposite=*/false});
  std::vector<SessionError> problems;
  ASSERT_TRUE(startForTest(*capture, WindowCaptureSource{target},
                           [&](SessionError error) {
                             problems.push_back(error);
                           }));
  capture->setRegion({0, 0, 32, 32});
  Frame frame;
  ASSERT_TRUE(capture->next(frame, std::chrono::milliseconds(0)));

  // Unmapping makes the window uncapturable; the failure only becomes fatal
  // after it has persisted for about two seconds.
  xcb_unmap_window(connection, target);
  sync(connection);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (problems.empty() && std::chrono::steady_clock::now() < deadline) {
    capture->next(frame, std::chrono::milliseconds(0));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_FALSE(problems.empty()) << "a persistent failure must be reported";
  EXPECT_EQ(problems.front().component, "video");
  EXPECT_EQ(problems.front().message, "X11 capture kept failing");
  EXPECT_THAT(problems.front().hint, HasSubstr("reselect the shared window"));
  capture->stop();
}

TEST_F(X11, RebindsACompositePixmapAcrossUnmapAndRemap) {
  if (!compositeNamedPixmapsAvailable(connection))
    GTEST_SKIP() << "this X server has no usable Composite extension";

  const auto target = window(32, 32, "composite");
  auto* redirectError = xcb_request_check(
      connection, xcb_composite_redirect_window_checked(
                      connection, target, XCB_COMPOSITE_REDIRECT_MANUAL));
  ASSERT_EQ(redirectError, nullptr);
  free(redirectError);
  const auto gc = context(target);
  fill(connection, target, gc, 0x00ff0000, 32, 32);

  auto capture = makeX11Capture({/*useShm=*/false, /*useComposite=*/true});
  ASSERT_TRUE(startForTest(*capture, WindowCaptureSource{target},
                           [](SessionError) {}));
  capture->setRegion({0, 0, 32, 32});
  Frame frame;
  ASSERT_TRUE(capture->next(frame, std::chrono::milliseconds(0)));

  // A named pixmap stays readable right after unmap, but the periodic refresh
  // must then reject it rather than keep serving stale content.
  xcb_unmap_window(connection, target);
  sync(connection);
  EXPECT_TRUE(capture->next(frame, std::chrono::milliseconds(0)));
  bool rejectedStalePixmap = false;
  for (int attempt = 0; attempt < 120 && !rejectedStalePixmap; ++attempt)
    rejectedStalePixmap = !capture->next(frame, std::chrono::milliseconds(0));
  EXPECT_TRUE(rejectedStalePixmap);

  xcb_map_window(connection, target);
  fill(connection, target, gc, 0x000000ff, 32, 32);
  ASSERT_TRUE(capture->next(frame, std::chrono::milliseconds(0)));
  // Reach the refresh that has to bind a fresh pixmap for the remapped window.
  for (int attempt = 0; attempt < 29; ++attempt)
    ASSERT_TRUE(capture->next(frame, std::chrono::milliseconds(0)));
  ASSERT_GE(frame.bgra.size(), 4u);
  EXPECT_EQ(frame.bgra[0], 255);
  EXPECT_EQ(frame.bgra[2], 0);
  capture->stop();
}

TEST_F(X11, RestartingRebindsTheSelectedSource) {
  const auto first = window(32, 32, "restart-first");
  const auto gc = context(first);
  fill(connection, first, gc, 0x00ff0000, 32, 32);
  auto capture = makeX11Capture({/*useShm=*/true, /*useComposite=*/false});
  ASSERT_TRUE(startForTest(*capture, WindowCaptureSource{first}));
  capture->setRegion({0, 0, 32, 32});
  Frame frame;
  ASSERT_TRUE(capture->next(frame, std::chrono::milliseconds(0)));

  // Starting again on a different source must release the previous binding.
  const auto second = window(48, 16, "restart-second");
  const auto secondGc = context(second);
  fill(connection, second, secondGc, 0x0000ff00, 48, 16);
  ASSERT_TRUE(startForTest(*capture, WindowCaptureSource{second}));
  EXPECT_EQ(capture->selectedGeometry().width, 48);
  ASSERT_TRUE(capture->next(frame, std::chrono::milliseconds(0)));
  EXPECT_EQ(frame.width, 48u);
  EXPECT_EQ(frame.height, 16u);
  EXPECT_EQ(frame.bgra[1], 255) << "green fill, as BGRA";
  capture->stop();
  capture->stop() /* idempotent */;
  EXPECT_EQ(capture->selectedGeometry().width, 0);
}

// A window ID is scoped to one X server connection and may name a different
// client after a restart, so it must never be silently rebound.
TEST(X11WindowLifetime, AConnectionLossPermanentlyExpiresTheWindowSelection) {
  int displayPipe[2];
  ASSERT_EQ(pipe(displayPipe), 0);
  const auto server = fork();
  ASSERT_GE(server, 0);
  if (server == 0) {
    close(displayPipe[0]);
    const auto descriptor = std::to_string(displayPipe[1]);
      // Detach the server from this process's stdio and process group. A forked
      // X server that keeps the test binary's stdout open makes ctest wait for
      // EOF long after the test itself has finished.
      const int devNull = open("/dev/null", O_RDWR);
      if (devNull >= 0) {
        dup2(devNull, STDIN_FILENO);
        dup2(devNull, STDOUT_FILENO);
        dup2(devNull, STDERR_FILENO);
        if (devNull > STDERR_FILENO) close(devNull);
      }
      setsid();
    execlp("Xvfb", "Xvfb", "-displayfd", descriptor.c_str(), "-screen", "0",
           "64x64x24", "-nolisten", "tcp", nullptr);
    _exit(127);
  }
  close(displayPipe[1]);
  char number[16]{};
  const auto length = read(displayPipe[0], number, sizeof(number) - 1);
  close(displayPipe[0]);
  if (length <= 0) {
    waitpid(server, nullptr, 0);
    GTEST_SKIP() << "a nested Xvfb server could not be started";
  }
  number[length] = '\0';
  std::string nested = ":" + std::string(number);
  while (!nested.empty() && (nested.back() == '\n' || nested.back() == '\r'))
    nested.pop_back();

  {
    const ScopedEnvironment display("DISPLAY", nested.c_str());
    auto* connection = connectForTest();
    ASSERT_NE(connection, nullptr);
    auto* screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
    const auto target = createWindow(connection, screen, 32, 32, "expires");
    const auto gc = xcb_generate_id(connection);
    xcb_create_gc(connection, gc, target, 0, nullptr);
    fill(connection, target, gc, 0x00ff0000, 32, 32);

    auto capture = makeX11Capture({/*useShm=*/false, /*useComposite=*/false});
    ASSERT_TRUE(startForTest(*capture, WindowCaptureSource{target}));
    capture->setRegion({0, 0, 32, 32});
    Frame frame;
    ASSERT_TRUE(capture->next(frame, std::chrono::milliseconds(0)));

    kill(server, SIGTERM);
    waitpid(server, nullptr, 0);

    // A replacement server deliberately reuses the numeric ID for different
    // content and geometry.
    const auto replacement = fork();
    ASSERT_GE(replacement, 0);
    if (replacement == 0) {
      // Detach the server from this process's stdio and process group. A forked
      // X server that keeps the test binary's stdout open makes ctest wait for
      // EOF long after the test itself has finished.
      const int devNull = open("/dev/null", O_RDWR);
      if (devNull >= 0) {
        dup2(devNull, STDIN_FILENO);
        dup2(devNull, STDOUT_FILENO);
        dup2(devNull, STDERR_FILENO);
        if (devNull > STDERR_FILENO) close(devNull);
      }
      setsid();
      execlp("Xvfb", "Xvfb", nested.c_str(), "-screen", "0", "64x64x24",
             "-nolisten", "tcp", nullptr);
      _exit(127);
    }
    xcb_connection_t* replacementConnection = nullptr;
    for (int attempt = 0; attempt < 200; ++attempt) {
      replacementConnection = xcb_connect(nullptr, nullptr);
      if (replacementConnection &&
          !xcb_connection_has_error(replacementConnection))
        break;
      if (replacementConnection) xcb_disconnect(replacementConnection);
      replacementConnection = nullptr;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_NE(replacementConnection, nullptr);
    auto* replacementScreen =
        xcb_setup_roots_iterator(xcb_get_setup(replacementConnection)).data;
    const auto reused = createWindow(replacementConnection, replacementScreen,
                                    48, 24, "replacement");
    EXPECT_EQ(reused, target) << "the ID really is reused by the new server";
    const auto replacementGc = xcb_generate_id(replacementConnection);
    xcb_create_gc(replacementConnection, replacementGc, reused, 0, nullptr);
    fill(replacementConnection, reused, replacementGc, 0x0000ff00, 48, 24);

    EXPECT_FALSE(capture->next(frame, std::chrono::milliseconds(0)));
    for (int attempt = 0; attempt < 4; ++attempt)
      EXPECT_FALSE(capture->next(frame, std::chrono::milliseconds(0)))
          << "the expiry must stay terminal";
    const auto geometry = capture->selectedGeometry();
    EXPECT_EQ(geometry.width, 32) << "the stale geometry is never updated";
    EXPECT_EQ(geometry.height, 32);
    capture->stop();

    xcb_disconnect(connection);
    xcb_disconnect(replacementConnection);
    kill(replacement, SIGTERM);
    waitpid(replacement, nullptr, 0);
  }
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  auto* probe = connectForTest();
  const bool usable =
      probe && xcb_setup_roots_iterator(xcb_get_setup(probe)).data;
  if (probe) xcb_disconnect(probe);
  if (!usable) {
    std::fputs("X11 tests skipped: no usable DISPLAY\n", stderr);
    return 77;  // CTest SKIP_RETURN_CODE
  }
  return RUN_ALL_TESTS();
}
