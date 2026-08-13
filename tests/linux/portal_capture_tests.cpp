#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>

#include "mistercast/interfaces.hpp"
#include "mistercast/transform.hpp"
#include "support/portal_session.hpp"

using namespace mistercast;
using mistercast::test::portalSessionSkipCode;

namespace {

// Drives the real ScreenCast portal on a real compositor. A fake one would prove
// nothing: what breaks is the handshake a portal answers and the buffers PipeWire
// delivers. The session is the rig in packaging/docker/, described in AGENTS.md.
//
// No test asserts the rig's output size, because the compositor decides it; each
// checks frames against what the portal reported. Crop geometry and BGRA byte
// order are covered by the cropToBgra tests in the core suite.

constexpr auto kFrameTimeout = std::chrono::milliseconds(2000);

// A compositor emits a frame only when something changes, and the rig desktop has
// nothing running on it. The first frame always arrives, because a stream starts
// with a full copy, so waiting for that one is what proves pixels flow.
bool awaitFrame(IVideoCapture& capture, Frame& frame) {
  const auto deadline = std::chrono::steady_clock::now() + kFrameTimeout;
  while (std::chrono::steady_clock::now() < deadline)
    if (capture.next(frame, std::chrono::milliseconds(100))) return true;
  return false;
}

// The colour the rig painted the desktop before starting the portal, as
// "RRGGBB", or empty when this session did not paint one. Driving damage from a
// test would be the obvious way to get known pixels, but repainting while
// xdg-desktop-portal-wlr is streaming kills it, so the rig paints once up front
// and names the colour here instead.
std::string riggedBackground() {
  const char* value = std::getenv("MISTERCAST_RIG_BACKGROUND");
  return value ? value : std::string{};
}

std::unique_ptr<IVideoCapture> startedCapture(std::optional<SessionError>& error,
                                              PortalCaptureOptions options = {}) {
  auto capture = makePortalCapture(std::move(options));
  if (!capture->start(MonitorCaptureSource{},
                      [&](SessionError problem) { error = std::move(problem); }))
    return nullptr;
  return capture;
}

TEST(PortalCapture, GrantsASessionAndReportsTheCompositorsGeometry) {
  std::optional<SessionError> error;
  auto capture = startedCapture(error);
  ASSERT_TRUE(capture) << (error ? error->message : "start returned false");
  const auto geometry = capture->selectedGeometry();
  // The size is known by the time start() returns, because the session computes
  // the crop from it immediately afterwards.
  EXPECT_GT(geometry.width, 0);
  EXPECT_GT(geometry.height, 0);
  EXPECT_FALSE(error) << error->message;
}

TEST(PortalCapture, DeliversFramesAtTheNegotiatedSize) {
  std::optional<SessionError> error;
  auto capture = startedCapture(error);
  ASSERT_TRUE(capture) << (error ? error->message : "start returned false");
  const auto geometry = capture->selectedGeometry();
  Frame frame;
  ASSERT_TRUE(awaitFrame(*capture, frame)) << "no frame within the timeout";
  EXPECT_EQ(frame.width, geometry.width);
  EXPECT_EQ(frame.height, geometry.height);
  EXPECT_EQ(frame.stride, frame.width * 4);
  EXPECT_EQ(frame.bgra.size(), size_t(frame.width) * frame.height * 4);
  EXPECT_GT(frame.sequence, 0u);
  EXPECT_FALSE(error) << error->message;
}

TEST(PortalCapture, NeverDeliversAStaleCropAfterTheRegionChanges) {
  std::optional<SessionError> error;
  auto capture = startedCapture(error);
  ASSERT_TRUE(capture) << (error ? error->message : "start returned false");
  const auto geometry = capture->selectedGeometry();
  ASSERT_GE(geometry.width, 64);
  ASSERT_GE(geometry.height, 64);
  Frame frame;
  ASSERT_TRUE(awaitFrame(*capture, frame));
  ASSERT_EQ(frame.width, geometry.width);
  capture->setRegion({16, 8, 64, 32});
  // The crop applies to the next buffer the compositor produces, and on a still
  // desktop there may not be another one -- the rig cannot force damage without
  // killing its own portal. What must hold either way is that the frame held from
  // before the change is not handed back at its old geometry: the session changes
  // the region exactly when it has stopped expecting that geometry, so a stale
  // frame would go out against a modeline that no longer matches it.
  const auto deadline = std::chrono::steady_clock::now() + kFrameTimeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (!capture->next(frame, std::chrono::milliseconds(100))) continue;
    EXPECT_EQ(frame.width, 64u) << "a stale crop was delivered after setRegion";
    EXPECT_EQ(frame.height, 32u);
    EXPECT_EQ(frame.bgra.size(), size_t(64) * 32 * 4);
    return;
  }
}

// The one test that looks at pixels rather than geometry, and so the one that
// would catch a channel swap: a solid background of a known colour must arrive
// as that colour in BGRA byte order. Everything upstream of this -- the portal
// grant, the negotiated format, the mapping, the crop copy -- has to be right
// for it to pass.
TEST(PortalCapture, DeliversThePixelsTheCompositorPaintedInBgraOrder) {
  const auto background = riggedBackground();
  if (background.size() != 6)
    GTEST_SKIP() << "this session painted no known desktop colour";
  const auto channel = [&](size_t index) {
    return uint8_t(std::stoul(background.substr(index, 2), nullptr, 16));
  };
  const uint8_t red = channel(0), green = channel(2), blue = channel(4);
  std::optional<SessionError> error;
  auto capture = startedCapture(error);
  ASSERT_TRUE(capture) << (error ? error->message : "start returned false");
  Frame frame;
  ASSERT_TRUE(awaitFrame(*capture, frame));
  ASSERT_GE(frame.bgra.size(), 4u);
  // Sampled at the centre, away from anything the compositor might overlay.
  const size_t middle =
      (size_t(frame.height / 2) * frame.width + frame.width / 2) * 4;
  ASSERT_LT(middle + 3, frame.bgra.size());
  EXPECT_EQ(frame.bgra[middle], blue);
  EXPECT_EQ(frame.bgra[middle + 1], green);
  EXPECT_EQ(frame.bgra[middle + 2], red);
}

