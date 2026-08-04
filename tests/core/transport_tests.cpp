#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <fcntl.h>
#include <netinet/ip.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <thread>
#include <optional>
#include <vector>

#include "mistercast/groovy_protocol.hpp"
#include "mistercast/groovy_transport.hpp"
#include "mistercast/udp_pacing.hpp"
#include "support/fake_groovy_endpoint.hpp"

namespace mistercast {
// The transport keeps its socket and status decoding private on purpose; this
// declared friend is the single seam the tests use, rather than widening the
// production interface.
class GroovyTransportTestPeer {
 public:
  static std::unique_ptr<GroovyTransport> withVideoConfig(
      UdpVideoConfig config, UdpSubmitSyscalls* syscalls = nullptr) {
    return std::unique_ptr<GroovyTransport>(
        new GroovyTransport(std::make_unique<UdpVideoSender>(config, syscalls)));
  }
  static std::unique_ptr<GroovyTransport> withVideoSyscalls(
      UdpSubmitSyscalls& syscalls) {
    UdpVideoConfig config{GroovyUdpPayloadBytes, GroovyFramebufferBytes,
                          GroovyUdpWireOverheadBytes, 950000000, 32, 100000};
    return withVideoConfig(config, &syscalls);
  }
  static std::optional<int> pathMtuDiscoveryMode(
      const GroovyTransport& transport) {
    int mode = 0;
    socklen_t size = sizeof(mode);
    if (getsockopt(transport.fd_, IPPROTO_IP, IP_MTU_DISCOVER, &mode, &size) !=
        0)
      return std::nullopt;
    return mode;
  }
  static void drainPendingStatus(GroovyTransport& transport,
                                 uint32_t expectedFrame) {
    transport.drainStatus(expectedFrame);
  }
};
}  // namespace mistercast

using namespace mistercast;
using namespace mistercast::test;
using testing::ElementsAre;
using testing::HasSubstr;

