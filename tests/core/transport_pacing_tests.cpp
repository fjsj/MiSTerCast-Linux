#include "transport_test_support.hpp"

#include <sys/socket.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace mistercast;
using namespace mistercast::test;
using testing::ElementsAre;
using testing::HasSubstr;

namespace {
// --------------------------------------------------------- payload and framing

class FailingVideoSyscalls final : public UdpSubmitSyscalls {
 public:
  int64_t monotonicNowNs() noexcept override { return 0; }
  int sleepUntil(int64_t) noexcept override { return 0; }
  int sendMessages(int, mmsghdr*, unsigned, int) noexcept override {
    return -EIO;
  }
  int waitWritable(int, int64_t) noexcept override { return 0; }
  bool outputQueueBytes(int, uint64_t&) noexcept override { return false; }
};

TEST(PayloadFailure, IsFatalAndStopsTheProtocolUntilReconnect) {
  // Half a frame on the wire leaves the receiver consuming later commands as
  // pixels, so a payload failure must end the session rather than continue.
  unsigned blits = 0, laterCommands = 0;
  FakeGroovyEndpoint endpoint([&](auto& peer, const auto& packet) {
    if (packet.empty()) return;
    if (packet[0] == kInit)
      peer.replyVersion();
    else if (packet[0] == kBlit)
      ++blits;
    else if (blits)
      ++laterCommands;
  });
  ASSERT_TRUE(endpoint.valid());
  FailingVideoSyscalls syscalls;
  auto transport = GroovyTransportTestPeer::withVideoSyscalls(syscalls);
  std::string error;
  ASSERT_TRUE(transport->open("localhost", 48000, error, endpoint.port()));
  ASSERT_TRUE(transport->switchMode(tinyMode(false), false, error));
  EXPECT_FALSE(transport->sendFrame(1, 0, std::vector<uint8_t>(12, 42), error));
  EXPECT_GT(transport->stats().sendErrors, 0u);

  int16_t audio[2]{};
  EXPECT_FALSE(transport->sendAudio(audio, 2, error));
  EXPECT_THAT(error, HasSubstr("requires reconnect"));
  EXPECT_FALSE(transport->sendFrame(2, 0, std::vector<uint8_t>(12, 42), error));
  EXPECT_THAT(error, HasSubstr("requires reconnect"));

  transport->close();
  ASSERT_TRUE(endpoint.waitForHandledCommand(kBlit));
  endpoint.stop();
  EXPECT_EQ(blits, 1u);
  EXPECT_EQ(laterCommands, 0u)
      << "not even CMD_CLOSE may follow a half-written frame";
}

TEST(SendFrame, RejectsAPayloadThatDoesNotMatchTheActiveMode) {
  FakeGroovyEndpoint endpoint(acknowledgeInit());
  ASSERT_TRUE(endpoint.valid());
  GroovyTransport transport;
  std::string error;
  ASSERT_TRUE(transport.open("localhost", 48000, error, endpoint.port()));
  ASSERT_TRUE(transport.switchMode(tinyMode(false), false, error));
  EXPECT_FALSE(transport.sendFrame(1, 0, std::vector<uint8_t>(11, 42), error));
  EXPECT_EQ(error, "transformed frame has unexpected size");
  EXPECT_FALSE(transport.sendFrame(1, 0, std::vector<uint8_t>(13, 42), error));
  EXPECT_EQ(error, "transformed frame has unexpected size");
}

TEST(SendAudio, RejectsABlockLargerThanTheCommandCanDescribe) {
  FakeGroovyEndpoint endpoint(acknowledgeInit());
  ASSERT_TRUE(endpoint.valid());
  GroovyTransport transport;
  std::string error;
  ASSERT_TRUE(transport.open("localhost", 48000, error, endpoint.port()));
  const std::vector<int16_t> tooMany(40000);
  EXPECT_FALSE(transport.sendAudio(tooMany.data(), tooMany.size(), error));
  EXPECT_EQ(error, "audio packet is too large");
}

TEST(Packetization, AudioAndVideoShareTheSameDatagramSize) {
  UdpVideoConfig config{5, 12, GroovyUdpWireOverheadBytes};
  enum class Payload { None, Audio, Video } payload = Payload::None;
  size_t remaining = 0;
  std::vector<size_t> audioDatagrams, videoDatagrams;
  FakeGroovyEndpoint endpoint([&](auto& peer, const auto& packet) {
    if (payload != Payload::None) {
      (payload == Payload::Audio ? audioDatagrams : videoDatagrams)
          .push_back(packet.size());
      remaining -= packet.size();
      if (!remaining) payload = Payload::None;
      return;
    }
    if (packet.empty()) return;
    if (packet[0] == kInit) {
      peer.replyVersion();
    } else if (packet[0] == kAudio) {
      remaining = packetU16(packet, 1);
      payload = Payload::Audio;
    } else if (packet[0] == kBlit) {
      remaining = packet.size() == 12 ? packetU32(packet, 8) : 12;
      payload = Payload::Video;
    }
  });
  ASSERT_TRUE(endpoint.valid());

  auto transport = GroovyTransportTestPeer::withVideoConfig(config);
  std::string error;
  ASSERT_TRUE(transport->open("localhost", 48000, error, endpoint.port()));
  ASSERT_TRUE(transport->switchMode(tinyMode(false), false, error));
  int16_t audio[4]{};
  ASSERT_TRUE(transport->sendAudio(audio, 4, error));
  std::vector<uint8_t> pixels(12);
  for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = uint8_t(i * 17);
  ASSERT_TRUE(transport->sendFrame(1, 0, pixels, error));
  transport->close();
  ASSERT_TRUE(endpoint.waitForCommand(kClose));
  endpoint.stop();
  EXPECT_THAT(audioDatagrams, ElementsAre(5u, 3u));
  EXPECT_THAT(videoDatagrams, ElementsAre(5u, 5u, 2u));
}

TEST(Packetization, PacesALargeFrameAndDeliversEveryByteInOrder) {
  const auto config = groovyVideoConfig();
  constexpr size_t payloadBytes = size_t(640) * 245 * 3;
  bool commandBeforePayload = false, sizesValid = true, receivingFrame = false;
  size_t expectedBytes = 0, receivedPayload = 0, datagrams = 0, finalSize = 0;
  std::chrono::steady_clock::time_point firstPayload;
  std::chrono::steady_clock::duration deliveryDuration{};
  FakeGroovyEndpoint endpoint([&](auto& peer, const auto& packet) {
    if (receivingFrame) {
      if (!receivedPayload) firstPayload = std::chrono::steady_clock::now();
      if (packet.size() > config.payloadBytes) sizesValid = false;
      receivedPayload += packet.size();
      ++datagrams;
      finalSize = packet.size();
      if (receivedPayload >= expectedBytes) {
        deliveryDuration = std::chrono::steady_clock::now() - firstPayload;
        receivingFrame = false;
      }
      return;
    }
    if (packet.empty()) return;
    if (packet[0] == kInit) {
      peer.replyVersion();
    } else if (packet[0] == kBlit) {
      commandBeforePayload = true;
      const uint32_t compressedBytes =
          packet.size() == 12 ? packetU32(packet, 8) : 0;
      expectedBytes = compressedBytes ? compressedBytes : payloadBytes;
      receivingFrame = true;
    }
  });
  ASSERT_TRUE(endpoint.valid());

  // Incompressible pixels, so the payload really is the full frame and the
  // pacing path is exercised rather than shortcut by LZ4.
  std::vector<uint8_t> pixels(payloadBytes);
  uint32_t random = 0x12345678;
  for (auto& value : pixels) {
    random = random * 1664525u + 1013904223u;
    value = uint8_t(random >> 24);
  }
  std::string error;
  GroovyTransport transport;
  ASSERT_TRUE(transport.open("localhost", 48000, error, endpoint.port()));
  const Modeline mode{"pacing", 12.5, 640, 656, 700, 800, 245, 246, 247, 260,
                      false};
  ASSERT_TRUE(transport.switchMode(mode, false, error));
  ASSERT_TRUE(transport.sendFrame(1, 0, pixels, error));
  const auto stats = transport.stats();
  transport.close();
  ASSERT_TRUE(endpoint.waitForHandledCommand(kClose));
  endpoint.stop();

  EXPECT_TRUE(commandBeforePayload) << "the blit header precedes its pixels";
  EXPECT_TRUE(sizesValid);
  EXPECT_EQ(receivedPayload, payloadBytes);
  EXPECT_EQ(datagrams, videoDatagramCount(payloadBytes, config));
  EXPECT_EQ(finalSize, payloadBytes % config.payloadBytes);
  EXPECT_EQ(stats.pacedVideoPayloads, 1u);
  EXPECT_EQ(stats.pacedDatagrams, videoDatagramCount(payloadBytes, config));
  EXPECT_GT(stats.compressionTimeUs + 1, 0u);
  EXPECT_GT(stats.estimatedWireTimeUs, 0u);

  const auto elapsedUs =
      std::chrono::duration_cast<std::chrono::microseconds>(deliveryDuration)
          .count();
  const auto expectedUs = pacingDurationNs(payloadBytes, config) / 1000;
  EXPECT_GE(elapsedUs + 1000, int64_t(expectedUs))
      << "the frame must not be released faster than the pacing rate";
}

TEST(SendErrors, AreCountedAndReportedWhenTheTargetDisappears) {
  FakeGroovyEndpoint endpoint([](auto& peer, const auto& packet) {
    if (!packet.empty() && packet[0] == kInit) peer.replyAck({});
  });
  ASSERT_TRUE(endpoint.valid());
  std::string error;
  GroovyTransport transport;
  ASSERT_TRUE(transport.open("localhost", 48000, error, endpoint.port()));
  endpoint.stop();
  // The first send after the peer closes usually succeeds; the ICMP port-unreachable
  // it triggers surfaces as an error on a later send.
  bool sendFailed = false;
  for (int attempt = 0; attempt < 10 && !sendFailed; ++attempt)
    sendFailed = !transport.switchMode(tinyMode(false), false, error);
  EXPECT_TRUE(sendFailed);
  EXPECT_THAT(error, testing::Not(testing::IsEmpty()));
  EXPECT_GT(transport.stats().sendErrors, 0u);
  transport.close();
}

// ------------------------------------------------------------ sync line policy

class SyncLine
    : public testing::TestWithParam<std::tuple<bool, bool, bool, uint16_t>> {};

TEST_P(SyncLine, FollowsTheConfiguredPolicy) {
  const auto [interlaced, progressiveBuffer, syncRefresh, frameDelay] =
      GetParam();
  FakeGroovyEndpoint endpoint([](auto& peer, const auto& packet) {
    if (!packet.empty() && packet[0] == kInit) peer.replyAck({});
  });
  ASSERT_TRUE(endpoint.valid());
  std::string error;
  GroovyTransport transport;
  ASSERT_TRUE(transport.open("localhost", 48000, error, endpoint.port()));
  const auto mode = vgaMode(interlaced);
  ASSERT_TRUE(transport.switchMode(mode, progressiveBuffer, error));
  transport.setSyncOptions(syncRefresh, frameDelay);
  const std::vector<uint8_t> pixels(
      size_t(640) * (interlaced && !progressiveBuffer ? 240 : 480) * 3, 42);

  uint16_t warmUpLine = 0, steadyLine = 0;
  for (uint32_t frame = 1; frame <= 12; ++frame) {
    ASSERT_TRUE(transport.sendFrame(frame, 0, pixels, error)) << error;
    transport.waitSync();
    if (frame == 5) warmUpLine = transport.stats().requestedSyncLine;
    if (frame == 12) steadyLine = transport.stats().requestedSyncLine;
  }
  transport.close();
  EXPECT_EQ(transport.stats().acknowledgedFrames, 0u)
      << "this endpoint never acknowledges blits";

  if (!syncRefresh) {
    EXPECT_EQ(warmUpLine, 0) << "sync line 0 means 'blit immediately'";
    EXPECT_EQ(steadyLine, 0);
  } else if (frameDelay) {
    // The requested line is clamped to the raster, so a delay of 10 asks for the
    // very last line rather than one past it.
    const auto expected = uint16_t(std::min<uint32_t>(
        uint32_t(std::llround(double(mode.vTotal) * frameDelay / 10.0)) + 1,
        mode.vTotal));
    EXPECT_EQ(warmUpLine, expected) << "a manual delay is a fixed fraction";
    EXPECT_EQ(steadyLine, expected);
  } else {
    EXPECT_EQ(warmUpLine, mode.vTotal / 2) << "warm-up is deliberately safe";
    if (interlaced && !progressiveBuffer)
      EXPECT_THAT(steadyLine, testing::AllOf(testing::Gt(0),
                                             testing::Le(mode.vTotal / 2)))
          << "an alternating field buffer never asks past mid-raster";
    else
      EXPECT_THAT(steadyLine,
                  testing::AllOf(testing::Gt(0),
                                 testing::Ne(mode.vTotal / 2)));
  }
}

INSTANTIATE_TEST_SUITE_P(
    Policies, SyncLine,
    testing::Values(
        std::make_tuple(false, false, true, uint16_t(0)),
        std::make_tuple(true, false, true, uint16_t(0)),
        std::make_tuple(true, true, true, uint16_t(0)),
        std::make_tuple(true, false, true, uint16_t(4)),
        std::make_tuple(true, false, true, uint16_t(10)),
        std::make_tuple(true, false, false, uint16_t(0))));

TEST(WaitSync, ReturnsImmediatelyBeforeAnyModeIsSelected) {
  GroovyTransport transport;
  const auto started = std::chrono::steady_clock::now();
  transport.waitSync();
  EXPECT_LT(std::chrono::steady_clock::now() - started,
            std::chrono::milliseconds(100));
  EXPECT_EQ(transport.stats().acknowledgedFrames, 0u);
  EXPECT_EQ(transport.stats().missedAcks, 0u);
}

TEST(WaitSync, CountsAMissedAcknowledgementWhenNoStatusArrives) {
  FakeGroovyEndpoint endpoint(acknowledgeInit());
  ASSERT_TRUE(endpoint.valid());
  GroovyTransport transport;
  std::string error;
  ASSERT_TRUE(transport.open("localhost", 48000, error, endpoint.port()));
  ASSERT_TRUE(transport.switchMode(tinyMode(false), false, error));
  ASSERT_TRUE(transport.sendFrame(1, 0, std::vector<uint8_t>(12, 42), error));
  transport.waitSync();
  const auto stats = transport.stats();
  EXPECT_EQ(stats.missedAcks, 1u);
  EXPECT_EQ(stats.acknowledgedFrames, 0u);
}

TEST(WaitSync, RefinesTheRoundTripEstimateFromRealAcknowledgements) {
  FakeGroovyEndpoint endpoint([](auto& peer, const auto& packet) {
    if (packet.empty()) return;
    if (packet[0] == kInit)
      peer.replyVersion();
    else if (packet[0] == kBlit)
      acknowledgeBlit(peer, packet, packetU32(packet, 1), 1, kHealthy);
  });
  ASSERT_TRUE(endpoint.valid());
  GroovyTransport transport;
  std::string error;
  ASSERT_TRUE(transport.open("localhost", 48000, error, endpoint.port()));
  ASSERT_TRUE(transport.switchMode(vgaMode(), false, error));
  const std::vector<uint8_t> pixels(size_t(640) * 480 * 3, 42);
  for (uint32_t frame = 1; frame <= 5; ++frame) {
    ASSERT_TRUE(transport.sendFrame(frame, 0, pixels, error));
    transport.waitSync();
  }
  const auto stats = transport.stats();
  EXPECT_EQ(stats.acknowledgedFrames, 5u);
  EXPECT_GT(stats.networkRttUs, 0u);
  EXPECT_LT(stats.ackAgeMs, 1000u);
  EXPECT_GT(stats.streamTimeUs, 0u);
}

TEST(WaitSync, AnAcknowledgementArrivingDuringThePacingWaitStillCounts) {
  // A receiver that answers a few milliseconds late. waitSync has to spend the
  // wait on the socket rather than on a timer, so a late ACK corrects this frame
  // instead of being counted as missed.
  FakeGroovyEndpoint endpoint([](auto& peer, const auto& packet) {
    if (packet.empty()) return;
    if (packet[0] == kInit) {
      peer.replyVersion();
    } else if (packet[0] == kBlit) {
      std::this_thread::sleep_for(std::chrono::milliseconds(4));
      acknowledgeBlit(peer, packet, packetU32(packet, 1), 1, kHealthy);
    }
  });
  ASSERT_TRUE(endpoint.valid());
  GroovyTransport transport;
  std::string error;
  ASSERT_TRUE(transport.open("localhost", 48000, error, endpoint.port()));
  ASSERT_TRUE(transport.switchMode(vgaMode(), false, error));
  transport.setSyncOptions(true, 0);
  const std::vector<uint8_t> pixels(size_t(640) * 480 * 3, 42);
  for (uint32_t frame = 1; frame <= 4; ++frame) {
    ASSERT_TRUE(transport.sendFrame(frame, 0, pixels, error)) << error;
    transport.waitSync();
  }
  const auto stats = transport.stats();
  EXPECT_GE(stats.acknowledgedFrames, 3u)
      << "late ACKs must be waited for, not missed";
  EXPECT_GT(stats.networkRttUs, 0u);
  transport.close();
}

TEST(RasterCorrection, IsZeroWhenTheReceiverEchoesNoRasterPosition) {
  FakeGroovyEndpoint endpoint([](auto& peer, const auto& packet) {
    if (packet.empty()) return;
    if (packet[0] == kInit)
      peer.replyVersion();
    else if (packet[0] == kBlit)
      // vCountEcho of zero: the core did not report where the raster was.
      peer.replyAck({packetU32(packet, 1), 0, 3, 4, kHealthy});
  });
  ASSERT_TRUE(endpoint.valid());
  GroovyTransport transport;
  std::string error;
  ASSERT_TRUE(transport.open("localhost", 48000, error, endpoint.port()));
  ASSERT_TRUE(transport.switchMode(vgaMode(), false, error));
  const std::vector<uint8_t> pixels(size_t(640) * 480 * 3, 42);
  ASSERT_TRUE(transport.sendFrame(1, 0, pixels, error));
  transport.waitSync();
  EXPECT_EQ(transport.stats().rasterCorrectionUs, 0);
}

TEST(RasterCorrection, IsPositiveWhenTheFpgaRasterTrailsTheRequest) {
  FakeGroovyEndpoint endpoint([](auto& peer, const auto& packet) {
    if (packet.empty()) return;
    if (packet[0] == kInit)
      peer.replyVersion();
    else if (packet[0] == kBlit)
      // The blit was requested at a much later raster line than the FPGA had
      // reached, so the sender is early and must wait longer.
      peer.replyAck({packetU32(packet, 1), 500, packetU32(packet, 1) - 1, 10,
                     kHealthy});
  });
  ASSERT_TRUE(endpoint.valid());
  GroovyTransport transport;
  std::string error;
  ASSERT_TRUE(transport.open("localhost", 48000, error, endpoint.port()));
  ASSERT_TRUE(transport.switchMode(vgaMode(), false, error));
  const std::vector<uint8_t> pixels(size_t(640) * 480 * 3, 42);
  ASSERT_TRUE(transport.sendFrame(4, 0, pixels, error));
  transport.waitSync();
  EXPECT_GT(transport.stats().rasterCorrectionUs, 0);
}

TEST(DrainStatus, OnAClosedTransportReportsNoMatch) {
  GroovyTransport transport;
  // No socket at all: draining must simply say nothing matched.
  GroovyTransportTestPeer::drainPendingStatus(transport, 1);
  const auto stats = transport.stats();
  EXPECT_EQ(stats.acknowledgedFrame, 0u);
  EXPECT_EQ(stats.fpgaStatusSamples, 0u);
}

TEST(WaitSync, AfterCloseDoesNothingAndCannotBlock) {
  FakeGroovyEndpoint endpoint(acknowledgeInit());
  ASSERT_TRUE(endpoint.valid());
  GroovyTransport transport;
  std::string error;
  ASSERT_TRUE(transport.open("localhost", 48000, error, endpoint.port()));
  ASSERT_TRUE(transport.switchMode(vgaMode(), false, error));
  transport.close();
  const auto started = std::chrono::steady_clock::now();
  transport.waitSync();
  EXPECT_LT(std::chrono::steady_clock::now() - started,
            std::chrono::milliseconds(200));
}

TEST(RasterCorrection, IsNotAppliedWhenRasterSyncIsTurnedOff) {
  FakeGroovyEndpoint endpoint([](auto& peer, const auto& packet) {
    if (packet.empty()) return;
    if (packet[0] == kInit)
      peer.replyVersion();
    else if (packet[0] == kBlit)
      peer.replyAck({packetU32(packet, 1), 500, packetU32(packet, 1) - 1, 10,
                     kHealthy});
  });
  ASSERT_TRUE(endpoint.valid());
  GroovyTransport transport;
  std::string error;
  ASSERT_TRUE(transport.open("localhost", 48000, error, endpoint.port()));
  ASSERT_TRUE(transport.switchMode(vgaMode(), false, error));
  transport.setSyncOptions(/*syncRefresh=*/false, 0);
  const std::vector<uint8_t> pixels(size_t(640) * 480 * 3, 42);
  ASSERT_TRUE(transport.sendFrame(4, 0, pixels, error));
  transport.waitSync();
  const auto stats = transport.stats();
  EXPECT_EQ(stats.requestedSyncLine, 0);
  EXPECT_EQ(stats.rasterCorrectionUs, 0)
      << "with sync off there is no raster to correct against";
  EXPECT_EQ(stats.acknowledgedFrames, 1u);
}

TEST(SendAudio, StopsAtTheFirstFailedDatagramWithoutSendingTheRest) {
  // A closed transport fails on the CMD_AUDIO header, so no PCM may follow.
  GroovyTransport transport;
  std::string error;
  int16_t samples[8]{};
  EXPECT_FALSE(transport.sendAudio(samples, 8, error));
  EXPECT_EQ(error, "audio was not negotiated for this transport");
}

}  // namespace
