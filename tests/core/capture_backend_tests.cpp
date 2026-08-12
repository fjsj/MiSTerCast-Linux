#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "mistercast/config.hpp"
#include "mistercast/interfaces.hpp"
#include "support/scoped_environment.hpp"
#include "support/temp_directory.hpp"

using namespace mistercast;
using mistercast::test::ScopedEnvironment;
using mistercast::test::TemporaryDirectory;
using testing::IsEmpty;

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

TEST(SessionEnvironment, ReadsTheSessionOutOfTheProcessEnvironment) {
  const ScopedEnvironment type("XDG_SESSION_TYPE", "wayland");
  const ScopedEnvironment wayland("WAYLAND_DISPLAY", "wayland-3");
  const ScopedEnvironment display("DISPLAY", ":7");
  const auto session = SessionEnvironment::current();
  EXPECT_EQ(session.sessionType, "wayland");
  EXPECT_EQ(session.waylandDisplay, "wayland-3");
  EXPECT_EQ(session.display, ":7");
}

TEST(SessionEnvironment, ReportsAnUnsetVariableAsEmptyRatherThanCrashing) {
  const ScopedEnvironment type("XDG_SESSION_TYPE", nullptr);
  const ScopedEnvironment wayland("WAYLAND_DISPLAY", nullptr);
  const ScopedEnvironment display("DISPLAY", nullptr);
  const auto session = SessionEnvironment::current();
  EXPECT_THAT(session.sessionType, IsEmpty());
  EXPECT_THAT(session.waylandDisplay, IsEmpty());
  EXPECT_THAT(session.display, IsEmpty());
}

// Which backend the factory built is observable through the failure it reports:
// only the X11 one has anything to say about DISPLAY. Asserting on that rather
// than on a type keeps the backends free of accessors that exist only for tests.
TEST(MakeVideoCapture, BuildsTheBackendItWasAskedFor) {
  const ScopedEnvironment display("DISPLAY", "");
  // No reachable portal, on purpose: with one, and with a grant already stored,
  // this would start a real capture of the screen of whoever is running the
  // suite. Which backend was built is still observable, because only the X11 one
  // has anything to say about DISPLAY.
  const ScopedEnvironment bus("DBUS_SESSION_BUS_ADDRESS",
                              "unix:path=/nonexistent/no-such-bus");
  std::optional<SessionError> x11Error;
  auto x11 = makeVideoCapture(CaptureBackend::X11);
  ASSERT_TRUE(x11);
  EXPECT_FALSE(x11->start(MonitorCaptureSource{},
                          [&](SessionError error) { x11Error = error; }));
  ASSERT_TRUE(x11Error);
  EXPECT_THAT(x11Error->message, testing::HasSubstr("DISPLAY"));

  std::optional<SessionError> portalError;
  auto portal = makeVideoCapture(CaptureBackend::Portal);
  ASSERT_TRUE(portal);
  EXPECT_FALSE(portal->start(MonitorCaptureSource{},
                             [&](SessionError error) { portalError = error; }));
  ASSERT_TRUE(portalError);
  EXPECT_THAT(portalError->message, testing::Not(testing::HasSubstr("DISPLAY")));
}

TEST(PortalOptionsFromTokenFile, OffersTheStoredGrantAndWritesBackANewOne) {
  const TemporaryDirectory directory("portal-options");
  const auto path = (directory.path() / "portal-token").string();
  constexpr const char* stored = "6b1f2c3d-4e5a-6789-abcd-ef0123456789";
  constexpr const char* issued = "0011aabb-ccdd-eeff-0011-223344556677";
  std::string error;
  ASSERT_TRUE(savePortalRestoreToken(stored, path, error)) << error;

  std::vector<std::string> warnings;
  auto options = portalOptionsFromTokenFile(
      path, [&](const std::string& warning) { warnings.push_back(warning); });
  EXPECT_EQ(options.restoreToken, stored);
  ASSERT_TRUE(options.onRestoreToken);
  options.onRestoreToken(issued);
  EXPECT_EQ(loadPortalRestoreToken(path), issued)
      << "a new grant must replace the stored one straight away";
  EXPECT_THAT(warnings, IsEmpty());
}

TEST(PortalOptionsFromTokenFile, HasNoTokenToOfferWhenNoneWasEverStored) {
  const TemporaryDirectory directory("portal-options-empty");
  auto options = portalOptionsFromTokenFile(
      (directory.path() / "portal-token").string(), {});
  EXPECT_THAT(options.restoreToken, IsEmpty());
  ASSERT_TRUE(options.onRestoreToken);
  options.onRestoreToken("0011aabb-ccdd-eeff-0011-223344556677");
}

// Without a warning sink a failed store has nowhere to go, and must be dropped
// rather than dereferenced: the caller asked not to be told.
TEST(PortalOptionsFromTokenFile, SurvivesAFailedStoreWithNoWarningSink) {
  const TemporaryDirectory directory("portal-options-silent");
  const auto blocker = directory.write("blocker", "not a directory");
  auto options =
      portalOptionsFromTokenFile((blocker / "nested" / "token").string(), {});
  ASSERT_TRUE(options.onRestoreToken);
  options.onRestoreToken("0011aabb-ccdd-eeff-0011-223344556677");
}

// Failing to remember a grant costs a dialog next time and nothing else, so it is
// reported rather than treated as a failure to start.
TEST(PortalOptionsFromTokenFile, WarnsWhenTheGrantCannotBeStored) {
  const TemporaryDirectory directory("portal-options-unwritable");
  const auto blocker = directory.write("blocker", "not a directory");
  std::vector<std::string> warnings;
  auto options = portalOptionsFromTokenFile(
      (blocker / "nested" / "portal-token").string(),
      [&](const std::string& warning) { warnings.push_back(warning); });
  ASSERT_TRUE(options.onRestoreToken);
  options.onRestoreToken("0011aabb-ccdd-eeff-0011-223344556677");
  ASSERT_EQ(warnings.size(), 1u);
  EXPECT_THAT(warnings.front(),
              testing::HasSubstr("Cannot remember the screen-sharing"));
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

  // The rest of the interface has to stay callable: the session queries geometry
  // and sets a region before it ever looks at whether start() succeeded.
  EXPECT_EQ(capture->selectedGeometry().width, 0);
  EXPECT_EQ(capture->selectedGeometry().height, 0);
  capture->setRegion({0, 0, 320, 240});
  Frame frame;
  EXPECT_FALSE(capture->next(frame, std::chrono::milliseconds(1)));
  capture->stop();
  capture->stop();
}
