#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "mistercast/stream_session.hpp"
#include "support/fake_capture.hpp"
#include "support/fake_groovy_endpoint.hpp"
#include "support/fake_mister.hpp"
#include "support/groovy_wire.hpp"
#include "support/wait_for.hpp"

using namespace mistercast;
using namespace mistercast::test;
using testing::HasSubstr;

namespace {

AppConfig sessionConfig() {
  AppConfig config;
  config.target = "127.0.0.1";
  config.source.audio = false;
  config.source.preview = false;
  config.source.crop = CropMode::Full43;
  config.modeline = Modeline::safeDefault();
  return config;
}

// Owns the fakes and the session so every test reads the same way, and records
// the state transitions the GUI and CLI both rely on.
class Session {
 public:
  explicit Session(bool withAudio = false) {
    auto video = std::make_unique<FakeVideo>();
    auto audio = std::make_unique<FakeAudio>();
    video_ = video.get();
    audio_ = audio.get();
    audio_->produce = withAudio;
    session_ = std::make_unique<StreamSession>(std::move(video),
                                               std::move(audio));
  }

  ~Session() { session_->stop(); }

  bool start(const AppConfig& config,
             const CaptureSource& source = MonitorCaptureSource{},
             std::string* error = nullptr) {
    return session_->start(
        config, source,
        [this](SessionState state, const std::optional<SessionError>& problem) {
          std::lock_guard<std::mutex> lock(mutex_);
          states_.push_back(state);
          if (problem) problems_.push_back(*problem);
        },
        error);
  }

  std::vector<SessionState> states() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return states_;
  }
  std::vector<SessionError> problems() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return problems_;
  }

  StreamSession& operator*() const { return *session_; }
  StreamSession* operator->() const { return session_.get(); }
  FakeVideo& video() const { return *video_; }
  FakeAudio& audio() const { return *audio_; }

 private:
  mutable std::mutex mutex_;
  std::vector<SessionState> states_;
  std::vector<SessionError> problems_;
  FakeVideo* video_{};
  FakeAudio* audio_{};
  std::unique_ptr<StreamSession> session_;
};

// StreamSession always dials UDP 32100, so the fake receiver has to own it.
// A machine already running something there skips rather than fails.
class SessionTest : public testing::Test {
 protected:
  // GTEST_SKIP() returns from the function it appears in, so a skip raised here
  // only aborts this helper. Every call site follows it with
  // `if (IsSkipped()) return;` — without that the body runs on against an
  // unbound receiver and fails instead of skipping.
  void bindReceiver(uint8_t statusBits = kHealthy) {
    silent.reset();  // free the port if this test was holding it
    mister = std::make_unique<FakeMister>(statusBits);
    if (!mister->bound())
      GTEST_SKIP() << "UDP port 32100 is already in use on this machine";
  }

  // Owns UDP 32100 without ever answering, for the tests whose premise is that
  // nothing is listening. Leaving the port unbound would do on an idle machine,
  // but anything else on 32100 — another ctest invocation, a running MiSTerCast —
  // answers the init and turns those tests into failures about the wrong thing.
  void holdPortSilently() {
    silent = std::make_unique<FakeGroovyEndpoint>(
        [](FakeGroovyEndpoint&, const FakeGroovyEndpoint::Packet&) {}, 32100);
    if (!silent->valid())
      GTEST_SKIP() << "UDP port 32100 is already in use on this machine";
  }

  std::unique_ptr<FakeGroovyEndpoint> silent;
  std::unique_ptr<FakeMister> mister;
};

// ------------------------------------------------------------- start validation

TEST_F(SessionTest, RefusesAConfigurationTheProtocolCannotStream) {
  Session session;
  auto config = sessionConfig();
  config.modeline = Modeline{"1024x768", 65.0,  1024, 1048, 1184,
                             1344,       768,  771,  777,  806, false};
  std::string error;
  EXPECT_FALSE(session.start(config, MonitorCaptureSource{}, &error));
  EXPECT_THAT(error, HasSubstr("exceeds Groovy_MiSTer frame buffer"));
  EXPECT_EQ(session->state(), SessionState::Error);
  ASSERT_FALSE(session.problems().empty());
  EXPECT_EQ(session.problems().front().component, "config");
  EXPECT_EQ(session.video().starts, 0u) << "nothing is opened before validation";
}

TEST_F(SessionTest, RequiresATargetAddress) {
  Session session;
  auto config = sessionConfig();
  config.target.clear();
  std::string error;
  EXPECT_FALSE(session.start(config, MonitorCaptureSource{}, &error));
  EXPECT_EQ(error, "target address is required");
  EXPECT_EQ(session.video().starts, 0u);
}

TEST_F(SessionTest, ReportsAVideoCaptureThatCannotStart) {
  Session session;
  session.video().startSucceeds = false;
  std::string error;
  EXPECT_FALSE(session.start(sessionConfig(), MonitorCaptureSource{}, &error));
  EXPECT_EQ(error, "video capture initialization failed");
  EXPECT_EQ(session->state(), SessionState::Error);
}

