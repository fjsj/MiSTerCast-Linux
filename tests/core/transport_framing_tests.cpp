#include "transport_test_support.hpp"

#include <chrono>
#include <cstring>
#include <string>
#include <vector>

using namespace mistercast;
using namespace mistercast::test;
using testing::ElementsAre;

namespace {
// ------------------------------------------------------- interlace and framing

class InterlaceTransport
    : public testing::TestWithParam<std::tuple<bool, uint8_t, uint8_t, size_t>> {
};

TEST_P(InterlaceTransport, SendsTheNegotiatedBufferKindAndFieldIndex) {
  const auto [progressive, expectedInterlaceCode, expectedField, pixelValues] =
      GetParam();
  bool sawMode = false, sawFrame = false, sawAudio = false, sawClose = false;
  uint8_t receivedInterlace = 0xff, receivedField = 0xff;
  uint16_t receivedSyncLine = 0;
  FakeGroovyEndpoint endpoint([&](auto& peer, const auto& packet) {
    if (packet.empty()) return;
    switch (packet[0]) {
      case kInit:
        peer.replyAck({});
        break;
      case kSwitchMode:
        sawMode = true;
        receivedInterlace = packet.at(25);
        break;
      case kBlit:
        sawFrame = true;
        receivedField = packet.at(5);
        receivedSyncLine = packetU16(packet, 6);
        acknowledgeBlit(peer, packet, packetU32(packet, 1) + 1, 1, 0x44);
        break;
      case kAudio:
        sawAudio = true;
        break;
      case kClose:
        sawClose = true;
        break;
      default:
        break;
    }
  });
  ASSERT_TRUE(endpoint.valid());

  std::string error;
  GroovyTransport transport;
  ASSERT_TRUE(transport.open("localhost", 48000, error, endpoint.port()));
  ASSERT_TRUE(transport.switchMode(tinyMode(), progressive, error));
  transport.setSyncOptions(true, 0);
  ASSERT_TRUE(transport.sendFrame(1, /*field=*/1,
                                  std::vector<uint8_t>(pixelValues, 42), error))
      << error;
  transport.waitSync();

  const auto status = transport.stats();
  EXPECT_EQ(status.acknowledgedFrame, 1u);
  EXPECT_EQ(status.acknowledgedFrames, 1u);
  EXPECT_LT(status.rasterCorrectionUs, 0)
      << "the FPGA raster is ahead of the requested line";
  EXPECT_TRUE(status.vramSynced);
  EXPECT_TRUE(transport.misterAudioEnabled());

  int16_t sound[4]{};
  EXPECT_TRUE(transport.sendAudio(sound, 4, error)) << error;
  transport.close();
  ASSERT_TRUE(endpoint.waitForCommand(kClose));
  endpoint.stop();

  EXPECT_TRUE(sawMode);
  EXPECT_TRUE(sawFrame);
  EXPECT_TRUE(sawAudio);
  EXPECT_TRUE(sawClose);
  EXPECT_EQ(receivedInterlace, expectedInterlaceCode);
  EXPECT_EQ(receivedField, expectedField);
  EXPECT_EQ(receivedSyncLine, 2);
}

INSTANTIATE_TEST_SUITE_P(
    FieldAndProgressiveBuffers, InterlaceTransport,
    testing::Values(
        // Field buffer: interlace code 1, the caller's field is forwarded, and
        // only half the lines are sent.
        std::make_tuple(false, uint8_t(1), uint8_t(1), size_t(6)),
        // Progressive framebuffer: interlace code 2, field index pinned to zero,
        // and every line is sent.
        std::make_tuple(true, uint8_t(2), uint8_t(0), size_t(12))));

TEST(AlignFrame, ProgressiveModesAlwaysUseFieldZero) {
  FakeGroovyEndpoint endpoint(acknowledgeInit());
  ASSERT_TRUE(endpoint.valid());
  GroovyTransport transport;
  std::string error;
  ASSERT_TRUE(transport.open("localhost", 48000, error, endpoint.port()));
  ASSERT_TRUE(transport.switchMode(tinyMode(false), false, error));
  uint32_t frame = 5;
  uint8_t field = 1;
  transport.alignFrame(frame, field);
  EXPECT_EQ(frame, 5u);
  EXPECT_EQ(field, 0);
  const auto stats = transport.stats();
  EXPECT_FALSE(stats.interlacedFieldBuffer);
  EXPECT_TRUE(stats.fieldPhaseValid)
      << "a progressive mode has no field phase to acquire";
}

TEST(AlignFrame, BeforeAnAcknowledgementItContinuesLocallyFromFieldZero) {
  FakeGroovyEndpoint endpoint(acknowledgeInit());
  ASSERT_TRUE(endpoint.valid());
  GroovyTransport transport;
  std::string error;
  ASSERT_TRUE(transport.open("localhost", 48000, error, endpoint.port()));
  ASSERT_TRUE(transport.switchMode(tinyMode(), false, error));

  uint32_t frame = 40;
  uint8_t field = 1;
  transport.alignFrame(frame, field);
  EXPECT_EQ(frame, 40u) << "no FPGA phase means no frame correction";
  EXPECT_EQ(field, 0) << "the mode switch deterministically starts at field 0";
  auto stats = transport.stats();
  EXPECT_EQ(stats.outgoingField, 0);
  EXPECT_TRUE(stats.interlacedFieldBuffer);
  EXPECT_FALSE(stats.fieldPhaseValid);

  for (uint32_t next = 41; next <= 44; ++next) {
    frame = next;
    transport.alignFrame(frame, field);
    EXPECT_EQ(field, (next - 40) & 1) << "fields alternate from the anchor";
  }
  EXPECT_EQ(transport.stats().fieldRealignments, 0u);
}

TEST(AlignFrame, AdoptsTheFpgaPhaseOnceABlitIsAcknowledged) {
  unsigned blits = 0;
  FakeGroovyEndpoint endpoint([&](auto& peer, const auto& packet) {
    if (packet.empty()) return;
    if (packet[0] == kInit) {
      peer.replyVersion();
    } else if (packet[0] == kBlit) {
      const unsigned index = blits++;
      // The first ACK reports FPGA frame 42 in field 0; the next reports 43 in
      // field 1 (status bit 0x20).
      acknowledgeBlit(peer, packet, index == 0 ? 42 : 43, 1,
                      index == 0 ? 0x00 : 0x20);
    }
  });
  ASSERT_TRUE(endpoint.valid());

  GroovyTransport transport;
  std::string error;
  ASSERT_TRUE(transport.open("localhost", 48000, error, endpoint.port()));
  ASSERT_TRUE(transport.switchMode(tinyMode(), false, error));
  transport.setSyncOptions(true, 0);
  const std::vector<uint8_t> pixels(6, 42);

  uint32_t frame = 40;
  uint8_t field = 1;
  transport.alignFrame(frame, field);
  ASSERT_TRUE(transport.sendFrame(frame, field, pixels, error));
  transport.waitSync();
  auto stats = transport.stats();
  EXPECT_TRUE(stats.fieldPhaseValid);
  EXPECT_EQ(stats.fpgaFrame, 42u);
  EXPECT_EQ(stats.fpgaField, 0);

  // The FPGA is already at 42, so frame 41 is pulled forward to 43 and the field
  // phase jumps, which counts as one realignment.
  frame = 41;
  transport.alignFrame(frame, field);
  stats = transport.stats();
  EXPECT_EQ(frame, 43u);
  EXPECT_EQ(field, 0);
  EXPECT_EQ(stats.fieldRealignments, 1u);

  ASSERT_TRUE(transport.sendFrame(frame, field, pixels, error));
  transport.waitSync();
  frame = 44;
  transport.alignFrame(frame, field);
  EXPECT_EQ(field, 1);
  EXPECT_EQ(transport.stats().fpgaField, 1);
  frame = 45;
  transport.alignFrame(frame, field);
  EXPECT_EQ(field, 0);
  EXPECT_EQ(transport.stats().fieldRealignments, 1u)
      << "continuing the established phase is not a realignment";
}

TEST(AlignFrame, ASecondModeSwitchDiscardsTheAcquiredPhase) {
  FakeGroovyEndpoint endpoint([](auto& peer, const auto& packet) {
    if (packet.empty()) return;
    if (packet[0] == kInit)
      peer.replyVersion();
    else if (packet[0] == kBlit)
      acknowledgeBlit(peer, packet, 42, 1, 0x00);
  });
  ASSERT_TRUE(endpoint.valid());
  GroovyTransport transport;
  std::string error;
  ASSERT_TRUE(transport.open("localhost", 48000, error, endpoint.port()));
  ASSERT_TRUE(transport.switchMode(tinyMode(), false, error));
  transport.setSyncOptions(true, 0);
  uint32_t frame = 40;
  uint8_t field = 0;
  transport.alignFrame(frame, field);
  ASSERT_TRUE(transport.sendFrame(frame, field, std::vector<uint8_t>(6, 1),
                                  error));
  transport.waitSync();
  ASSERT_TRUE(transport.stats().fieldPhaseValid);

  ASSERT_TRUE(transport.switchMode(tinyMode(), false, error));
  frame = 46;
  field = 1;
  transport.alignFrame(frame, field);
  const auto stats = transport.stats();
  EXPECT_EQ(field, 0) << "the new mode restarts from field zero";
  EXPECT_FALSE(stats.fieldPhaseValid);
  EXPECT_EQ(stats.fieldRealignments, 0u);
}

TEST(AlignFrame, OrdersFrameNumbersCorrectlyAcrossTheCounterWraparound) {
  FakeGroovyEndpoint endpoint([](auto& peer, const auto& packet) {
    if (packet.empty()) return;
    if (packet[0] == kInit)
      peer.replyVersion();
    else if (packet[0] == kBlit)
      acknowledgeBlit(peer, packet, UINT32_MAX, 1, 0x00);
  });
  ASSERT_TRUE(endpoint.valid());
  GroovyTransport transport;
  std::string error;
  ASSERT_TRUE(transport.open("localhost", 48000, error, endpoint.port()));
  ASSERT_TRUE(transport.switchMode(tinyMode(), false, error));
  transport.setSyncOptions(true, 0);
  uint32_t frame = UINT32_MAX;
  uint8_t field = 1;
  transport.alignFrame(frame, field);
  ASSERT_TRUE(transport.sendFrame(frame, field, std::vector<uint8_t>(6, 42),
                                  error));
  transport.waitSync();

  frame = 0;
  transport.alignFrame(frame, field);
  EXPECT_EQ(frame, 0u) << "zero must be treated as newer than UINT32_MAX";
  EXPECT_EQ(field, 0);
  EXPECT_TRUE(transport.stats().fieldPhaseValid);
}

// --------------------------------------------------------- receiver diagnostics

TEST(ReceiverDiagnostics, CountsUnhealthyStatesOncePerAcknowledgedFrame) {
  FakeGroovyEndpoint endpoint([](auto& peer, const auto& packet) {
    if (packet.empty()) return;
    if (packet[0] == kInit) {
      peer.replyVersion();
    } else if (packet[0] == kBlit) {
      const auto frame = packetU32(packet, 1);
      if (frame == UINT32_MAX) {
        // Two statuses for the same frame plus a stale one for an older frame:
        // only the first may be counted, and the stale one must be ignored.
        peer.replyAck({frame, 0, 10, 2, 0x8c});
        peer.replyAck({frame, 0, 11, 3, 0x04});
        peer.replyAck({frame - 1, 0, 99, 4, 0x00});
      } else {
        peer.replyAck({frame, 0, 12, 4, 0x00});
      }
    }
  });
  ASSERT_TRUE(endpoint.valid());
  GroovyTransport transport;
  std::string error;
  ASSERT_TRUE(transport.open("localhost", 48000, error, endpoint.port()));
  ASSERT_TRUE(transport.switchMode(tinyMode(false), false, error));
  const std::vector<uint8_t> pixels(12, 42);

  ASSERT_TRUE(transport.sendFrame(UINT32_MAX, 0, pixels, error));
  ASSERT_TRUE(endpoint.waitForHandledCommand(kBlit));
  GroovyTransportTestPeer::drainPendingStatus(transport, UINT32_MAX);
  transport.waitSync();
  auto status = transport.stats();
  EXPECT_EQ(status.fpgaStatusSamples, 1u);
  EXPECT_EQ(status.fpgaFallbackSamples, 1u);
  EXPECT_EQ(status.vramUnsyncedSamples, 0u);
  EXPECT_EQ(status.vramQueueEmptySamples, 0u);
  EXPECT_EQ(status.fpgaFrame, 11u) << "the newest status for the frame wins";
  EXPECT_TRUE(status.vramSynced);
  EXPECT_FALSE(status.vgaFrameskip);
  EXPECT_FALSE(status.vramQueuePresent);

  ASSERT_TRUE(transport.sendFrame(0, 0, pixels, error));
  ASSERT_TRUE(endpoint.waitForHandledCommand(kBlit, 2));
  GroovyTransportTestPeer::drainPendingStatus(transport, 0);
  transport.waitSync();
  status = transport.stats();
  EXPECT_EQ(status.fpgaStatusSamples, 2u);
  EXPECT_EQ(status.fpgaFallbackSamples, 1u);
  EXPECT_EQ(status.vramUnsyncedSamples, 1u);
  EXPECT_EQ(status.vramQueueEmptySamples, 1u);
  EXPECT_EQ(status.acknowledgedFrame, 0u);
  EXPECT_EQ(status.fpgaFrame, 12u);
  EXPECT_FALSE(status.vramSynced);
}

TEST(ReceiverDiagnostics, IgnoresRepliesThatAreNotThirteenBytes) {
  FakeGroovyEndpoint endpoint([](auto& peer, const auto& packet) {
    if (packet.empty()) return;
    if (packet[0] == kInit) {
      peer.replyVersion();
    } else if (packet[0] == kBlit) {
      const uint8_t tooShort[12]{};
      const uint8_t tooLong[14]{};
      peer.reply(tooShort, sizeof(tooShort));
      peer.reply(tooLong, sizeof(tooLong));
      acknowledgeBlit(peer, packet, 5, 1, kHealthy);
    }
  });
  ASSERT_TRUE(endpoint.valid());
  GroovyTransport transport;
  std::string error;
  ASSERT_TRUE(transport.open("localhost", 48000, error, endpoint.port()));
  ASSERT_TRUE(transport.switchMode(tinyMode(false), false, error));
  ASSERT_TRUE(transport.sendFrame(1, 0, std::vector<uint8_t>(12, 42), error));
  ASSERT_TRUE(endpoint.waitForHandledCommand(kBlit));
  GroovyTransportTestPeer::drainPendingStatus(transport, 1);
  const auto status = transport.stats();
  EXPECT_EQ(status.fpgaFrame, 5u) << "only the well-formed status was applied";
  EXPECT_EQ(status.fpgaStatusSamples, 1u);
}

TEST(ReceiverDiagnostics, ExposesEveryStatusBit) {
  FakeGroovyEndpoint endpoint([](auto& peer, const auto& packet) {
    if (!packet.empty() && packet[0] == kInit)
      peer.replyAck({0, 0, 1, 2, 0xff});
  });
  ASSERT_TRUE(endpoint.valid());
  GroovyTransport transport;
  std::string error;
  ASSERT_TRUE(transport.open("localhost", 48000, error, endpoint.port()));
  const auto stats = transport.stats();
  EXPECT_TRUE(stats.vramSynced);
  EXPECT_TRUE(stats.vgaFrameskip);
  EXPECT_TRUE(stats.vgaVblank);
  EXPECT_TRUE(stats.vramQueuePresent);
  EXPECT_EQ(stats.fpgaField, 1);
  EXPECT_TRUE(transport.misterAudioEnabled());
}

// ------------------------------------------------------------------ adaptive

TEST(AdaptiveTransport, WalksTheReserveDownAndResetsOnUnhealthyFeedback) {
  FakeGroovyEndpoint endpoint([](auto& peer, const auto& packet) {
    if (packet.empty()) return;
    if (packet[0] == kInit) {
      peer.replyVersion();
    } else if (packet[0] == kSwitchMode) {
      peer.replyAck({0, 1, 0, 1, kHealthy});
    } else if (packet[0] == kBlit) {
      const auto frame = packetU32(packet, 1);
      if (frame == 611) return;  // a missing ACK
      const uint8_t status = frame == 612 ? 0 : kHealthy;
      if (frame == 2) peer.replyAck({1, 1, 2, 1, kHealthy});
      acknowledgeBlit(peer, packet, frame + 1, 1, status);
      if (frame == 1) acknowledgeBlit(peer, packet, frame + 1, 1, status);
    }
  });
  ASSERT_TRUE(endpoint.valid());

  GroovyTransport transport;
  std::string error;
  ASSERT_TRUE(transport.open("localhost", 48000, error, endpoint.port()));
  ASSERT_TRUE(transport.switchMode(tinyMode(), false, error));
  transport.setSyncOptions(true, 0);
  const std::vector<uint8_t> pixels(6, 42);
  for (uint32_t frame = 1; frame <= 300; ++frame) {
    ASSERT_TRUE(transport.sendFrame(frame, frame & 1, pixels, error)) << frame;
    transport.waitSync();
    // A duplicated status for the same frame must count once, and a status for
    // an older frame must not count at all.
    if (frame == 1) {
      EXPECT_EQ(transport.stats().adaptiveHealthyAcks, 1u);
    }
    if (frame == 2) {
      EXPECT_EQ(transport.stats().adaptiveHealthyAcks, 2u);
    }
  }
  auto stats = transport.stats();
  EXPECT_TRUE(stats.adaptiveTimingEligible);
  EXPECT_EQ(stats.deliveryReserveLines, 1);
  EXPECT_EQ(stats.adaptiveLatestSafeLine, 4);
  EXPECT_EQ(stats.adaptiveReductions, 1u);

  // A manual frame delay takes the margin out of automatic mode entirely.
  transport.setSyncOptions(true, 1);
  stats = transport.stats();
  EXPECT_FALSE(stats.adaptiveTimingEligible);
  EXPECT_EQ(stats.deliveryReserveLines, 2);
  EXPECT_EQ(stats.adaptiveHealthyAcks, 0u);
  transport.setSyncOptions(true, 0);
  EXPECT_TRUE(transport.stats().adaptiveTimingEligible);
  EXPECT_EQ(transport.stats().deliveryReserveLines, 2);

  for (uint32_t frame = 301; frame <= 600; ++frame) {
    ASSERT_TRUE(transport.sendFrame(frame, frame & 1, pixels, error));
    transport.waitSync();
  }
  EXPECT_EQ(transport.stats().deliveryReserveLines, 1);
  for (uint32_t frame = 601; frame <= 610; ++frame) {
    ASSERT_TRUE(transport.sendFrame(frame, frame & 1, pixels, error));
    transport.waitSync();
  }
  stats = transport.stats();
  EXPECT_EQ(stats.deliveryReserveLines, 1);
  EXPECT_EQ(stats.adaptiveHealthyAcks, 10u);

  ASSERT_TRUE(transport.sendFrame(611, 1, pixels, error));
  transport.waitSync();
  stats = transport.stats();
  EXPECT_EQ(stats.deliveryReserveLines, 1)
      << "a lost ACK breaks the run without giving up the gain";
  EXPECT_EQ(stats.adaptiveHealthyAcks, 0u);

  ASSERT_TRUE(transport.sendFrame(612, 0, pixels, error));
  transport.waitSync();
  stats = transport.stats();
  EXPECT_EQ(stats.deliveryReserveLines, 2) << "unhealthy feedback resets";
  EXPECT_EQ(stats.adaptiveResets, 1u);

  ASSERT_TRUE(transport.switchMode(tinyMode(), false, error));
  stats = transport.stats();
  EXPECT_FALSE(stats.adaptiveTimingEligible);
  EXPECT_EQ(stats.adaptiveReductions, 0u);
  EXPECT_EQ(stats.adaptiveResets, 0u);
  transport.close();
}


TEST(AlignFrame, TheProgressiveBufferAlwaysReportsFieldZeroOnTheWire) {
  FakeGroovyEndpoint endpoint([](auto& peer, const auto& packet) {
    if (packet.empty()) return;
    if (packet[0] == kInit)
      peer.replyVersion();
    else if (packet[0] == kBlit)
      acknowledgeBlit(peer, packet, packetU32(packet, 1), 1, 0x20 | kHealthy);
  });
  ASSERT_TRUE(endpoint.valid());
  GroovyTransport transport;
  std::string error;
  ASSERT_TRUE(transport.open("localhost", 48000, error, endpoint.port()));
  // Interlaced timings with the progressive framebuffer opt-in.
  ASSERT_TRUE(transport.switchMode(tinyMode(true), /*progressive=*/true, error));
  transport.setSyncOptions(true, 0);
  const std::vector<uint8_t> pixels(12, 7);
  uint32_t frame = 1;
  uint8_t field = 1;
  for (int i = 0; i < 3; ++i, ++frame) {
    transport.alignFrame(frame, field);
    const auto stats = transport.stats();
    EXPECT_EQ(stats.outgoingField, 0)
        << "a single framebuffer never alternates the field index";
    EXPECT_TRUE(stats.fieldPhaseValid);
    EXPECT_FALSE(stats.interlacedFieldBuffer);
    ASSERT_TRUE(transport.sendFrame(frame, field, pixels, error)) << error;
    transport.waitSync();
  }
  // The internal field phase still tracks the FPGA, and realignments are still
  // counted from it; what must never change is the index put on the wire.
  transport.close();
}

}  // namespace
