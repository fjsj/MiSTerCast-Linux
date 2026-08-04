#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <pulse/error.h>
#include <pulse/simple.h>

#include "mistercast/interfaces.hpp"
#include "support/pulse_server.hpp"
#include "support/scoped_environment.hpp"
#include "support/temp_directory.hpp"

using namespace mistercast;
using namespace mistercast::test;
using mistercast::test::ScopedEnvironment;
using testing::HasSubstr;

namespace {

// These tests talk to a real PulseAudio server, because the behaviour they check
// — which source a sink maps to, what rate the server grants, how large a read
// comes back, what happens to playback routing — only exists in a real server.
//
// The server is a private one started by this binary with a single null sink, so
// the silent-output tests can move the default sink and live streams around
// without disturbing whoever is using the desktop. When pulseaudio cannot be
// started the whole binary exits 77 and CTest records a skip.
std::unique_ptr<TemporaryDirectory> serverDirectory;
std::unique_ptr<PrivatePulseServer> server;

bool startPrivateServer() {
  serverDirectory = std::make_unique<TemporaryDirectory>("pulse-server");
  server = std::make_unique<PrivatePulseServer>(serverDirectory->path());
  if (!server->ready()) return false;
  ::setenv("PULSE_SERVER", server->address().c_str(), 1);
  std::string error;
  pulseAudioSinks(error);
  return error.empty();
}

// A playback stream on the test server, so the silent-output path has a live
// sink input to move and to move back. Without one, the loops that relocate
// existing playback never run.
class PlaybackStream {
 public:
  PlaybackStream() {
    pa_sample_spec spec{PA_SAMPLE_S16LE, 48000, 2};
    int error = 0;
    stream_ = pa_simple_new(nullptr, "MiSTerCast test playback",
                            PA_STREAM_PLAYBACK, nullptr, "silence", &spec,
                            nullptr, nullptr, &error);
    if (stream_) {
      const std::vector<int16_t> silence(960, 0);
      pa_simple_write(stream_, silence.data(),
                      silence.size() * sizeof(int16_t), &error);
    }
  }
  ~PlaybackStream() {
    if (stream_) pa_simple_free(stream_);
  }
  PlaybackStream(const PlaybackStream&) = delete;
  PlaybackStream& operator=(const PlaybackStream&) = delete;
  bool open() const noexcept { return stream_ != nullptr; }