TEST_F(SessionTest, ReportsACropThatCannotBeComputed) {
  Session session;
  session.video().width = 0;
  session.video().height = 0;
  std::string error;
  EXPECT_FALSE(session.start(sessionConfig(), MonitorCaptureSource{}, &error));
  EXPECT_THAT(error, HasSubstr("zero dimensions"));
  EXPECT_EQ(session->state(), SessionState::Error);
  EXPECT_EQ(session.video().stops, 1u) << "capture must be released again";
  ASSERT_FALSE(session.problems().empty());
  EXPECT_EQ(session.problems().back().component, "video");
}

TEST_F(SessionTest, ReportsAnAudioCaptureThatCannotStart) {
  Session session;
  session.audio().startSucceeds = false;
  auto config = sessionConfig();
  config.source.audio = true;
  std::string error;
  EXPECT_FALSE(session.start(config, MonitorCaptureSource{}, &error));
  EXPECT_EQ(error, "audio capture initialization failed");
  EXPECT_EQ(session.video().stops, 1u);
}

TEST_F(SessionTest, ReportsATargetThatIsNotListening) {
  holdPortSilently();
  if (IsSkipped()) return;
  Session session;
  std::string error;
  EXPECT_FALSE(session.start(sessionConfig(), MonitorCaptureSource{}, &error));
  EXPECT_THAT(error, testing::Not(testing::IsEmpty()));
  EXPECT_EQ(session->state(), SessionState::Error);
  ASSERT_FALSE(session.problems().empty());
  const auto problem = session.problems().back();
  EXPECT_EQ(problem.component, "network");
  EXPECT_THAT(problem.hint, HasSubstr("32100"));
  EXPECT_EQ(session.video().stops, 1u);
  EXPECT_EQ(session.audio().stops, 1u);
}

TEST_F(SessionTest, PassesTheSelectedSourceAndSinkToTheCaptureDevices) {
  bindReceiver(kHealthyWithAudio);
  if (IsSkipped()) return;
  Session session;
  auto config = sessionConfig();
  config.source.audio = true;
  config.source.audioSink = "chosen.monitor";
  std::string error;
  ASSERT_TRUE(session.start(config, WindowCaptureSource{4242}, &error)) << error;
  EXPECT_EQ(session.audio().lastSink, "chosen.monitor");
  ASSERT_TRUE(std::holds_alternative<WindowCaptureSource>(
      session.video().lastSource));
  EXPECT_EQ(std::get<WindowCaptureSource>(session.video().lastSource).id, 4242u);
  session->stop();
}

TEST_F(SessionTest, AnActiveStreamCannotBeStartedAgain) {
  bindReceiver();
  if (IsSkipped()) return;
  Session session;
  std::string error;
  ASSERT_TRUE(session.start(sessionConfig(), MonitorCaptureSource{}, &error))
      << error;
  ASSERT_TRUE(waitFor([&] { return session->state() == SessionState::Streaming; }));
  EXPECT_FALSE(session.start(sessionConfig(), MonitorCaptureSource{}, &error));
  EXPECT_EQ(error, "stream is already active");
  session->stop();
}

TEST_F(SessionTest, PublishesTheDocumentedStateSequence) {
  bindReceiver();
  if (IsSkipped()) return;
  Session session;
  std::string error;
  ASSERT_TRUE(session.start(sessionConfig(), MonitorCaptureSource{}, &error))
      << error;
  ASSERT_TRUE(waitFor([&] { return session->state() == SessionState::Streaming; }));
  session->stop();
  EXPECT_EQ(session->state(), SessionState::Idle);
  EXPECT_THAT(session.states(),
              testing::IsSupersetOf({SessionState::Starting,
                                     SessionState::Streaming,
                                     SessionState::Stopping,
                                     SessionState::Idle}));
}

// ---------------------------------------------------------------- streaming

TEST_F(SessionTest, StreamsFramesAndReportsProgress) {
  bindReceiver();
  if (IsSkipped()) return;
  Session session;
  std::string error;
  ASSERT_TRUE(session.start(sessionConfig(), MonitorCaptureSource{}, &error))
      << error;
  ASSERT_TRUE(waitFor([&] { return session->stats().sentFrames >= 3; }));
  const auto stats = session->stats();
  session->stop();

  EXPECT_GE(stats.capturedFrames, 3u);
  EXPECT_GE(stats.sentFrames, 3u);
  EXPECT_GT(stats.captureFps, 0);
  EXPECT_GT(stats.streamFps, 0);
  EXPECT_EQ(stats.audioSampleRate, 0u) << "audio was disabled";
  EXPECT_GE(mister->blits(), 3u);
  EXPECT_EQ(mister->audioPackets(), 0u);
  // Exactly one mode switch preceded the stream. Asserted before the first blit
  // because this test streams: past that point an uncompressed blit's payload
  // would be decoded as commands and could inflate the total (see FakeMister),
  // which is a build without liblz4 rather than a defect in the session.
  EXPECT_EQ(mister->switchModesBeforeFirstBlit(), 1u);
  EXPECT_TRUE(waitFor([&] { return mister->closes() >= 1; }));
}