namespace {

constexpr uint8_t kClose = 1, kInit = 2, kSwitchMode = 3, kAudio = 4,
                  kBlit = 7;
// Receiver status bits: 0x04 VRAM synced, 0x08 VGA frameskip, 0x10 vblank,
// 0x20 FPGA field, 0x40 core audio on, 0x80 VRAM queue present.
constexpr uint8_t kHealthy = 0x84, kHealthyWithAudio = 0xc4;

UdpVideoConfig groovyVideoConfig() {
  return {GroovyUdpPayloadBytes, GroovyFramebufferBytes,
          GroovyUdpWireOverheadBytes, 950000000, 32, 100000};
}

// 2x2 active, 5x5 total: small enough that a frame is a handful of bytes and the
// automatic sync-line calculation is always in its "work exceeds a frame period"
// regime.
Modeline tinyMode(bool interlaced = true) {
  return {"tiny", 1, 2, 3, 4, 5, 2, 3, 4, 5, interlaced};
}

Modeline vgaMode(bool interlaced = false) {
  return {"vga", 25.175, 640, 656, 752, 800, 480, 490, 492, 525, interlaced};
}

// Replies to CMD_INIT only, which is all most tests need to get connected.
FakeGroovyEndpoint::Handler acknowledgeInit() {
  return [](FakeGroovyEndpoint& peer, const FakeGroovyEndpoint::Packet& packet) {
    if (!packet.empty() && packet[0] == kInit) peer.replyVersion();
  };
}

void acknowledgeBlit(FakeGroovyEndpoint& endpoint,
                     const FakeGroovyEndpoint::Packet& packet,
                     uint32_t fpgaFrame, uint16_t fpgaLine,
                     uint8_t statusBits) {
  endpoint.replyAck({packetU32(packet, 1), packetU16(packet, 6), fpgaFrame,
                     fpgaLine, statusBits});
}

// --------------------------------------------------------------- open and close

TEST(TransportOpen, RequiresATargetAddress) {
  GroovyTransport transport;
  std::string error;
  EXPECT_FALSE(transport.open("", std::nullopt, error));
  EXPECT_EQ(error, "target address is required");
  EXPECT_FALSE(transport.connected());
}

TEST(TransportOpen, ReportsAnUnresolvableTarget) {
  GroovyTransport transport;
  std::string error;
  EXPECT_FALSE(transport.open("mistercast.invalid.", std::nullopt, error));
  EXPECT_THAT(error, HasSubstr("cannot resolve IPv4 target"));
  EXPECT_FALSE(transport.connected());
}

TEST(TransportOpen, RejectsAnUnsupportedAudioRateBeforeSendingAnything) {
  FakeGroovyEndpoint endpoint(acknowledgeInit());
  ASSERT_TRUE(endpoint.valid());
  GroovyTransport transport;
  std::string error;
  EXPECT_FALSE(transport.open("localhost", 96000, error, endpoint.port()));
  EXPECT_EQ(error, "unsupported audio sample rate");
  EXPECT_FALSE(transport.connected());
  endpoint.stop();
  for (const auto& packet : endpoint.packets())
    EXPECT_TRUE(packet.empty() || packet[0] != kInit)
        << "nothing may be sent once the rate is known to be unusable";
}

TEST(TransportOpen, NegotiatesTheAudioRateCodeAndChannelCount) {
  const struct {
    std::optional<uint32_t> audioRate;
    uint8_t rateCode, channelCode;
  } cases[] = {{std::nullopt, 0, 0},
               {22050, 1, 2},
               {44100, 2, 2},
               {48000, 3, 2}};
  for (const auto& test : cases) {
    FakeGroovyEndpoint endpoint(acknowledgeInit());
    ASSERT_TRUE(endpoint.valid());
    GroovyTransport transport;
    std::string error;
    ASSERT_TRUE(transport.open("localhost", test.audioRate, error,
                               endpoint.port()))
        << error;
    EXPECT_TRUE(transport.connected());

    const FakeGroovyEndpoint::Packet expected{
        kInit, uint8_t(compressionAvailable()), test.rateCode, test.channelCode,
        0};
    EXPECT_THAT(endpoint.packets(), testing::Contains(expected));

    if (!test.audioRate) {
      int16_t samples[2]{};
      EXPECT_FALSE(transport.sendAudio(samples, 2, error));
      EXPECT_EQ(error, "audio was not negotiated for this transport");
      for (const auto& packet : endpoint.packets())
        EXPECT_TRUE(packet.empty() || packet[0] != kAudio);
    }
    transport.close();
    endpoint.stop();
  }
}

TEST(TransportOpen, ReportsThatNoSocketCouldBeCreatedAtAll) {
  // Lower this process's descriptor limit and hold what is left, so socket()
  // fails for every resolved candidate. The transport then has no candidate
  // error to report and must say it could not connect at all.
  rlimit original{};
  ASSERT_EQ(::getrlimit(RLIMIT_NOFILE, &original), 0);
  rlimit limited = original;
  limited.rlim_cur = 64;
  if (::setrlimit(RLIMIT_NOFILE, &limited) != 0)
    GTEST_SKIP() << "the descriptor limit cannot be lowered here";

  std::vector<int> held;
  for (;;) {
    const int descriptor = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) break;
    held.push_back(descriptor);
  }

  GroovyTransport transport;
  std::string error;
  const bool opened = transport.open("127.0.0.1", std::nullopt, error, 32100);

  for (const int descriptor : held) ::close(descriptor);
  ASSERT_EQ(::setrlimit(RLIMIT_NOFILE, &original), 0);

  EXPECT_FALSE(opened);
  EXPECT_EQ(error, "cannot create UDP connection to target");
  EXPECT_FALSE(transport.connected());
}