 private:
  pa_simple* stream_{};
};

class Pulse : public testing::Test {
 protected:
  static std::vector<AudioSink> sinks() {
    std::string error;
    auto result = pulseAudioSinks(error);
    EXPECT_THAT(error, testing::IsEmpty());
    return result;
  }
};

TEST_F(Pulse, EnumeratesTheServersOutputSinks) {
  std::string error;
  const auto outputs = pulseAudioSinks(error);
  EXPECT_THAT(error, testing::IsEmpty());
  ASSERT_FALSE(outputs.empty()) << "a reachable server must expose a sink";
  for (const auto& sink : outputs) {
    EXPECT_FALSE(sink.name.empty());
    EXPECT_FALSE(sink.description.empty())
        << "the chooser falls back to the name when there is no description";
  }
  EXPECT_THAT(outputs, testing::Contains(testing::Field(
                           &AudioSink::name, "mistercast_test_output")));
  EXPECT_THAT(outputs, testing::Contains(testing::Field(
                           &AudioSink::description,
                           "MiSTerCast_Test_Output")));
  EXPECT_LE(std::count_if(outputs.begin(), outputs.end(),
                          [](const AudioSink& sink) { return sink.isDefault; }),
            1)
      << "at most one sink can be the server default";
}

TEST_F(Pulse, CapturesTheDefaultOutputsMonitorSource) {
  auto capture = makePulseAudioCapture();
  std::vector<SessionError> problems;
  ASSERT_TRUE(capture->start({}, [&](SessionError error) {
    problems.push_back(error);
  })) << (problems.empty() ? std::string() : problems.front().message);
  EXPECT_THAT(problems, testing::IsEmpty());

  const auto rate = capture->sampleRate();
  EXPECT_THAT(rate, testing::AnyOf(48000u, 44100u, 22050u));

  PcmBlock block;
  ASSERT_TRUE(capture->next(block, std::chrono::milliseconds(100)));
  EXPECT_EQ(block.sampleRate, rate);
  // A 10 ms stereo read, which is what the fragment size is asked to be.
  EXPECT_EQ(block.samples.size(), size_t(rate) / 100 * 2);
  EXPECT_GT(block.timestampNs, 0u);

  const auto firstTimestamp = block.timestampNs;
  ASSERT_TRUE(capture->next(block, std::chrono::milliseconds(100)));
  EXPECT_GE(block.timestampNs, firstTimestamp);

  capture->stop();
  EXPECT_FALSE(capture->next(block, std::chrono::milliseconds(100)))
      << "a stopped capture produces nothing";
  capture->stop() /* idempotent */;
}

TEST_F(Pulse, ReadsAreBoundedByTenMillisecondsHoweverLongTheTimeoutIs) {
  auto capture = makePulseAudioCapture();
  ASSERT_TRUE(capture->start({}, {}));
  const auto rate = capture->sampleRate();
  PcmBlock block;
  // A burst-sized read is what put audio a second behind video, so the block
  // size must not scale with the caller's timeout.
  ASSERT_TRUE(capture->next(block, std::chrono::milliseconds(5000)));
  EXPECT_EQ(block.samples.size(), size_t(rate) / 100 * 2);
  capture->stop();
}

TEST_F(Pulse, ANonPositiveTimeoutReadsNothing) {
  auto capture = makePulseAudioCapture();
  ASSERT_TRUE(capture->start({}, {}));
  PcmBlock block;
  EXPECT_FALSE(capture->next(block, std::chrono::milliseconds(0)));
  EXPECT_FALSE(capture->next(block, std::chrono::milliseconds(-1)));
  capture->stop();
}

TEST_F(Pulse, ReadingBeforeStartingProducesNothing) {
  auto capture = makePulseAudioCapture();
  PcmBlock block;
  EXPECT_FALSE(capture->next(block, std::chrono::milliseconds(100)));
  EXPECT_EQ(capture->sampleRate(), 48000u) << "the nominal rate before opening";
}

TEST_F(Pulse, CapturesANamedSinksMonitorWithoutChangingRouting) {
  const auto outputs = sinks();
  ASSERT_FALSE(outputs.empty());
  std::string error;
  const auto before = pulseAudioSinks(error);

  auto capture = makePulseAudioCapture();
  std::vector<SessionError> problems;
  ASSERT_TRUE(capture->start(outputs.front().name, [&](SessionError problem) {
    problems.push_back(problem);
  })) << (problems.empty() ? std::string() : problems.front().message);
  PcmBlock block;
  EXPECT_TRUE(capture->next(block, std::chrono::milliseconds(200)));
  capture->stop();

  const auto after = pulseAudioSinks(error);
  ASSERT_EQ(after.size(), before.size())
      << "capturing a monitor must not add or remove sinks";
  for (size_t i = 0; i < after.size(); ++i) {
    EXPECT_EQ(after[i].name, before[i].name);
    EXPECT_EQ(after[i].isDefault, before[i].isDefault)
        << "playback routing must be left alone";
  }
}

TEST_F(Pulse, ReportsASinkThatHasNoMonitorSource) {
  auto capture = makePulseAudioCapture();
  std::optional<SessionError> problem;
  EXPECT_FALSE(capture->start("mistercast.sink.does.not.exist",
                              [&](SessionError error) { problem = error; }));
  ASSERT_TRUE(problem.has_value());
  EXPECT_EQ(problem->component, "audio");
  EXPECT_THAT(problem->message, HasSubstr("no monitor source"));
  EXPECT_THAT(problem->hint, HasSubstr("disable audio"));
}

TEST_F(Pulse, RestartsCleanlyOnTheSameInstance) {
  auto capture = makePulseAudioCapture();
  for (int attempt = 0; attempt < 2; ++attempt) {
    ASSERT_TRUE(capture->start({}, {})) << "attempt " << attempt;
    PcmBlock block;
    EXPECT_TRUE(capture->next(block, std::chrono::milliseconds(200)));
    capture->stop();
  }
}

// Safe to run because the server is private to this binary: the default sink it
// moves around is the test server's, never the desktop's.
TEST_F(Pulse, TheSilentSinkReroutesPlaybackAndRestoresItOnStop) {
  std::string error;
  const auto before = pulseAudioSinks(error);
  ASSERT_THAT(error, testing::IsEmpty());
  const auto defaultBefore =
      std::find_if(before.begin(), before.end(),
                   [](const AudioSink& sink) { return sink.isDefault; });
  ASSERT_NE(defaultBefore, before.end()) << "no default sink to restore";

  auto capture = makePulseAudioCapture();
  std::optional<SessionError> problem;
  ASSERT_TRUE(capture->start(SilentAudioSink,
                             [&](SessionError value) { problem = value; }))
      << (problem ? problem->message : std::string());

  const auto during = pulseAudioSinks(error);
  EXPECT_EQ(during.size(), before.size() + 1)
      << "a null sink is loaded for the duration";
  const auto silent =
      std::find_if(during.begin(), during.end(), [](const AudioSink& sink) {
        return sink.name.rfind("mistercast_silent_", 0) == 0;
      });
  ASSERT_NE(silent, during.end());
  EXPECT_TRUE(silent->isDefault) << "new playback has to land in the stream";

  PcmBlock block;
  EXPECT_TRUE(capture->next(block, std::chrono::milliseconds(200)));
  capture->stop();

  const auto after = pulseAudioSinks(error);
  EXPECT_EQ(after.size(), before.size()) << "the null sink is unloaded again";
  const auto defaultAfter =
      std::find_if(after.begin(), after.end(),
                   [](const AudioSink& sink) { return sink.isDefault; });
  ASSERT_NE(defaultAfter, after.end());
  EXPECT_EQ(defaultAfter->name, defaultBefore->name)
      << "the original output must be restored";
}

TEST_F(Pulse, TheSilentSinkMovesLivePlaybackAndPutsItBack) {
  PlaybackStream playback;
  ASSERT_TRUE(playback.open()) << "could not open a playback stream to move";

  std::string error;
  const auto before = pulseAudioSinks(error);
  const auto defaultBefore =
      std::find_if(before.begin(), before.end(),
                   [](const AudioSink& sink) { return sink.isDefault; });
  ASSERT_NE(defaultBefore, before.end());

  auto capture = makePulseAudioCapture();
  std::optional<SessionError> problem;
  ASSERT_TRUE(capture->start(SilentAudioSink,
                             [&](SessionError value) { problem = value; }))
      << (problem ? problem->message : std::string());
  PcmBlock block;
  EXPECT_TRUE(capture->next(block, std::chrono::milliseconds(200)));
  capture->stop();

  const auto after = pulseAudioSinks(error);
  EXPECT_EQ(after.size(), before.size());
  const auto defaultAfter =
      std::find_if(after.begin(), after.end(),
                   [](const AudioSink& sink) { return sink.isDefault; });
  ASSERT_NE(defaultAfter, after.end());
  EXPECT_EQ(defaultAfter->name, defaultBefore->name)
      << "the playback stream and the default output are both put back";
}

TEST_F(Pulse, ReportsAServerWithNoOutputAtAll) {
  TemporaryDirectory directory("pulse-no-sink");
  PrivatePulseServer sinkless(directory.path(), /*withNullSink=*/false);
  if (!sinkless.ready())
    GTEST_SKIP() << "a private pulseaudio server could not be started";
  const ScopedEnvironment address("PULSE_SERVER", sinkless.address().c_str());

  std::string error;
  EXPECT_THAT(pulseAudioSinks(error), testing::IsEmpty());
  EXPECT_THAT(error, testing::IsEmpty())
      << "a server with no outputs is not an error, it just has nothing to list";

  auto capture = makePulseAudioCapture();
  std::optional<SessionError> problem;
  EXPECT_FALSE(capture->start({}, [&](SessionError value) { problem = value; }));
  ASSERT_TRUE(problem.has_value());
  EXPECT_THAT(problem->message, HasSubstr("no monitor source"));

  // The silent output needs a default sink to restore afterwards, so it has to
  // refuse rather than load a null sink it could never put back.
  auto silent = makePulseAudioCapture();
  problem.reset();
  EXPECT_FALSE(silent->start(SilentAudioSink,
                             [&](SessionError value) { problem = value; }));
  ASSERT_TRUE(problem.has_value());
  EXPECT_THAT(problem->message, HasSubstr("no default output sink"));
  EXPECT_THAT(problem->hint, HasSubstr("module-null-sink"));
}

// Runs last, because it takes the private server down for good.
TEST_F(Pulse, ZzReportsTheServerGoingAway) {
  std::vector<SessionError> readProblems;
  auto capture = makePulseAudioCapture();
  ASSERT_TRUE(capture->start(
      {}, [&](SessionError problem) { readProblems.push_back(problem); }));
  PcmBlock block;
  ASSERT_TRUE(capture->next(block, std::chrono::milliseconds(200)));

  server.reset();  // terminates the daemon

  // A read from a dead server has to be reported through the callback, not
  // silently retried forever.
  auto reader = makePulseAudioCapture();
  bool failed = false;
  for (int attempt = 0; attempt < 200 && !failed; ++attempt)
    failed = !capture->next(block, std::chrono::milliseconds(200));
  EXPECT_TRUE(failed);
  ASSERT_FALSE(readProblems.empty()) << "a failed read must be reported";
  EXPECT_EQ(readProblems.back().component, "audio");
  EXPECT_THAT(readProblems.back().hint, HasSubstr("restart streaming"));
  capture->stop();

  // And so has every fresh connection attempt.
  std::string error;
  EXPECT_THAT(pulseAudioSinks(error), testing::IsEmpty());
  EXPECT_THAT(error, testing::Not(testing::IsEmpty()));

  std::optional<SessionError> problem;
  EXPECT_FALSE(reader->start({}, [&](SessionError value) { problem = value; }));
  ASSERT_TRUE(problem.has_value());
  EXPECT_EQ(problem->component, "audio");
  EXPECT_THAT(problem->hint, HasSubstr("audio"));

  // The silent-output path needs the same connection and fails the same way.
  auto silent = makePulseAudioCapture();
  problem.reset();
  EXPECT_FALSE(
      silent->start(SilentAudioSink, [&](SessionError value) { problem = value; }));
  ASSERT_TRUE(problem.has_value());
  EXPECT_EQ(problem->component, "audio");
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
#ifndef MISTERCAST_HAVE_PULSE
  std::fputs("PulseAudio tests skipped: built without libpulse\n", stderr);
  return 77;  // CTest SKIP_RETURN_CODE
#else
  if (!startPrivateServer()) {
    std::fputs("PulseAudio tests skipped: a private pulseaudio server could "
               "not be started\n",
               stderr);
    return 77;
  }
  const int result = RUN_ALL_TESTS();
  server.reset();  // a no-op when the last test already stopped it
  serverDirectory.reset();
  return result;
#endif
}