TEST_F(SessionTest, TracksTransformCostAndResetsItOnRestart) {
  bindReceiver(kHealthy | kFrameskip);
  if (IsSkipped()) return;
  Session session;
  session.video().produce = false;
  auto config = sessionConfig();
  config.source.sampling = SamplingMode::LineBlend;
  std::string error;
  ASSERT_TRUE(session.start(config, MonitorCaptureSource{}, &error)) << error;
  auto stats = session->stats();
  EXPECT_EQ(stats.transformTimeUs, 0u);
  EXPECT_EQ(stats.transformMaxUs, 0u);

  session.video().produce = true;
  ASSERT_TRUE(waitFor([&] { return session->stats().sentFrames >= 3; }));
  stats = session->stats();
  EXPECT_GE(stats.transformMaxUs, stats.transformTimeUs);
  EXPECT_GT(stats.transport.fpgaStatusSamples, 0u);
  EXPECT_EQ(stats.transport.fpgaFallbackSamples,
            stats.transport.fpgaStatusSamples)
      << "this receiver always reports the VGA frameskip fallback";
  EXPECT_EQ(stats.transport.vramUnsyncedSamples, 0u);
  EXPECT_EQ(stats.transport.vramQueueEmptySamples, 0u);
  EXPECT_TRUE(stats.transport.vramSynced);
  EXPECT_TRUE(stats.transport.vgaFrameskip);
  EXPECT_TRUE(stats.transport.vramQueuePresent);

  session->stop();
  session.video().produce = false;
  ASSERT_TRUE(session.start(config, MonitorCaptureSource{}, &error)) << error;
  stats = session->stats();
  EXPECT_EQ(stats.transformTimeUs, 0u) << "counters reset on every start";
  EXPECT_EQ(stats.transformMaxUs, 0u);
  EXPECT_EQ(stats.capturedFrames, 0u);
  EXPECT_EQ(stats.sentFrames, 0u);
  session->stop();
}

TEST_F(SessionTest, AnInterlacedModeReachesTheAdaptiveDeliveryMargin) {
  bindReceiver(kHealthy);
  if (IsSkipped()) return;
  Session session;
  auto config = sessionConfig();
  config.modeline = {"480i", 12.336, 640, 662, 720, 784, 480, 488, 494, 525,
                     true};
  std::string error;
  ASSERT_TRUE(session.start(config, MonitorCaptureSource{}, &error)) << error;
  ASSERT_TRUE(waitFor([&] { return session->stats().sentFrames >= 3; }));
  const auto stats = session->stats();
  session->stop();

  EXPECT_TRUE(stats.transport.interlacedFieldBuffer);
  EXPECT_TRUE(stats.transport.adaptiveTimingEligible);
  EXPECT_EQ(stats.transport.deliveryReserveLines, 262);
  EXPECT_EQ(stats.transport.adaptiveLatestSafeLine, 263);
  EXPECT_GT(stats.transport.adaptiveHealthyAcks, 0u);
  EXPECT_EQ(stats.transport.adaptiveReductions, 0u);
  EXPECT_EQ(stats.transport.adaptiveResets, 0u);
  EXPECT_EQ(mister->lastInterlaceMode(), 1) << "alternating field buffer";
}

TEST_F(SessionTest, TheProgressiveInterlaceBufferSendsFullHeightFrames) {
  bindReceiver(kHealthy);
  if (IsSkipped()) return;
  Session session;
  auto config = sessionConfig();
  config.modeline = {"480i", 12.336, 640, 662, 720, 784, 480, 488, 494, 525,
                     true};
  config.source.progressiveInterlaceBuffer = true;
  std::string error;
  ASSERT_TRUE(session.start(config, MonitorCaptureSource{}, &error)) << error;
  ASSERT_TRUE(waitFor([&] { return session->stats().sentFrames >= 3; }));
  const auto stats = session->stats();
  session->stop();
  EXPECT_FALSE(stats.transport.interlacedFieldBuffer);
  EXPECT_EQ(mister->lastInterlaceMode(), 2);
  EXPECT_THAT(mister->fields(), testing::Each(uint8_t(0)))
      << "a single framebuffer never alternates the field index";
}

TEST_F(SessionTest, CaptureIsPacedAgainstSendingSoNoBacklogCanForm) {
  bindReceiver();
  if (IsSkipped()) return;
  Session session;
  // Transient capture failures, which is when the capture loop re-requests a
  // frame of its own accord and could otherwise run ahead of the sender.
  session.video().failEvery = 3;
  std::string error;
  ASSERT_TRUE(session.start(sessionConfig(), MonitorCaptureSource{}, &error))
      << error;
  ASSERT_TRUE(waitFor([&] { return session->stats().sentFrames >= 20; }));
  const auto stats = session->stats();
  session->stop();

  // One capture is requested per blit, so at most one finished frame is ever
  // waiting; a captured frame is never queued behind another.
  EXPECT_LE(stats.capturedFrames, stats.sentFrames + 2)
      << "captured " << stats.capturedFrames << " for " << stats.sentFrames
      << " sent: capture must not outrun the paced sender";
  EXPECT_GT(stats.sentFrames, 0u);
}