TEST(TransportOpen, EnforcesStrictIpv4PathMtuDiscovery) {
  FakeGroovyEndpoint endpoint(acknowledgeInit());
  ASSERT_TRUE(endpoint.valid());
  GroovyTransport transport;
  std::string error;
  ASSERT_TRUE(transport.open("localhost", 48000, error, endpoint.port()));
  const auto mode = GroovyTransportTestPeer::pathMtuDiscoveryMode(transport);
  ASSERT_TRUE(mode.has_value());
  EXPECT_EQ(*mode, IP_PMTUDISC_DO)
      << "IPv4 fragmentation would silently break frame reassembly";
}

TEST(TransportOpen, RecordsTheSocketSendBufferAndLoopbackPathMtu) {
  FakeGroovyEndpoint endpoint(acknowledgeInit());
  ASSERT_TRUE(endpoint.valid());
  GroovyTransport transport;
  std::string error;
  ASSERT_TRUE(transport.open("localhost", 48000, error, endpoint.port()));
  const auto stats = transport.stats();
  EXPECT_GT(stats.socketSendBufferBytes, 0u);
  EXPECT_GE(stats.pathMtu, 1500u) << "loopback has a large MTU";
}

TEST(TransportOpen, RefusesATargetThatNeverAcknowledgesTheInit) {
  // A silent endpoint: bound so the datagram is not refused, but never replying.
  FakeGroovyEndpoint endpoint([](auto&, const auto&) {});
  ASSERT_TRUE(endpoint.valid());
  GroovyTransport transport;
  std::string error;
  EXPECT_FALSE(transport.open("localhost", std::nullopt, error,
                              endpoint.port()));
  EXPECT_THAT(error, HasSubstr("did not acknowledge CMD_INIT"));
  EXPECT_FALSE(transport.connected());
}

TEST(TransportOpen, RefusesAReplyThatIsNotTheGroovyProtocol) {
  FakeGroovyEndpoint endpoint([](auto& peer, const auto& packet) {
    if (!packet.empty() && packet[0] == kInit) {
      const uint8_t nonsense[5]{1, 2, 3, 4, 5};
      peer.reply(nonsense, sizeof(nonsense));
    }
  });
  ASSERT_TRUE(endpoint.valid());
  GroovyTransport transport;
  std::string error;
  EXPECT_FALSE(transport.open("localhost", std::nullopt, error,
                              endpoint.port()));
  EXPECT_EQ(error, "invalid CMD_INIT acknowledgment");
  EXPECT_FALSE(transport.connected());
}

TEST(TransportOpen, AcceptsAThirteenByteStatusAsTheInitAcknowledgment) {
  // Some cores answer the init with a full status frame instead of a version
  // byte; that status must be adopted immediately.
  FakeGroovyEndpoint endpoint([](auto& peer, const auto& packet) {
    if (!packet.empty() && packet[0] == kInit)
      peer.replyAck({0, 0, 77, 5, kHealthyWithAudio});
  });
  ASSERT_TRUE(endpoint.valid());
  GroovyTransport transport;
  std::string error;
  ASSERT_TRUE(transport.open("localhost", 48000, error, endpoint.port()));
  const auto stats = transport.stats();
  EXPECT_EQ(stats.fpgaFrame, 77u);
  EXPECT_EQ(stats.fpgaVCount, 5);
  EXPECT_TRUE(stats.vramSynced);
  EXPECT_TRUE(transport.misterAudioEnabled());
}

TEST(TransportOpen, MeasuresAStartupRoundTrip) {
  FakeGroovyEndpoint endpoint(acknowledgeInit());
  ASSERT_TRUE(endpoint.valid());
  GroovyTransport transport;
  std::string error;
  ASSERT_TRUE(transport.open("localhost", 48000, error, endpoint.port()));
  EXPECT_GT(transport.stats().networkRttUs, 0u);
}