// A region left over from a larger source would read past the mapped buffer, so
// setRegion drops anything that does not fit instead of trusting the caller.
TEST(PortalCapture, IgnoresARegionLargerThanTheSource) {
  std::optional<SessionError> error;
  auto capture = startedCapture(error);
  ASSERT_TRUE(capture) << (error ? error->message : "start returned false");
  const auto geometry = capture->selectedGeometry();
  capture->setRegion({0, 0, uint32_t(geometry.width) + 64, geometry.height});
  Frame frame;
  ASSERT_TRUE(awaitFrame(*capture, frame));
  EXPECT_EQ(frame.width, geometry.width) << "an impossible region must not crop";
  EXPECT_EQ(frame.height, geometry.height);
}

// A still desktop produces no new buffers, but the MiSTer still needs a frame
// per modeline refresh, so next() hands the last one back rather than starving
// the stream. This is the behaviour difference from X11 capture worth pinning.
TEST(PortalCapture, RedeliversTheLastFrameWhileNothingChanges) {
  std::optional<SessionError> error;
  auto capture = startedCapture(error);
  ASSERT_TRUE(capture) << (error ? error->message : "start returned false");
  Frame frame;
  ASSERT_TRUE(awaitFrame(*capture, frame));
  const auto width = frame.width;
  uint64_t previous = frame.sequence;
  for (int attempt = 0; attempt < 5; ++attempt) {
    ASSERT_TRUE(capture->next(frame, std::chrono::milliseconds(100)))
        << "attempt " << attempt << " starved on an idle desktop";
    EXPECT_GT(frame.sequence, previous) << "each delivery gets a new sequence";
    EXPECT_EQ(frame.width, width);
    previous = frame.sequence;
  }
}

TEST(PortalCapture, StoppingIsIdempotentAndReleasesTheGrant) {
  std::optional<SessionError> error;
  auto capture = startedCapture(error);
  ASSERT_TRUE(capture) << (error ? error->message : "start returned false");
  Frame frame;
  ASSERT_TRUE(awaitFrame(*capture, frame));
  capture->stop();
  capture->stop();
  EXPECT_FALSE(capture->next(frame, std::chrono::milliseconds(50)));
  // Closing the portal session must not stop the next one from being granted,
  // which is what a leaked session or an unclosed PipeWire remote would break.
  std::optional<SessionError> restartError;
  ASSERT_TRUE(capture->start(
      MonitorCaptureSource{},
      [&](SessionError problem) { restartError = std::move(problem); }))
      << (restartError ? restartError->message : "restart returned false");
  EXPECT_TRUE(awaitFrame(*capture, frame));
}

TEST(PortalCapture, ARejectedRestoreTokenStillGrantsASession) {
  PortalCaptureOptions options;
  // A well-formed token no portal ever issued, which is what a grant that has
  // been revoked since it was stored looks like. Capture must fall back to
  // asking rather than failing the stream.
  options.restoreToken = "6b1f2c3d-4e5a-6789-abcd-ef0123456789";
  std::optional<SessionError> error;
  auto capture = startedCapture(error, std::move(options));
  ASSERT_TRUE(capture) << (error ? error->message : "start returned false");
  Frame frame;
  EXPECT_TRUE(awaitFrame(*capture, frame));
}

TEST(PortalCapture, ReportsWhetherTheSessionCanBeRestored) {
  PortalCaptureOptions options;
  std::optional<std::string> issued;
  options.onRestoreToken = [&](const std::string& token) { issued = token; };
  std::optional<SessionError> error;
  auto capture = startedCapture(error, std::move(options));
  ASSERT_TRUE(capture) << (error ? error->message : "start returned false");
  // Whether a token comes back is the portal backend's choice, not this code's:
  // xdg-desktop-portal-wlr issues none, GNOME and KDE do. What must hold either
  // way is that a token is never empty and never needs quoting when stored.
  if (!issued) GTEST_SKIP() << "this portal backend issues no restore token";
  EXPECT_FALSE(issued->empty());
  EXPECT_EQ(issued->find_first_of(" \t\n\""), std::string::npos);
}

// Nothing else can be done with the ScreenCast portal once the grant is gone, so
// the failure has to reach the session rather than leaving it streaming a frozen
// frame. next() is the reporting path, matching X11 capture.
TEST(PortalCapture, ReportsAnUnsupportedSourceKindInsteadOfHanging) {
  auto capture = makePortalCapture();
  std::optional<SessionError> error;
  const auto started = capture->start(
      WindowCaptureSource{}, [&](SessionError problem) { error = problem; });
  // Window capture exists in the portal API but not in every backend; wlroots
  // advertises monitors only. Either answer is correct, but a refusal has to say
  // something actionable and must not take the picker timeout to arrive.
  if (started) {
    Frame frame;
    EXPECT_TRUE(awaitFrame(*capture, frame));
    return;
  }
  ASSERT_TRUE(error);
  EXPECT_EQ(error->component, "video");
  EXPECT_FALSE(error->message.empty());
  EXPECT_FALSE(error->hint.empty());
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  if (const int skip = portalSessionSkipCode("Wayland tests")) return skip;
  return RUN_ALL_TESTS();
}