TEST_F(SessionTest, CountsACapturedFrameThatIsReplacedBeforeItIsSent) {
  bindReceiver();
  if (IsSkipped()) return;
  Session session;
  // A small source keeps the two captures below well inside one blit period.
  session.video().width = 320;
  session.video().height = 240;
  session.video().gated = true;
  auto config = sessionConfig();
  config.source.crop = CropMode::X1;
  std::string error;
  ASSERT_TRUE(session.start(config, MonitorCaptureSource{}, &error)) << error;
  session.video().release(1);  // the sender blocks until its first frame

  // One capture is requested per blit, but the request is a flag, not a queue,
  // and it is set again at the end of every blit whether or not a frame arrived.
  // So a capture that stalls past a blit and then delivers two frames quickly —
  // an X11 hiccup followed by a fast frame — finds the flag already set and
  // publishes twice before the sender picks either one up. The first of the two
  // is the frame that is dropped.
  uint64_t dropped = 0;
  for (int attempt = 0; attempt < 20 && !dropped; ++attempt) {
    ASSERT_TRUE(waitFor([&] { return session.video().parked(); }));
    const auto sent = session->stats().sentFrames;
    ASSERT_TRUE(waitFor([&] { return session->stats().sentFrames > sent + 1; }))
        << "the sender must blit past the stall and re-request a capture";
    session.video().release(2);
    ASSERT_TRUE(waitFor([&] { return session.video().parked(); }));
    dropped = session->droppedFrames();
  }
  session->stop();
  EXPECT_GT(dropped, 0u) << "a replaced capture must be counted as dropped";
}

TEST_F(SessionTest, RecomputesTheCropWhenTheMonitorIsResized) {
  bindReceiver();
  if (IsSkipped()) return;
  Session session;
  std::string error;
  // 4:3 of a 1080-tall monitor is 1440x1080.
  ASSERT_TRUE(session.start(sessionConfig(), MonitorCaptureSource{}, &error))
      << error;
  ASSERT_TRUE(waitFor([&] { return session.video().captured >= 3; }));

  session.video().height = 720;  // 4:3 of 720 is 960x720
  const auto before = session.video().captured.load();
  ASSERT_TRUE(waitFor([&] { return session.video().captured >= before + 3; }));
  session->stop();

  const auto regions = session.video().capturedRegions();
  ASSERT_GE(regions.size(), 2u);
  EXPECT_EQ(regions.front().width, 1440u);
  EXPECT_EQ(regions.front().height, 1080u);
  EXPECT_EQ(regions.back().width, 960u);
  EXPECT_EQ(regions.back().height, 720u);
}

TEST_F(SessionTest, KeepsStreamingWhileCaptureIsTemporarilyUnavailable) {
  bindReceiver();
  if (IsSkipped()) return;
  Session session;
  std::string error;
  ASSERT_TRUE(session.start(sessionConfig(), MonitorCaptureSource{}, &error))
      << error;
  ASSERT_TRUE(waitFor([&] { return session->stats().sentFrames >= 2; }));

  // A transient capture failure must cost frames, not the session.
  session.video().produce = false;
  const auto blits = mister->blits();
  EXPECT_TRUE(waitFor([&] { return mister->blits() > blits + 2; }))
      << "the previous frame is resent while capture is unavailable";
  EXPECT_EQ(session->state(), SessionState::Streaming);

  session.video().produce = true;
  const auto captured = session.video().captured.load();
  EXPECT_TRUE(waitFor([&] { return session.video().captured > captured; }));
  EXPECT_EQ(session->state(), SessionState::Streaming);
  session->stop();
}

TEST_F(SessionTest, AFatalCaptureErrorFailsTheSession) {
  bindReceiver();
  if (IsSkipped()) return;
  Session session;
  std::string error;
  ASSERT_TRUE(session.start(sessionConfig(), MonitorCaptureSource{}, &error))
      << error;
  ASSERT_TRUE(waitFor([&] { return session->stats().sentFrames >= 1; }));

  session.video().produce = false;
  session.video().reportError = true;
  ASSERT_TRUE(waitFor([&] { return session->state() == SessionState::Error; }));
  const auto problems = session.problems();
  ASSERT_FALSE(problems.empty());
  EXPECT_EQ(problems.back().component, "video");
  session->stop();
  EXPECT_EQ(session->state(), SessionState::Idle);
}

TEST_F(SessionTest, PublishesPreviewFramesAtAThrottledRate) {
  bindReceiver();
  if (IsSkipped()) return;
  Session session;
  std::atomic<uint32_t> previews{0};
  std::atomic<uint32_t> lastWidth{0};
  session->setPreviewCallback([&](const Frame& frame) {
    lastWidth = frame.width;
    ++previews;
  });
  auto config = sessionConfig();
  config.source.preview = true;
  std::string error;
  ASSERT_TRUE(session.start(config, MonitorCaptureSource{}, &error)) << error;
  ASSERT_TRUE(waitFor([&] { return previews >= 2; }));
  const auto captured = session.video().captured.load();
  const auto seen = previews.load();
  session->stop();
  EXPECT_EQ(lastWidth, 1440u);
  EXPECT_LT(seen, captured)
      << "preview is throttled to roughly 10 FPS, not one per captured frame";
}

