#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "mistercast/interfaces.hpp"

using namespace mistercast;

namespace {
// The three variables backend selection reads, in the combinations a real
// session produces. A Wayland session sets WAYLAND_DISPLAY and usually DISPLAY
// too, because XWayland is running; an Xorg session sets only DISPLAY.
SessionEnvironment wayland() { return {"wayland", "wayland-0", ":0"}; }
SessionEnvironment xorg() { return {"x11", "", ":0"}; }
SessionEnvironment bare() { return {"", "", ""}; }
}  // namespace

TEST(ResolveCaptureBackend, PrefersThePortalOnAWaylandSession) {
  EXPECT_EQ(resolveCaptureBackend(CaptureBackend::Auto, wayland()),
            CaptureBackend::Portal);
}

// The DISPLAY a Wayland session exports belongs to XWayland, which connects and
// then hands out no desktop content, so it must not win over WAYLAND_DISPLAY.
TEST(ResolveCaptureBackend, IgnoresTheXwaylandDisplayWhenWaylandIsRunning) {
  EXPECT_EQ(resolveCaptureBackend(CaptureBackend::Auto,
                                  {"wayland", "wayland-0", ":0"}),
            CaptureBackend::Portal);
  // Either signal alone is enough: a session type without WAYLAND_DISPLAY
  // happens under some session managers, and the reverse under nested
  // compositors.
  EXPECT_EQ(resolveCaptureBackend(CaptureBackend::Auto, {"wayland", "", ":0"}),
            CaptureBackend::Portal);
  EXPECT_EQ(resolveCaptureBackend(CaptureBackend::Auto, {"", "wayland-1", ":0"}),
            CaptureBackend::Portal);
}

TEST(ResolveCaptureBackend, PicksX11OnAnXorgSession) {
  EXPECT_EQ(resolveCaptureBackend(CaptureBackend::Auto, xorg()),
            CaptureBackend::X11);
}

// Nothing will capture either way, and the X11 failure names DISPLAY, which is
// the one thing the user can act on.
TEST(ResolveCaptureBackend, FallsBackToX11WithNoSessionAtAll) {
  EXPECT_EQ(resolveCaptureBackend(CaptureBackend::Auto, bare()),
            CaptureBackend::X11);
}

TEST(ResolveCaptureBackend, ReturnsAnExplicitChoiceAgainstTheSession) {
  EXPECT_EQ(resolveCaptureBackend(CaptureBackend::X11, wayland()),
            CaptureBackend::X11);
  EXPECT_EQ(resolveCaptureBackend(CaptureBackend::Portal, xorg()),
            CaptureBackend::Portal);
  EXPECT_EQ(resolveCaptureBackend(CaptureBackend::Portal, bare()),
            CaptureBackend::Portal);
}

TEST(ParseCaptureBackend, AcceptsTheNamesTheCliAndConfigUse) {
  CaptureBackend backend{};
  EXPECT_TRUE(parseCaptureBackend("auto", backend));
  EXPECT_EQ(backend, CaptureBackend::Auto);
  EXPECT_TRUE(parseCaptureBackend("x11", backend));
  EXPECT_EQ(backend, CaptureBackend::X11);
  EXPECT_TRUE(parseCaptureBackend("portal", backend));
  EXPECT_EQ(backend, CaptureBackend::Portal);
  // What users call the session, rather than the protocol MiSTerCast speaks.
  EXPECT_TRUE(parseCaptureBackend("wayland", backend));
  EXPECT_EQ(backend, CaptureBackend::Portal);
  EXPECT_FALSE(parseCaptureBackend("xorg", backend));
  EXPECT_FALSE(parseCaptureBackend("", backend));
}

TEST(ParseCaptureBackend, RoundTripsThroughToString) {
  for (const auto backend : {CaptureBackend::Auto, CaptureBackend::X11,
                             CaptureBackend::Portal}) {
    CaptureBackend parsed{};
    ASSERT_TRUE(parseCaptureBackend(toString(backend), parsed))
        << toString(backend);
    EXPECT_EQ(parsed, backend);
  }
}

// A build without libpipewire/libsystemd still resolves to the portal on a
// Wayland session, so that start() can name the missing build dependency
// instead of an X11 failure the user cannot act on.
TEST(PortalCapture, ExplainsAMissingBuildDependencyWhenNotCompiledIn) {
  auto capture = makePortalCapture();
  ASSERT_TRUE(capture);
  if (portalCaptureAvailable()) GTEST_SKIP() << "built with portal support";
  std::optional<SessionError> reported;
  EXPECT_FALSE(capture->start(MonitorCaptureSource{}, [&](SessionError error) {
    reported = std::move(error);
  }));
  ASSERT_TRUE(reported);
  EXPECT_THAT(reported->message, testing::HasSubstr("no Wayland screen"));
  EXPECT_THAT(reported->hint, testing::HasSubstr("libpipewire"));
}