TEST(TransportClose, IsIdempotentAndClearsNegotiatedState) {
  FakeGroovyEndpoint endpoint([](auto& peer, const auto& packet) {
    if (!packet.empty() && packet[0] == kInit)
      peer.replyAck({0, 0, 3, 4, kHealthyWithAudio});
  });
  ASSERT_TRUE(endpoint.valid());
  GroovyTransport transport;
  std::string error;
  ASSERT_TRUE(transport.open("localhost", 48000, error, endpoint.port()));
  ASSERT_TRUE(transport.switchMode(tinyMode(), false, error));
  ASSERT_TRUE(transport.misterAudioEnabled());

  transport.close();
  EXPECT_FALSE(transport.connected());
  EXPECT_FALSE(transport.misterAudioEnabled());
  auto stats = transport.stats();
  EXPECT_FALSE(stats.vramSynced);
  EXPECT_FALSE(stats.interlacedFieldBuffer);
  EXPECT_EQ(stats.pathMtu, 0u);
  EXPECT_EQ(stats.socketSendBufferBytes, 0u);

  transport.close();  // must not send another CMD_CLOSE or crash
  ASSERT_TRUE(endpoint.waitForCommand(kClose));
  endpoint.stop();
  size_t closes = 0;
  for (const auto& packet : endpoint.packets())
    if (packet.size() == 1 && packet[0] == kClose) ++closes;
  EXPECT_EQ(closes, 1u);

  // Audio is no longer negotiated, and frames can no longer be sent.
  int16_t samples[2]{};
  EXPECT_FALSE(transport.sendAudio(samples, 2, error));
  EXPECT_FALSE(transport.sendFrame(1, 0, std::vector<uint8_t>(6, 1), error));
}

TEST(TransportClose, ReopeningClearsTheReceiverHealthCounters) {
  FakeGroovyEndpoint endpoint([](auto& peer, const auto& packet) {
    if (packet.empty()) return;
    if (packet[0] == kInit)
      peer.replyVersion();
    else if (packet[0] == kBlit)
      acknowledgeBlit(peer, packet, packetU32(packet, 1), 1, 0x00);
  });
  ASSERT_TRUE(endpoint.valid());
  GroovyTransport transport;
  std::string error;
  ASSERT_TRUE(transport.open("localhost", 48000, error, endpoint.port()));
  ASSERT_TRUE(transport.switchMode(tinyMode(false), false, error));
  ASSERT_TRUE(transport.sendFrame(1, 0, std::vector<uint8_t>(12, 42), error));
  ASSERT_TRUE(endpoint.waitForHandledCommand(kBlit));
  GroovyTransportTestPeer::drainPendingStatus(transport, 1);
  ASSERT_GT(transport.stats().fpgaStatusSamples, 0u);

  transport.close();
  ASSERT_TRUE(transport.open("localhost", 48000, error, endpoint.port()));
  const auto stats = transport.stats();
  EXPECT_EQ(stats.fpgaStatusSamples, 0u);
  EXPECT_EQ(stats.fpgaFallbackSamples, 0u);
  EXPECT_EQ(stats.vramUnsyncedSamples, 0u);
  EXPECT_EQ(stats.vramQueueEmptySamples, 0u);
  EXPECT_EQ(stats.sendErrors, 0u);
  EXPECT_EQ(stats.acknowledgedFrames, 0u);
  EXPECT_FALSE(stats.vramQueuePresent);
  transport.close();
}

// --------------------------------------------------------------- mode switching

TEST(SwitchMode, RejectsTimingsTheCoreCannotDisplay) {
  FakeGroovyEndpoint endpoint(acknowledgeInit());
  ASSERT_TRUE(endpoint.valid());
  GroovyTransport transport;
  std::string error;
  ASSERT_TRUE(transport.open("localhost", 48000, error, endpoint.port()));
  auto broken = tinyMode();
  broken.vTotal = 1;
  EXPECT_FALSE(transport.switchMode(broken, false, error));
  EXPECT_THAT(error, HasSubstr("vertical timings"));

  Modeline oversized{"1024x768", 65.0,  1024, 1048, 1184,
                     1344,       768,  771,  777,  806, false};
  EXPECT_FALSE(transport.switchMode(oversized, false, error));
  EXPECT_THAT(error, HasSubstr("exceeds Groovy_MiSTer frame buffer"));
}