TEST_F(SessionTest, PreviewIsSilentWhenTheSettingIsOff) {
  bindReceiver();
  if (IsSkipped()) return;
  Session session;
  std::atomic<uint32_t> previews{0};
  session->setPreviewCallback([&](const Frame&) { ++previews; });
  std::string error;
  ASSERT_TRUE(session.start(sessionConfig(), MonitorCaptureSource{}, &error))
      << error;
  ASSERT_TRUE(waitFor([&] { return session->stats().sentFrames >= 3; }));
  session->stop();
  EXPECT_EQ(previews, 0u);
}

// ------------------------------------------------------------------ live switch

TEST_F(SessionTest, SwitchesTimingsLiveAndFollowsTheNewActiveArea) {
  bindReceiver();
  if (IsSkipped()) return;
  Session session;
  auto config = sessionConfig();
  config.source.crop = CropMode::X1;  // 1x crop tracks the active area
  config.modeline = Modeline::safeDefault();  // 320x240
  std::string error;
  ASSERT_TRUE(session.start(config, MonitorCaptureSource{}, &error)) << error;
  ASSERT_TRUE(waitFor([&] { return session.video().captured >= 3; }));

  const Modeline vga{"vga", 25.175, 640, 656, 752, 800, 480, 490, 492, 525,
                     false};
  ASSERT_TRUE(session->updateModeline(vga, false, &error)) << error;
  ASSERT_TRUE(waitFor([&] {
    const auto regions = session.video().capturedRegions();
    return !regions.empty() && regions.back().width == 640;
  }));
  EXPECT_EQ(session->state(), SessionState::Streaming);
  const auto blits = mister->blits();
  session->stop();

  EXPECT_GT(blits, 0u);
  const auto regions = session.video().capturedRegions();
  EXPECT_EQ(regions.front().width, 320u);
  EXPECT_EQ(regions.front().height, 240u);
  EXPECT_EQ(regions.back().width, 640u);
  EXPECT_EQ(regions.back().height, 480u);
  EXPECT_GE(mister->switchModes(), 2u);
  EXPECT_THAT(mister->activeHeights(), testing::Contains(480));
}

TEST_F(SessionTest, RefusesToSwitchToTimingsTheProtocolCannotStream) {
  bindReceiver();
  if (IsSkipped()) return;
  Session session;
  std::string error;
  ASSERT_TRUE(session.start(sessionConfig(), MonitorCaptureSource{}, &error))
      << error;
  ASSERT_TRUE(waitFor([&] { return session->state() == SessionState::Streaming; }));

  auto broken = Modeline::safeDefault();
  broken.vTotal = 1;
  EXPECT_FALSE(session->updateModeline(broken, false, &error));
  EXPECT_THAT(error, HasSubstr("vertical timings"));
  EXPECT_EQ(session->state(), SessionState::Streaming);
  session->stop();
}

TEST_F(SessionTest, RefusesToSwitchTimingsWhileIdle) {
  Session session;
  std::string error;
  EXPECT_FALSE(session->updateModeline(Modeline::safeDefault(), false, &error));
  EXPECT_EQ(error, "stream is not active");
  EXPECT_TRUE(session->updateModeline(Modeline::safeDefault(), false, nullptr) ==
              false);
}

// ---------------------------------------------------------------------- audio

TEST_F(SessionTest, NegotiatesNoAudioAtAllWhenAudioIsDisabled) {
  bindReceiver(kHealthyWithAudio);
  if (IsSkipped()) return;
  Session session(/*withAudio=*/true);
  auto config = sessionConfig();
  config.source.audio = false;
  std::string error;
  ASSERT_TRUE(session.start(config, MonitorCaptureSource{}, &error)) << error;
  ASSERT_TRUE(waitFor([&] { return mister->blits() >= 2; }));
  session->stop();
  EXPECT_EQ(session.audio().starts, 0u);
  EXPECT_EQ(mister->audioPackets(), 0u);
  EXPECT_EQ(mister->initRateCode(), 0)
      << "a disabled session must negotiate rate code 0";
  EXPECT_EQ(mister->initChannelCode(), 0);
}

TEST_F(SessionTest, NegotiatesStereoAndSendsPcmWhenTheCoreHasAudioOn) {
  bindReceiver(kHealthyWithAudio);
  if (IsSkipped()) return;
  Session session(/*withAudio=*/true);
  auto config = sessionConfig();
  config.source.audio = true;
  std::string error;
  ASSERT_TRUE(session.start(config, MonitorCaptureSource{}, &error)) << error;
  EXPECT_EQ(mister->initRateCode(), 3) << "48 kHz";
  EXPECT_EQ(mister->initChannelCode(), 2);
  ASSERT_TRUE(waitFor([&] { return mister->audioPackets() > 0; }));
  ASSERT_TRUE(waitFor([&] { return session->stats().audioPeak > 0; }));
  const auto stats = session->stats();
  session->stop();
  EXPECT_EQ(stats.audioSampleRate, 48000u);
  EXPECT_TRUE(stats.misterAudioEnabled);
  EXPECT_GT(mister->audioBytes(), 0u);
}

