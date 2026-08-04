#include "transport_test_support.hpp"

#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

using namespace mistercast;
using namespace mistercast::test;
using testing::HasSubstr;

namespace {
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