TEST(SwitchMode, EncodesTheTimingsAndInterlaceModeOnTheWire) {
  FakeGroovyEndpoint endpoint(acknowledgeInit());
  ASSERT_TRUE(endpoint.valid());
  GroovyTransport transport;
  std::string error;
  ASSERT_TRUE(transport.open("localhost", 48000, error, endpoint.port()));
  const auto mode = vgaMode();
  ASSERT_TRUE(transport.switchMode(mode, false, error));
  ASSERT_TRUE(endpoint.waitForCommand(kSwitchMode));
  transport.close();
  endpoint.stop();

  FakeGroovyEndpoint::Packet command;
  for (const auto& packet : endpoint.packets())
    if (packet.size() == 26 && packet[0] == kSwitchMode) command = packet;
  ASSERT_EQ(command.size(), 26u);
  double clock = 0;
  std::memcpy(&clock, command.data() + 1, 8);
  EXPECT_DOUBLE_EQ(clock, mode.pixelClockMHz);
  EXPECT_EQ(packetU16(command, 9), mode.hActive);
  EXPECT_EQ(packetU16(command, 11), mode.hBegin);
  EXPECT_EQ(packetU16(command, 13), mode.hEnd);
  EXPECT_EQ(packetU16(command, 15), mode.hTotal);
  EXPECT_EQ(packetU16(command, 17), mode.vActive);
  EXPECT_EQ(packetU16(command, 19), mode.vBegin);
  EXPECT_EQ(packetU16(command, 21), mode.vEnd);
  EXPECT_EQ(packetU16(command, 23), mode.vTotal);
  EXPECT_EQ(command[25], 0) << "a progressive mode carries interlace code 0";
}

TEST(SwitchMode, FailsOnceTheTransportIsClosed) {
  GroovyTransport transport;
  std::string error;
  EXPECT_FALSE(transport.switchMode(tinyMode(), false, error));
  EXPECT_EQ(error, "transport is closed");
}

TEST(SetSyncOptions, ClampsTheFrameDelayToTen) {
  FakeGroovyEndpoint endpoint(acknowledgeInit());
  ASSERT_TRUE(endpoint.valid());
  GroovyTransport transport;
  std::string error;
  ASSERT_TRUE(transport.open("localhost", 48000, error, endpoint.port()));
  ASSERT_TRUE(transport.switchMode(vgaMode(), false, error));
  transport.setSyncOptions(true, 99);
  const std::vector<uint8_t> pixels(size_t(640) * 480 * 3, 42);
  ASSERT_TRUE(transport.sendFrame(1, 0, pixels, error));
  // A frame delay of 10 asks for the last line of the raster.
  EXPECT_EQ(transport.stats().requestedSyncLine, 525);
}

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
  UdpVideoConfig config{5, 12, GroovyUdpWireOverheadBytes, 950000000, 32,
                        100000};
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

TEST(CompressionAvailability, MatchesTheInitCommandItAdvertises) {
  FakeGroovyEndpoint endpoint(acknowledgeInit());
  ASSERT_TRUE(endpoint.valid());
  GroovyTransport transport;
  std::string error;
  ASSERT_TRUE(transport.open("localhost", 48000, error, endpoint.port()));
  transport.close();
  endpoint.stop();
  for (const auto& packet : endpoint.packets()) {
    if (packet.size() == 5 && packet[0] == kInit) {
      EXPECT_EQ(packet[1], uint8_t(compressionAvailable()));
    }
  }
}

}  // namespace