TEST_F(SessionTest, SendsNoAudioAtAllWhileTheCoreReportsAudioOff) {
  bindReceiver(kHealthy);  // core audio bit clear
  if (IsSkipped()) return;
  Session session(/*withAudio=*/true);
  auto config = sessionConfig();
  config.source.audio = true;
  std::string error;
  ASSERT_TRUE(session.start(config, MonitorCaptureSource{}, &error)) << error;
  // Enough frames that audio would certainly have been sent if it were going to
  // be, and enough that the ring would have grown unboundedly if undrained.
  ASSERT_TRUE(waitFor([&] { return session.video().captured >= 30; }));
  const auto stats = session->stats();
  session->stop();
  EXPECT_EQ(mister->audioPackets(), 0u);
  EXPECT_LT(stats.audioBufferedSamples, 32000u)
      << "the ring must be drained rather than allowed to grow";
}

TEST_F(SessionTest, StartsSendingAudioWhenTheCoreTurnsItOnMidStream) {
  bindReceiver(kHealthy);
  if (IsSkipped()) return;
  Session session(/*withAudio=*/true);
  auto config = sessionConfig();
  config.source.audio = true;
  std::string error;
  ASSERT_TRUE(session.start(config, MonitorCaptureSource{}, &error)) << error;
  ASSERT_TRUE(waitFor([&] { return session.video().captured >= 10; }));
  ASSERT_EQ(mister->audioPackets(), 0u);

  mister->setStatusBits(kHealthyWithAudio);
  EXPECT_TRUE(waitFor([&] { return mister->audioPackets() > 0; }))
      << "audio must start bounded rather than seconds behind";
  session->stop();
}

TEST_F(SessionTest, DropsTheOldestSamplesWhenAudioArrivesFasterThanItIsSent) {
  bindReceiver(kHealthyWithAudio);
  if (IsSkipped()) return;
  Session session(/*withAudio=*/true);
  // Far more PCM than one video frame's worth, delivered with no delay.
  session.audio().valuesPerBlock = 9600;
  session.audio().blockDelayMs = 0;
  auto config = sessionConfig();
  config.source.audio = true;
  std::string error;
  ASSERT_TRUE(session.start(config, MonitorCaptureSource{}, &error)) << error;
  EXPECT_TRUE(waitFor([&] { return session->stats().audioDroppedSamples > 0; }))
      << "audio latency must stay bounded instead of growing until overrun";
  const auto stats = session->stats();
  session->stop();
  EXPECT_LT(stats.audioBufferedSamples, 200000u);
}

TEST_F(SessionTest, InsertsSilenceWhenNoPcmIsAvailable) {
  bindReceiver(kHealthyWithAudio);
  if (IsSkipped()) return;
  Session session(/*withAudio=*/true);
  // A trickle: enough to pass the prebuffer, then far too little to keep up.
  session.audio().valuesPerBlock = 4000;
  session.audio().blockDelayMs = 0;
  auto config = sessionConfig();
  config.source.audio = true;
  std::string error;
  ASSERT_TRUE(session.start(config, MonitorCaptureSource{}, &error)) << error;
  ASSERT_TRUE(waitFor([&] { return mister->audioPackets() > 0; }));
  session.audio().produce = false;
  EXPECT_TRUE(waitFor([&] { return session->stats().audioUnderrunSamples > 0; }))
      << "underrun must be filled with silence and accounted for";
  session->stop();
}

TEST_F(SessionTest, MeasuresThePeakOfTheLoudestPossibleSample) {
  bindReceiver(kHealthyWithAudio);
  if (IsSkipped()) return;
  Session session(/*withAudio=*/true);
  session.audio().sampleValue = INT16_MIN;  // magnitude 32768, not 32767
  auto config = sessionConfig();
  config.source.audio = true;
  std::string error;
  ASSERT_TRUE(session.start(config, MonitorCaptureSource{}, &error)) << error;
  ASSERT_TRUE(waitFor([&] { return session->stats().audioPeak > 0; }));
  EXPECT_DOUBLE_EQ(session->stats().audioPeak, 1.0);
  session->stop();
}

TEST_F(SessionTest, AdoptsTheRateTheAudioDeviceActuallyOpened) {
  bindReceiver(kHealthyWithAudio);
  if (IsSkipped()) return;
  Session session(/*withAudio=*/true);
  session.audio().rate = 44100;
  auto config = sessionConfig();
  config.source.audio = true;
  std::string error;
  ASSERT_TRUE(session.start(config, MonitorCaptureSource{}, &error)) << error;
  EXPECT_EQ(mister->initRateCode(), 2) << "44.1 kHz";
  EXPECT_EQ(session->stats().audioSampleRate, 44100u);
  session->stop();
}

// ------------------------------------------------------------------- shutdown

