#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "mistercast/groovy_transport.hpp"
#include "mistercast/pattern.hpp"
#include "support/fake_groovy_endpoint.hpp"
#include "support/fake_mister.hpp"
#include "support/groovy_wire.hpp"
#include "support/wait_for.hpp"

using namespace mistercast;
using namespace mistercast::test;
using testing::HasSubstr;

namespace {

// 8x8 active on a 100-line raster: a whole frame is a few hundred bytes and the
// pacing wait is short, so protocol-level tests stay fast.
Modeline runnerMode(bool interlaced = true) {
  return {"runner", 1, 8, 10, 12, 100, 8, 10, 12, 100, interlaced};
}

PatternOptions patternFor(const Modeline& mode) {
  PatternOptions options;
  options.target = "localhost";
  options.modeline = mode;
  return options;
}

// ----------------------------------------------------------- option parsing

TEST(ParsePatternOptions, RequiresATarget) {
  PatternOptions options;
  std::string error;
  EXPECT_FALSE(parsePatternOptions({}, options, error));
  EXPECT_THAT(error, HasSubstr("a target is required"));
  EXPECT_FALSE(parsePatternOptions({"--tone"}, options, error));
  EXPECT_THAT(error, HasSubstr("a target is required"));
}

TEST(ParsePatternOptions, AcceptsEveryDocumentedOption) {
  PatternOptions options;
  std::string error;
  ASSERT_TRUE(parsePatternOptions(
      {"--target", "host", "--tone", "--content", "noise",
       "--progressive-interlace-buffer", "--frame-delay", "4", "--modeline",
       "1 16 18 20 24 8 9 10 12 1"},
      options, error))
      << error;
  EXPECT_EQ(options.target, "host");
  EXPECT_TRUE(options.tone);
  EXPECT_EQ(options.content, PatternContent::Noise);
  EXPECT_TRUE(options.progressiveInterlaceBuffer);
  EXPECT_EQ(options.frameDelay, 4);
  EXPECT_TRUE(options.modeline.interlaced);
  EXPECT_EQ(options.modeline.hActive, 16);
}

TEST(ParsePatternOptions, DefaultsToColourBarsWithoutToneOrDelay) {
  PatternOptions options;
  std::string error;
  ASSERT_TRUE(parsePatternOptions({"--target", "host"}, options, error));
  EXPECT_EQ(options.content, PatternContent::Bars);
  EXPECT_FALSE(options.tone);
  EXPECT_FALSE(options.progressiveInterlaceBuffer);
  EXPECT_EQ(options.frameDelay, 0);
  EXPECT_EQ(options.modeline.hActive, Modeline::safeDefault().hActive);
}

TEST(ParsePatternOptions, TheTwoInterlaceBufferSwitchesOverrideEachOther) {
  PatternOptions options;
  std::string error;
  ASSERT_TRUE(parsePatternOptions({"--target", "host",
                                   "--progressive-interlace-buffer",
                                   "--interlaced-field-buffer"},
                                  options, error));
  EXPECT_FALSE(options.progressiveInterlaceBuffer);
  ASSERT_TRUE(parsePatternOptions({"--target", "host",
                                   "--interlaced-field-buffer",
                                   "--progressive-interlace-buffer"},
                                  options, error));
  EXPECT_TRUE(options.progressiveInterlaceBuffer);
}

TEST(ParsePatternOptions, ReportsAMissingValueForEveryOptionThatTakesOne) {
  PatternOptions options;
  std::string error;
  for (const char* option :
       {"--target", "--modeline", "--content", "--frame-delay"}) {
    error.clear();
    EXPECT_FALSE(parsePatternOptions({option}, options, error)) << option;
    EXPECT_THAT(error, HasSubstr("missing value for"));
    EXPECT_THAT(error, HasSubstr(option));
  }
}

TEST(ParsePatternOptions, RejectsUnknownContent) {
  PatternOptions options;
  std::string error;
  EXPECT_FALSE(parsePatternOptions({"--target", "host", "--content", "solid"},
                                   options, error));
  EXPECT_EQ(error, "pattern content must be bars or noise");
}

TEST(ParsePatternOptions, RejectsAFrameDelayOutsideZeroToTen) {
  PatternOptions options;
  std::string error;
  for (const char* value : {"11", "-1", "abc", "", "99999999999999999999"}) {
    error.clear();
    EXPECT_FALSE(parsePatternOptions({"--target", "host", "--frame-delay", value},
                                     options, error))
        << value;
    EXPECT_EQ(error, "frame delay must be 0..10");
  }
  ASSERT_TRUE(parsePatternOptions({"--target", "host", "--frame-delay", "0"},
                                  options, error));
  EXPECT_EQ(options.frameDelay, 0);
  ASSERT_TRUE(parsePatternOptions({"--target", "host", "--frame-delay", "10"},
                                  options, error));
  EXPECT_EQ(options.frameDelay, 10);
}

TEST(ParsePatternOptions, RejectsAnUnknownOption) {
  PatternOptions options;
  std::string error;
  EXPECT_FALSE(
      parsePatternOptions({"--target", "host", "--audio"}, options, error));
  EXPECT_EQ(error, "unknown pattern option: --audio");
}

TEST(ParsePatternOptions, RejectsAModelineTheProtocolCannotStream) {
  PatternOptions options;
  std::string error;
  EXPECT_FALSE(parsePatternOptions({"--target", "host", "--modeline", "bad"},
                                   options, error));
  EXPECT_THAT(error, HasSubstr("modeline needs"));

  error.clear();
  EXPECT_FALSE(parsePatternOptions(
      {"--target", "host", "--modeline",
       "65 1024 1048 1184 1344 768 771 777 806 0"},
      options, error));
  EXPECT_THAT(error, HasSubstr("exceeds Groovy_MiSTer frame buffer"));
}

TEST(ParsePatternOptions, StartsFromACleanSlateOnEveryCall) {
  PatternOptions options;
  std::string error;
  ASSERT_TRUE(parsePatternOptions({"--target", "host", "--tone"}, options,
                                  error));
  ASSERT_TRUE(options.tone);
  ASSERT_TRUE(parsePatternOptions({"--target", "other"}, options, error));
  EXPECT_FALSE(options.tone) << "a previous run's options must not leak";
  EXPECT_EQ(options.target, "other");
}

// -------------------------------------------------------- pattern generation

TEST(GeneratePattern, DrawsEightColourBarsAcrossTheActiveWidth) {
  auto options = patternFor({"bars", 1, 16, 18, 20, 24, 8, 9, 10, 12, false});
  std::vector<uint8_t> even, odd;
  generatePattern(options, 2, 0, even);
  generatePattern(options, 3, 1, odd);

  ASSERT_EQ(even.size(), size_t(16 * 8 * 3));
  // The latency square in the top-left flashes with the frame parity.
  EXPECT_THAT(std::vector<uint8_t>(even.begin(), even.begin() + 3),
              testing::ElementsAre(255, 255, 255));
  EXPECT_THAT(std::vector<uint8_t>(odd.begin(), odd.begin() + 3),
              testing::ElementsAre(0, 0, 0));
  // Bar two is yellow in BGR once past the marker.
  EXPECT_THAT(std::vector<uint8_t>(even.begin() + 6, even.begin() + 9),
              testing::ElementsAre(0, 255, 255));
  // The bottom-right field marker is green for field 0 and red for field 1.
  EXPECT_EQ(even[even.size() - 3], 255);
  EXPECT_EQ(even[even.size() - 2], 0);
  EXPECT_EQ(odd[odd.size() - 3], 0);
  EXPECT_EQ(odd[odd.size() - 2], 255);
}

TEST(GeneratePattern, NoiseIsDeterministicPerFrameAndChangesBetweenFrames) {
  auto options = patternFor({"noise", 1, 16, 18, 20, 24, 8, 9, 10, 12, true});
  options.content = PatternContent::Noise;
  options.progressiveInterlaceBuffer = true;
  std::vector<uint8_t> first, repeated, next;
  generatePattern(options, 7, 0, first);
  generatePattern(options, 7, 0, repeated);
  generatePattern(options, 8, 0, next);
  EXPECT_EQ(first, repeated) << "the same frame must reproduce exactly";
  EXPECT_NE(first, next);
}

TEST(GeneratePattern, NoiseHasLowCompressibilityToExerciseUdpPacing) {
  auto options = patternFor({"noise", 1, 64, 66, 68, 80, 64, 66, 68, 80, false});
  options.content = PatternContent::Noise;
  std::vector<uint8_t> pixels;
  generatePattern(options, 7, 0, pixels);
  size_t equalNeighbours = 0;
  for (size_t i = 3; i < pixels.size(); i += 3)
    if (std::memcmp(pixels.data() + i - 3, pixels.data() + i, 3) == 0)
      ++equalNeighbours;
  EXPECT_LT(equalNeighbours, pixels.size() / 96)
      << "run-length-friendly output would not stress the pacer";
}

TEST(GeneratePattern, FieldBuffersCarryTheAlternatingHalvesOfTheFullFrame) {
  auto options = patternFor({"noise", 1, 16, 18, 20, 24, 8, 9, 10, 12, true});
  options.content = PatternContent::Noise;
  options.progressiveInterlaceBuffer = true;
  std::vector<uint8_t> full;
  generatePattern(options, 7, 0, full);

  options.progressiveInterlaceBuffer = false;
  std::vector<uint8_t> fieldZero, fieldOne;
  generatePattern(options, 7, 0, fieldZero);
  generatePattern(options, 7, 1, fieldOne);
  ASSERT_EQ(fieldZero.size(), full.size() / 2);
  ASSERT_EQ(fieldOne.size(), full.size() / 2);

  const size_t rowBytes = size_t(options.modeline.hActive) * 3;
  for (size_t y = 0; y < size_t(options.modeline.vActive) / 2; ++y) {
    EXPECT_EQ(0, std::memcmp(fieldZero.data() + y * rowBytes,
                             full.data() + (y * 2 + 1) * rowBytes, rowBytes))
        << "field 0 row " << y;
    EXPECT_EQ(0, std::memcmp(fieldOne.data() + y * rowBytes,
                             full.data() + y * 2 * rowBytes, rowBytes))
        << "field 1 row " << y;
  }
}

TEST(GeneratePattern, ReusesTheCallersBuffer) {
  auto options = patternFor({"bars", 1, 16, 18, 20, 24, 8, 9, 10, 12, false});
  std::vector<uint8_t> pixels;
  generatePattern(options, 1, 0, pixels);
  const auto* data = pixels.data();
  generatePattern(options, 2, 0, pixels);
  EXPECT_EQ(pixels.data(), data);
}

// ------------------------------------------------------------------ tone

TEST(PatternTone, IsContinuousAcrossUnevenlySpacedCalls) {
  PatternTone split;
  std::vector<int16_t> pieces;
  for (uint64_t elapsed : {10000000ull, 7000000ull, 13000000ull}) {
    std::vector<int16_t> generated;
    split.generate(elapsed, generated);
    pieces.insert(pieces.end(), generated.begin(), generated.end());
  }
  PatternTone once;
  std::vector<int16_t> combined;
  once.generate(30000000, combined);
  EXPECT_EQ(pieces, combined) << "phase must carry across calls";
  EXPECT_EQ(combined.size(), 2880u) << "30 ms of stereo at 48 kHz";
}

TEST(PatternTone, IsIdenticalOnBothChannelsAndWithinAmplitude) {
  PatternTone tone;
  std::vector<int16_t> stereo;
  tone.generate(100000000, stereo);
  ASSERT_FALSE(stereo.empty());
  for (size_t i = 0; i < stereo.size(); i += 2) {
    ASSERT_EQ(stereo[i], stereo[i + 1]) << "at value " << i;
    ASSERT_LE(std::abs(int(stereo[i])), 4096);
  }
  EXPECT_GT(*std::max_element(stereo.begin(), stereo.end()), 4000)
      << "a full cycle should reach close to the amplitude";
}

TEST(PatternTone, ResetReturnsToTheStartOfTheWaveform) {
  PatternTone tone;
  std::vector<int16_t> first, afterReset;
  tone.generate(30000000, first);
  tone.reset();
  tone.generate(30000000, afterReset);
  EXPECT_EQ(first, afterReset);
}

// ------------------------------------------------------------- streaming

TEST(StreamGeneratedPattern, ReportsAModeThatCannotBeSelected) {
  // An unopened transport cannot send CMD_SWITCHRES.
  GroovyTransport transport;
  std::atomic<bool> stop{false};
  std::ostringstream status;
  std::string error;
  EXPECT_FALSE(streamGeneratedPattern(patternFor(runnerMode()), transport, stop,
                                      status, error));
  EXPECT_EQ(error, "transport is closed");
}

TEST(StreamGeneratedPattern, StopsImmediatelyWhenAskedBeforeTheFirstFrame) {
  FakeGroovyEndpoint endpoint([](auto& peer, const auto& packet) {
    if (!packet.empty() && packet[0] == kInit) peer.replyVersion();
  });
  ASSERT_TRUE(endpoint.valid());
  GroovyTransport transport;
  std::string error;
  ASSERT_TRUE(transport.open("localhost", std::nullopt, error,
                             endpoint.port()));
  std::atomic<bool> stop{true};
  std::ostringstream status;
  EXPECT_TRUE(streamGeneratedPattern(patternFor(runnerMode()), transport, stop,
                                     status, error))
      << error;
  EXPECT_THAT(status.str(), HasSubstr("press Ctrl-C to stop"));
  endpoint.stop();
}

TEST(StreamGeneratedPattern, SendsToneBeforeEachFrameAndAlternatesFields) {
  std::atomic<bool> stop{false};
  std::vector<char> events;
  std::vector<uint8_t> fields;
  bool sawInit = false, sawMode = false, sawClose = false;
  size_t payloadRemaining = 0;
  char payloadEvent = 0;
  FakeGroovyEndpoint endpoint([&](auto& peer, const auto& packet) {
    if (payloadRemaining) {
      payloadRemaining -= std::min(payloadRemaining, packet.size());
      if (!payloadRemaining) {
        events.push_back(payloadEvent);
        if (payloadEvent == 'V' && fields.size() >= 4) stop = true;
      }
      return;
    }
    if (packet.empty()) return;
    if (packet[0] == kInit) {
      sawInit = true;
      peer.replyAck({0, 0, 0, 0, kHealthyWithAudio});
    } else if (packet[0] == kSwitchMode) {
      sawMode = true;
    } else if (packet[0] == kAudio) {
      payloadRemaining = packetU16(packet, 1);
      payloadEvent = 'A';
    } else if (packet[0] == kBlit) {
      fields.push_back(packet.at(5));
      const auto compressed = packet.size() == 12 ? packetU32(packet, 8) : 0;
      payloadRemaining = compressed ? compressed : size_t(8 * 8 * 3 / 2);
      payloadEvent = 'V';
      // Alternate the reported FPGA field so alignFrame keeps flipping.
      peer.replyAck({packetU32(packet, 1), packetU16(packet, 6),
                     packetU32(packet, 1), packetU16(packet, 6),
                     uint8_t(kHealthyWithAudio |
                             ((fields.size() & 1) ? 0x20 : 0))});
    } else if (packet[0] == kClose) {
      sawClose = true;
    }
  });
  ASSERT_TRUE(endpoint.valid());

  auto options = patternFor(runnerMode());
  options.tone = true;
  std::ostringstream status;
  std::string error;
  GroovyTransport transport;
  ASSERT_TRUE(transport.open(options.target, 48000, error, endpoint.port()))
      << error;
  ASSERT_TRUE(streamGeneratedPattern(options, transport, stop, status, error))
      << error;
  EXPECT_TRUE(transport.connected());
  transport.close();
  ASSERT_TRUE(endpoint.waitForCommand(kClose));
  endpoint.stop();

  EXPECT_TRUE(sawInit);
  EXPECT_TRUE(sawMode);
  EXPECT_TRUE(sawClose);
  ASSERT_GE(fields.size(), 4u);
  EXPECT_NE(fields[0], fields[1]) << "fields must alternate";
  EXPECT_NE(fields[1], fields[2]);
  EXPECT_THAT(events, testing::Contains('A'));

  // Audio for a frame must precede that frame, matching the upstream sender.
  size_t videos = 0;
  for (size_t i = 0; i < events.size(); ++i) {
    if (events[i] != 'V') continue;
    if (videos++) {
      ASSERT_GT(i, 0u);
      EXPECT_EQ(events[i - 1], 'A') << "video payload " << videos;
    }
  }
  EXPECT_GE(videos, 4u);
}

TEST(StreamGeneratedPattern, SendsNoAudioWhenTheCoreReportsAudioOff) {
  std::atomic<bool> stop{false};
  std::atomic<unsigned> audioCommands{0}, blits{0};
  FakeGroovyEndpoint endpoint([&](auto& peer, const auto& packet) {
    if (packet.empty()) return;
    if (packet[0] == kInit) {
      peer.replyAck({0, 0, 0, 0, 0x84});  // healthy, core audio off
    } else if (packet[0] == kAudio && packet.size() == 3) {
      ++audioCommands;
    } else if (packet[0] == kBlit && (packet.size() == 8 || packet.size() == 12)) {
      if (++blits >= 4) stop = true;
      peer.replyAck({packetU32(packet, 1), packetU16(packet, 6),
                     packetU32(packet, 1), packetU16(packet, 6), 0x84});
    }
  });
  ASSERT_TRUE(endpoint.valid());
  auto options = patternFor(runnerMode(false));
  options.tone = true;
  std::ostringstream status;
  std::string error;
  GroovyTransport transport;
  ASSERT_TRUE(transport.open(options.target, 48000, error, endpoint.port()));
  ASSERT_TRUE(streamGeneratedPattern(options, transport, stop, status, error))
      << error;
  transport.close();
  endpoint.stop();
  EXPECT_GE(blits.load(), 4u);
  EXPECT_EQ(audioCommands.load(), 0u);
}

TEST(StreamGeneratedPattern, StopsAndReportsWhenAFrameCannotBeSent) {
  std::atomic<bool> stop{false};
  auto endpoint = std::make_unique<FakeGroovyEndpoint>(
      [](auto& peer, const auto& packet) {
        if (!packet.empty() && packet[0] == kInit) peer.replyVersion();
      });
  ASSERT_TRUE(endpoint->valid());
  GroovyTransport transport;
  std::string error;
  ASSERT_TRUE(transport.open("localhost", std::nullopt, error,
                             endpoint->port()));
  const auto options = patternFor(runnerMode(false));
  // Dropping the endpoint makes the port unreachable; the resulting ICMP error
  // surfaces on a later send.
  endpoint.reset();
  std::ostringstream status;
  bool streamed = true;
  for (int attempt = 0; attempt < 20 && streamed; ++attempt)
    streamed = streamGeneratedPattern(options, transport, stop, status, error);
  EXPECT_FALSE(streamed);
  EXPECT_THAT(error, testing::Not(testing::IsEmpty()));
}

TEST(RunGeneratedPattern, ReportsATargetThatCannotBeReached) {
  auto options = patternFor(runnerMode(false));
  options.target = "mistercast.invalid.";
  std::atomic<bool> stop{false};
  std::ostringstream status;
  std::string error;
  EXPECT_FALSE(runGeneratedPattern(options, stop, status, error));
  EXPECT_THAT(error, HasSubstr("cannot resolve"));
}

TEST(RunGeneratedPattern, OpensClosesAndStreamsOverTheProtocolPort) {
  // runGeneratedPattern owns the transport, so it always dials UDP 32100.
  FakeMister mister(kHealthyWithAudio);
  if (!mister.bound())
    GTEST_SKIP() << "UDP port 32100 is already in use on this machine";

  auto options = patternFor(runnerMode(false));
  options.target = "127.0.0.1";
  options.tone = true;
  std::atomic<bool> stop{false};
  std::ostringstream status;
  std::string error;
  std::thread runner([&] {
    EXPECT_TRUE(runGeneratedPattern(options, stop, status, error)) << error;
  });
  EXPECT_TRUE(waitFor([&] { return mister.blits() >= 4; }));
  EXPECT_TRUE(waitFor([&] { return mister.audioPackets() >= 1; }));
  stop = true;
  runner.join();
  EXPECT_TRUE(waitFor([&] { return mister.closes() >= 1; }));
  EXPECT_EQ(mister.initRateCode(), 3) << "the tone negotiates 48 kHz";
  EXPECT_THAT(status.str(), HasSubstr("Pattern streaming"));
}

TEST(StreamGeneratedPattern, PrintsPeriodicDiagnosticsForAnInterlacedMode) {
  // The counters are printed every five seconds, so this deliberately runs past
  // that boundary to cover the interlaced reserve/field reporting.
  std::atomic<bool> stop{false};
  // Synced but with the VGA frameskip fallback engaged and an empty VRAM queue,
  // so the unhealthy side of every diagnostic is printed too.
  FakeGroovyEndpoint endpoint([&](auto& peer, const auto& packet) {
    if (packet.empty()) return;
    if (packet[0] == kInit)
      peer.replyAck({0, 0, 0, 0, kUnhealthy});
    else if (packet[0] == kBlit && (packet.size() == 8 || packet.size() == 12))
      peer.replyAck({packetU32(packet, 1), packetU16(packet, 6),
                     packetU32(packet, 1), packetU16(packet, 6), kUnhealthy});
  });
  ASSERT_TRUE(endpoint.valid());
  GroovyTransport transport;
  std::string error;
  ASSERT_TRUE(transport.open("localhost", std::nullopt, error,
                             endpoint.port()));
  // The stream is only inspected after the worker is joined, so the stream
  // object is never touched from two threads at once.
  std::ostringstream status;
  // Report on every pass instead of once every five seconds, so the periodic
  // branch is crossed repeatedly within a few frames rather than by waiting out
  // the production cadence.
  auto options = patternFor(runnerMode(true));
  options.statsInterval = std::chrono::milliseconds(1);
  std::thread runner([&] {
    EXPECT_TRUE(streamGeneratedPattern(options, transport, stop, status, error))
        << error;
  });
  waitFor([&] { return endpoint.packets().size() > 8; });
  stop = true;
  runner.join();
  transport.close();
  endpoint.stop();
  EXPECT_THAT(status.str(), HasSubstr("reserve/latest"));
  EXPECT_THAT(status.str(), HasSubstr("steps/resets"));
  EXPECT_THAT(status.str(), HasSubstr("MTU"));
  EXPECT_THAT(status.str(), HasSubstr("/fallback"));
  EXPECT_THAT(status.str(), HasSubstr("queue empty"));
}

}  // namespace