TEST_F(SessionTest, StopIsIdempotentAndSafeWhileIdle) {
  Session session;
  session->stop();
  session->stop();
  EXPECT_EQ(session->state(), SessionState::Idle);

  bindReceiver();
  if (IsSkipped()) return;
  std::string error;
  ASSERT_TRUE(session.start(sessionConfig(), MonitorCaptureSource{}, &error))
      << error;
  ASSERT_TRUE(waitFor([&] { return session->stats().sentFrames >= 1; }));
  session->stop();
  ASSERT_EQ(session->state(), SessionState::Idle);
  const auto stopsAfterFirst = session.video().stops.load();
  ASSERT_EQ(stopsAfterFirst, 1u) << "stopping a live stream releases capture";
  session->stop();
  EXPECT_EQ(session->state(), SessionState::Idle);
  EXPECT_EQ(session.video().stops, stopsAfterFirst)
      << "a second stop on an idle session must be a no-op";
}

TEST_F(SessionTest, StoppingSendsTheCloseCommandAndJoinsEveryWorker) {
  bindReceiver(kHealthyWithAudio);
  if (IsSkipped()) return;
  Session session(/*withAudio=*/true);
  auto config = sessionConfig();
  config.source.audio = true;
  std::string error;
  ASSERT_TRUE(session.start(config, MonitorCaptureSource{}, &error)) << error;
  ASSERT_TRUE(waitFor([&] { return session->stats().sentFrames >= 2; }));
  session->stop();
  EXPECT_TRUE(waitFor([&] { return mister->closes() >= 1; }));
  EXPECT_EQ(session.audio().stops, 1u);
  // A stopped session reports no live transport state.
  const auto stats = session->stats();
  EXPECT_FALSE(stats.misterAudioEnabled);
  EXPECT_EQ(stats.transport.pathMtu, 0u);
  EXPECT_FALSE(stats.transport.vramSynced);
}

TEST_F(SessionTest, DestructionStopsARunningStream) {
  bindReceiver();
  if (IsSkipped()) return;
  {
    Session session;
    std::string error;
    ASSERT_TRUE(session.start(sessionConfig(), MonitorCaptureSource{}, &error))
        << error;
    ASSERT_TRUE(waitFor([&] { return session->stats().sentFrames >= 1; }));
  }
  EXPECT_TRUE(waitFor([&] { return mister->closes() >= 1; }));
}

TEST_F(SessionTest, RestartsCleanlyAfterAFailedStart) {
  holdPortSilently();
  if (IsSkipped()) return;
  Session session;
  std::string error;
  // Fails because nothing is listening yet.
  ASSERT_FALSE(session.start(sessionConfig(), MonitorCaptureSource{}, &error));
  ASSERT_EQ(session->state(), SessionState::Error);

  bindReceiver();
  if (IsSkipped()) return;
  ASSERT_TRUE(session.start(sessionConfig(), MonitorCaptureSource{}, &error))
      << error;
  EXPECT_TRUE(waitFor([&] { return session->stats().sentFrames >= 1; }));
  session->stop();
}

// start() takes both the callback and the error string by optional pointer, and
// the CLI's pattern mode and the destructor path both leave them out.
TEST_F(SessionTest, WorksWithNeitherACallbackNorAnErrorPointer) {
  bindReceiver();
  if (IsSkipped()) return;
  auto video = std::make_unique<FakeVideo>();
  auto* raw = video.get();
  StreamSession session(std::move(video), std::make_unique<FakeAudio>());
  ASSERT_TRUE(session.start(sessionConfig(), MonitorCaptureSource{}));
  EXPECT_TRUE(waitFor([&] { return session.stats().sentFrames >= 1; }));
  EXPECT_EQ(session.state(), SessionState::Streaming);
  EXPECT_FALSE(session.start(sessionConfig(), MonitorCaptureSource{}))
      << "already streaming, and there is no error string to fill in";
  auto broken = Modeline::safeDefault();
  broken.vTotal = 1;
  EXPECT_FALSE(session.updateModeline(broken, false));
  EXPECT_TRUE(session.updateModeline(Modeline::safeDefault(), false));
  session.stop();
  EXPECT_EQ(session.state(), SessionState::Idle);
  EXPECT_GT(raw->captured, 0u);
}

TEST_F(SessionTest, EveryFailurePathIsSafeWithoutACallbackOrErrorPointer) {
  holdPortSilently();
  if (IsSkipped()) return;
  {  // configuration the protocol cannot stream
    StreamSession session(std::make_unique<FakeVideo>(),
                          std::make_unique<FakeAudio>());
    auto config = sessionConfig();
    config.modeline.vTotal = 1;
    EXPECT_FALSE(session.start(config, MonitorCaptureSource{}));
  }
  {  // no target
    StreamSession session(std::make_unique<FakeVideo>(),
                          std::make_unique<FakeAudio>());
    auto config = sessionConfig();
    config.target.clear();
    EXPECT_FALSE(session.start(config, MonitorCaptureSource{}));
  }
  {  // video capture refuses to open
    auto video = std::make_unique<FakeVideo>();
    video->startSucceeds = false;
    StreamSession session(std::move(video), std::make_unique<FakeAudio>());
    EXPECT_FALSE(session.start(sessionConfig(), MonitorCaptureSource{}));
  }
  {  // the crop cannot be computed
    auto video = std::make_unique<FakeVideo>();
    video->width = 0;
    video->height = 0;
    StreamSession session(std::move(video), std::make_unique<FakeAudio>());
    EXPECT_FALSE(session.start(sessionConfig(), MonitorCaptureSource{}));
  }
  {  // audio capture refuses to open
    auto audio = std::make_unique<FakeAudio>();
    audio->startSucceeds = false;
    auto config = sessionConfig();
    config.source.audio = true;
    StreamSession session(std::make_unique<FakeVideo>(), std::move(audio));
    EXPECT_FALSE(session.start(config, MonitorCaptureSource{}));
  }
  {  // nothing listening on the protocol port
    StreamSession session(std::make_unique<FakeVideo>(),
                          std::make_unique<FakeAudio>());
    EXPECT_FALSE(session.start(sessionConfig(), MonitorCaptureSource{}));
    EXPECT_EQ(session.state(), SessionState::Error);
    // A live modeline switch is refused, again with no error pointer.
    EXPECT_FALSE(session.updateModeline(Modeline::safeDefault(), false));
  }
}

TEST_F(SessionTest, FailsTheStreamWhenTheTargetStopsListening) {
  bindReceiver();
  if (IsSkipped()) return;
  Session session;
  std::string error;
  ASSERT_TRUE(session.start(sessionConfig(), MonitorCaptureSource{}, &error))
      << error;
  ASSERT_TRUE(waitFor([&] { return session->stats().sentFrames >= 2; }));

  // The receiver disappearing makes the socket report the resulting ICMP error
  // on a later send, which has to end the session rather than stream into a void.
  mister.reset();
  ASSERT_TRUE(waitFor([&] { return session->state() == SessionState::Error; }));
  const auto problems = session.problems();
  ASSERT_FALSE(problems.empty());
  EXPECT_THAT(problems.back().component,
              testing::AnyOf("stream", "audio", "video"));
  session->stop();
  EXPECT_EQ(session->state(), SessionState::Idle);
}

TEST_F(SessionTest, ReportsAnAudioSendFailureAsAnAudioProblem) {
  bindReceiver(kHealthyWithAudio);
  if (IsSkipped()) return;
  Session session(/*withAudio=*/true);
  auto config = sessionConfig();
  config.source.audio = true;
  std::string error;
  ASSERT_TRUE(session.start(config, MonitorCaptureSource{}, &error)) << error;
  ASSERT_TRUE(waitFor([&] { return mister->audioPackets() > 0; }));

  // Audio goes out before its video frame, so the receiver disappearing is felt
  // on the audio command first.
  mister.reset();
  ASSERT_TRUE(waitFor([&] { return session->state() == SessionState::Error; }));
  const auto problems = session.problems();
  ASSERT_FALSE(problems.empty());
  EXPECT_THAT(problems.back().component, testing::AnyOf("audio", "stream"));
  EXPECT_THAT(problems.back().hint, testing::Not(testing::IsEmpty()));
  session->stop();
}

TEST_F(SessionTest, KeepsTheWorkingCropWhenTheSourceGeometryBecomesUnusable) {
  bindReceiver();
  if (IsSkipped()) return;
  Session session;
  std::string error;
  ASSERT_TRUE(session.start(sessionConfig(), MonitorCaptureSource{}, &error))
      << error;
  ASSERT_TRUE(waitFor([&] { return session->stats().sentFrames >= 2; }));
  const auto regionsBefore = session.video().capturedRegions().size();

  // A monitor reporting zero size cannot produce a crop; the previous one has to
  // stay in force rather than the stream collapsing.
  session.video().width = 0;
  session.video().height = 0;
  const auto sent = session->stats().sentFrames;
  EXPECT_TRUE(waitFor([&] { return session->stats().sentFrames > sent + 1; }));
  EXPECT_EQ(session->state(), SessionState::Streaming);
  EXPECT_EQ(session.video().capturedRegions().size(), regionsBefore)
      << "no new region may be set from an unusable geometry";
  session->stop();
}

TEST_F(SessionTest, FailsWhenNoFrameIsCapturedForFiveSeconds) {
  bindReceiver();
  if (IsSkipped()) return;
  Session session;
  session.video().produce = false;
  std::string error;
  ASSERT_TRUE(session.start(sessionConfig(), MonitorCaptureSource{}, &error))
      << error;
  // The rendering thread blocks for the very first frame only, and gives up
  // after five seconds rather than hanging forever.
  ASSERT_TRUE(waitFor([&] { return session->state() == SessionState::Error; },
                      std::chrono::seconds(20)));
  const auto problems = session.problems();
  ASSERT_FALSE(problems.empty());
  EXPECT_EQ(problems.back().component, "video");
  EXPECT_THAT(problems.back().message, HasSubstr("within 5 seconds"));
  session->stop();
}

}  // namespace
