#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

#include "fake_groovy_endpoint.hpp"
#include "mistercast/groovy_protocol.hpp"
#include "mistercast/groovy_transport.hpp"
#include "mistercast/udp_pacing.hpp"

namespace mistercast {
class GroovyTransportTestPeer {
 public:
  static std::unique_ptr<GroovyTransport> withVideoConfig(
      UdpVideoConfig config, UdpSubmitSyscalls* syscalls = nullptr) {
    return std::unique_ptr<GroovyTransport>(new GroovyTransport(
        std::make_unique<UdpVideoSender>(config, syscalls)));
  }
  static std::unique_ptr<GroovyTransport> withVideoSyscalls(
      UdpSubmitSyscalls& syscalls) {
    UdpVideoConfig config{GroovyUdpPayloadBytes, GroovyFramebufferBytes,
                          GroovyUdpWireOverheadBytes, 950000000, 32, 100000};
    return withVideoConfig(config, &syscalls);
  }
};
}  // namespace mistercast

using namespace mistercast;
using namespace mistercast::test;

namespace {
int failed = 0;
#define CHECK(x)                                                              \
  do {                                                                        \
    if (!(x)) {                                                               \
      std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #x "\n"; \
      ++failed;                                                               \
    }                                                                         \
  } while (0)

constexpr uint8_t kClose = 1, kInit = 2, kSwitchMode = 3, kAudio = 4,
                  kBlit = 7;

UdpVideoConfig groovyVideoConfig() {
  return {GroovyUdpPayloadBytes, GroovyFramebufferBytes,
          GroovyUdpWireOverheadBytes, 950000000, 32, 100000};
}

Modeline tinyMode(bool interlaced = true) {
  return {"tiny", 1, 2, 3, 4, 5, 2, 3, 4, 5, interlaced};
}

void acknowledgeBlit(FakeGroovyEndpoint& endpoint,
                     const FakeGroovyEndpoint::Packet& packet,
                     uint32_t fpgaFrame, uint16_t fpgaLine,
                     uint8_t statusBits) {
  endpoint.replyAck({packetU32(packet, 1), packetU16(packet, 6), fpgaFrame,
                     fpgaLine, statusBits});
}

void checkInterlaceTransport(bool progressive, uint8_t sentField,
                             uint8_t expectedInterlace,
                             uint8_t expectedField, size_t pixelValues) {
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
    }
  });
  if (!endpoint.valid()) return;

  std::string error;
  GroovyTransport transport;
  CHECK(transport.open("localhost", true, 48000, error, endpoint.port()));
  CHECK(transport.switchMode(tinyMode(), progressive, error));
  transport.setSyncOptions(true, 0);
  CHECK(transport.sendFrame(1, sentField,
                            std::vector<uint8_t>(pixelValues, 42), error));
  transport.waitSync();
  const auto status = transport.stats();
  CHECK(status.acknowledgedFrame == 1 && status.acknowledgedFrames == 1 &&
        status.rasterCorrectionUs < 0 && status.vramSynced &&
        transport.misterAudioEnabled());
  int16_t sound[4]{};
  CHECK(transport.sendAudio(sound, 4, error));
  transport.close();
  CHECK(endpoint.waitForCommand(kClose));
  endpoint.stop();
  CHECK(sawMode && sawFrame && sawAudio && sawClose);
  CHECK(receivedInterlace == expectedInterlace &&
        receivedField == expectedField && receivedSyncLine == 2);
}

void checkFieldAlignment() {
  unsigned blits = 0;
  FakeGroovyEndpoint endpoint([&](auto& peer, const auto& packet) {
    if (packet.empty()) return;
    if (packet[0] == kInit) {
      peer.replyVersion();
    } else if (packet[0] == kBlit) {
      const unsigned index = blits++;
      acknowledgeBlit(peer, packet, index == 0 ? 42 : 43, 1,
                      index == 0 ? 0 : 0x20);
    }
  });
  if (!endpoint.valid()) return;

  std::string error;
  GroovyTransport transport;
  CHECK(transport.open("localhost", true, 48000, error, endpoint.port()));
  CHECK(transport.switchMode(tinyMode(), false, error));
  transport.setSyncOptions(true, 0);
  std::vector<uint8_t> pixels(6, 42);
  uint32_t frame = 40;
  uint8_t field = 1;
  transport.alignFrame(frame, field);
  auto status = transport.stats();
  CHECK(frame == 40 && field == 0 && status.outgoingField == 0 &&
        status.interlacedFieldBuffer && !status.fieldPhaseValid);
  CHECK(transport.sendFrame(frame, field, pixels, error));
  transport.waitSync();
  status = transport.stats();
  CHECK(status.fieldPhaseValid && status.fpgaFrame == 42 &&
        status.fpgaField == 0);

  frame = 41;
  transport.alignFrame(frame, field);
  status = transport.stats();
  CHECK(frame == 43 && field == 0 && status.fieldRealignments == 1);
  CHECK(transport.sendFrame(frame, field, pixels, error));
  transport.waitSync();
  frame = 44;
  transport.alignFrame(frame, field);
  CHECK(field == 1 && transport.stats().fpgaField == 1);
  frame = 45;
  transport.alignFrame(frame, field);
  CHECK(field == 0 && transport.stats().fieldRealignments == 1);

  CHECK(transport.switchMode(tinyMode(), false, error));
  frame = 46;
  field = 1;
  transport.alignFrame(frame, field);
  status = transport.stats();
  CHECK(field == 0 && !status.fieldPhaseValid &&
        status.fieldRealignments == 1);
  transport.close();
}

void checkFieldAlignmentWraparound() {
  FakeGroovyEndpoint endpoint([](auto& peer, const auto& packet) {
    if (packet.empty()) return;
    if (packet[0] == kInit)
      peer.replyVersion();
    else if (packet[0] == kBlit)
      acknowledgeBlit(peer, packet, UINT32_MAX, 1, 0);
  });
  if (!endpoint.valid()) return;
  std::string error;
  GroovyTransport transport;
  CHECK(transport.open("localhost", true, 48000, error, endpoint.port()));
  CHECK(transport.switchMode(tinyMode(), false, error));
  transport.setSyncOptions(true, 0);
  uint32_t frame = UINT32_MAX;
  uint8_t field = 1;
  transport.alignFrame(frame, field);
  CHECK(transport.sendFrame(frame, field, std::vector<uint8_t>(6, 42), error));
  transport.waitSync();
  frame = 0;
  transport.alignFrame(frame, field);
  CHECK(frame == 0 && field == 0 && transport.stats().fieldPhaseValid);
  transport.close();
}

void checkFpgaHealthDiagnostics() {
  FakeGroovyEndpoint endpoint([](auto& peer, const auto& packet) {
    if (packet.empty()) return;
    if (packet[0] == kInit) {
      peer.replyVersion();
    } else if (packet[0] == kBlit) {
      const auto frame = packetU32(packet, 1);
      if (frame == UINT32_MAX) {
        peer.replyAck({frame, 0, 10, 2, 0x8c});
        peer.replyAck({frame, 0, 11, 3, 0x04});
        peer.replyAck({frame - 1, 0, 99, 4, 0x00});
      } else {
        peer.replyAck({frame, 0, 12, 4, 0x00});
      }
    }
  });
  if (!endpoint.valid()) return;
  std::string error;
  GroovyTransport transport;
  CHECK(transport.open("localhost", true, 48000, error, endpoint.port()));
  CHECK(transport.switchMode(tinyMode(false), false, error));
  std::vector<uint8_t> pixels(12, 42);
  CHECK(transport.sendFrame(UINT32_MAX, 0, pixels, error));
  transport.waitSync();
  auto status = transport.stats();
  CHECK(status.fpgaStatusSamples == 1 && status.fpgaFallbackSamples == 1 &&
        status.vramUnsyncedSamples == 0 && status.vramQueueEmptySamples == 0);
  CHECK(status.fpgaFrame == 11 && status.vramSynced &&
        !status.vgaFrameskip && !status.vramQueuePresent);

  CHECK(transport.sendFrame(0, 0, pixels, error));
  transport.waitSync();
  status = transport.stats();
  CHECK(status.fpgaStatusSamples == 2 && status.fpgaFallbackSamples == 1 &&
        status.vramUnsyncedSamples == 1 && status.vramQueueEmptySamples == 1);
  CHECK(status.acknowledgedFrame == 0 && status.fpgaFrame == 12 &&
        !status.vramSynced && !status.vgaFrameskip &&
        !status.vramQueuePresent);

  transport.close();
  status = transport.stats();
  CHECK(status.fpgaStatusSamples == 0 && status.fpgaFallbackSamples == 0 &&
        status.vramUnsyncedSamples == 0 && status.vramQueueEmptySamples == 0 &&
        !status.vramQueuePresent);
  CHECK(transport.open("localhost", true, 48000, error, endpoint.port()));
  status = transport.stats();
  CHECK(status.fpgaStatusSamples == 0 && status.fpgaFallbackSamples == 0 &&
        status.vramUnsyncedSamples == 0 && status.vramQueueEmptySamples == 0 &&
        !status.vramQueuePresent);
  transport.close();
}


void checkAdaptiveDeliveryMargin() {
  AdaptiveDeliveryMargin margin;
  margin.configure(525, true);
  auto stats = margin.stats();
  CHECK(stats.reserveLines == 262 && stats.latestSafeLine == 263 &&
        stats.healthyAcks == 0 && stats.reductions == 0 && stats.resets == 0);
  for (unsigned i = 0; i < 299; ++i) margin.healthyAck();
  stats = margin.stats();
  CHECK(stats.reserveLines == 262 && stats.healthyAcks == 299 &&
        stats.reductions == 0);
  margin.healthyAck();
  stats = margin.stats();
  CHECK(stats.reserveLines == 258 && stats.latestSafeLine == 267 &&
        stats.healthyAcks == 0 && stats.reductions == 1);
  margin.unhealthyAck();
  stats = margin.stats();
  CHECK(stats.reserveLines == 262 && stats.latestSafeLine == 263 &&
        stats.resets == 1);
  margin.unhealthyAck();
  CHECK(margin.stats().resets == 1);
  for (unsigned i = 0; i < 10; ++i) margin.healthyAck();
  margin.unhealthyAck();
  CHECK(margin.stats().resets == 2 && margin.stats().healthyAcks == 0);
  for (unsigned i = 0; i < 300; ++i) margin.healthyAck();
  for (unsigned i = 0; i < 10; ++i) margin.healthyAck();
  margin.missingAck();
  stats = margin.stats();
  CHECK(stats.reserveLines == 258 && stats.healthyAcks == 0 &&
        stats.resets == 2);
  for (unsigned step = 0; step < 20; ++step)
    for (unsigned i = 0; i < AdaptiveHealthyAcksPerStep; ++i)
      margin.healthyAck();
  stats = margin.stats();
  CHECK(stats.reserveLines == 196 && stats.latestSafeLine == 329);
  const auto reductionsAtFloor = stats.reductions;
  for (unsigned i = 0; i < AdaptiveHealthyAcksPerStep; ++i)
    margin.healthyAck();
  CHECK(margin.stats().reserveLines == 196 &&
        margin.stats().reductions == reductionsAtFloor);

  margin.configure(625, true);
  for (unsigned step = 0; step < 20; ++step)
    for (unsigned i = 0; i < AdaptiveHealthyAcksPerStep; ++i)
      margin.healthyAck();
  stats = margin.stats();
  CHECK(stats.reserveLines == 234 && stats.latestSafeLine == 391 &&
        stats.reductions == 20);
  margin.configure(525, false);
  stats = margin.stats();
  CHECK(stats.reserveLines == 0 && stats.latestSafeLine == 0 &&
        stats.reductions == 0 && stats.resets == 0);
  margin.configure(525, true);
  margin.healthyAck();
  margin.configure(625, true);
  stats = margin.stats();
  CHECK(stats.reserveLines == 312 && stats.latestSafeLine == 313 &&
        stats.healthyAcks == 0 && stats.reductions == 0 && stats.resets == 0);
  margin.close();
  CHECK(margin.stats().reserveLines == 0);
}

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


void checkFatalPayloadFailureStopsProtocol() {
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
  if (!endpoint.valid()) return;
  FailingVideoSyscalls syscalls;
  auto transport = GroovyTransportTestPeer::withVideoSyscalls(syscalls);
  std::string error;
  CHECK(transport->open("localhost", true, 48000, error, endpoint.port()));
  CHECK(transport->switchMode(tinyMode(false), false, error));
  CHECK(!transport->sendFrame(1, 0, std::vector<uint8_t>(12, 42), error));
  int16_t audio[2]{};
  CHECK(!transport->sendAudio(audio, 2, error));
  transport->close();
  endpoint.stop();
  CHECK(blits == 1 && laterCommands == 0);
}

void checkSharedAudioVideoPacketization() {
  UdpVideoConfig config{5, 12, GroovyUdpWireOverheadBytes,
                        950000000, 32, 100000};
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
  if (!endpoint.valid()) return;

  auto transport = GroovyTransportTestPeer::withVideoConfig(config);
  std::string error;
  CHECK(transport->open("localhost", true, 48000, error, endpoint.port()));
  CHECK(transport->switchMode(tinyMode(false), false, error));
  int16_t audio[4]{};
  CHECK(transport->sendAudio(audio, 4, error));
  std::vector<uint8_t> pixels(12);
  for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = uint8_t(i * 17);
  CHECK(transport->sendFrame(1, 0, pixels, error));
  transport->close();
  CHECK(endpoint.waitForCommand(kClose));
  endpoint.stop();
  CHECK(audioDatagrams == std::vector<size_t>({5, 3}));
  CHECK(videoDatagrams == std::vector<size_t>({5, 5, 2}));
}

void checkPacedUdpDelivery() {
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
  if (!endpoint.valid()) return;

  std::vector<uint8_t> pixels(payloadBytes);
  uint32_t random = 0x12345678;
  for (auto& value : pixels) {
    random = random * 1664525u + 1013904223u;
    value = uint8_t(random >> 24);
  }
  std::string error;
  GroovyTransport transport;
  CHECK(transport.open("localhost", true, 48000, error, endpoint.port()));
  Modeline mode{"pacing", 12.5, 640, 656, 700, 800, 245, 246, 247, 260,
                false};
  CHECK(transport.switchMode(mode, false, error));
  CHECK(transport.sendFrame(1, 0, pixels, error));
  const auto stats = transport.stats();
  transport.close();
  endpoint.stop();

  CHECK(commandBeforePayload && sizesValid && receivedPayload == payloadBytes);
  CHECK(datagrams == videoDatagramCount(payloadBytes, config));
  CHECK(finalSize == payloadBytes % config.payloadBytes);
  CHECK(stats.pacedVideoPayloads == 1 &&
        stats.pacedDatagrams == videoDatagramCount(payloadBytes, config));
  const auto elapsedUs =
      std::chrono::duration_cast<std::chrono::microseconds>(deliveryDuration)
          .count();
  const auto expectedUs = pacingDurationNs(payloadBytes, config) / 1000;
  CHECK(elapsedUs + 1000 >= int64_t(expectedUs));
}

void checkSendErrorsAreFatal() {
  FakeGroovyEndpoint endpoint([](auto& peer, const auto& packet) {
    if (!packet.empty() && packet[0] == kInit) peer.replyAck({});
  });
  if (!endpoint.valid()) return;
  std::string error;
  GroovyTransport transport;
  CHECK(transport.open("localhost", true, 48000, error, endpoint.port()));
  endpoint.stop();
  bool sendFailed = false;
  for (int attempt = 0; attempt < 10 && !sendFailed; ++attempt)
    sendFailed = !transport.switchMode(tinyMode(false), false, error);
  CHECK(sendFailed && !error.empty());
  CHECK(transport.stats().sendErrors > 0);
  transport.close();
}

void checkAutomaticSyncLine(bool interlaced, bool progressiveBuffer = false,
                            bool syncRefresh = true,
                            uint16_t frameDelay = 0) {
  FakeGroovyEndpoint endpoint([](auto& peer, const auto& packet) {
    if (!packet.empty() && packet[0] == kInit) peer.replyAck({});
  });
  if (!endpoint.valid()) return;
  std::string error;
  GroovyTransport transport;
  CHECK(transport.open("localhost", true, 48000, error, endpoint.port()));
  Modeline vga{"vga", 25.175, 640, 656, 752, 800, 480, 490, 492, 525,
               interlaced};
  CHECK(transport.switchMode(vga, progressiveBuffer, error));
  transport.setSyncOptions(syncRefresh, frameDelay);
  std::vector<uint8_t> pixels(
      size_t(640) * (interlaced && !progressiveBuffer ? 240 : 480) * 3, 42);
  uint16_t warmUpLine = 0, steadyLine = 0;
  for (uint32_t frame = 1; frame <= 12; ++frame) {
    CHECK(transport.sendFrame(frame, 0, pixels, error));
    transport.waitSync();
    if (frame == 5) warmUpLine = transport.stats().requestedSyncLine;
    if (frame == 12) steadyLine = transport.stats().requestedSyncLine;
  }
  transport.close();
  CHECK(transport.stats().acknowledgedFrames == 0);
  if (!syncRefresh) {
    CHECK(warmUpLine == 0 && steadyLine == 0);
  } else if (frameDelay) {
    const auto expected =
        uint16_t(std::llround(double(525) * frameDelay / 10.0) + 1);
    CHECK(warmUpLine == expected && steadyLine == expected);
  } else {
    CHECK(warmUpLine == 525 / 2);
    if (interlaced && !progressiveBuffer)
      CHECK(steadyLine > 0 && steadyLine <= 525 / 2);
    else
      CHECK(steadyLine != 525 / 2 && steadyLine > 0);
  }
}
}  // namespace

int main() {
  checkInterlaceTransport(false, 1, 1, 1, 6);
  checkInterlaceTransport(true, 1, 2, 0, 12);
  checkFieldAlignment();
  checkFieldAlignmentWraparound();
  checkFpgaHealthDiagnostics();
  checkAdaptiveDeliveryMargin();
  checkFatalPayloadFailureStopsProtocol();
  checkSharedAudioVideoPacketization();
  checkPacedUdpDelivery();
  checkSendErrorsAreFatal();
  checkAutomaticSyncLine(false);
  checkAutomaticSyncLine(true);
  checkAutomaticSyncLine(true, true);
  checkAutomaticSyncLine(true, false, true, 4);
  checkAutomaticSyncLine(true, false, false);
  return failed ? 1 : 0;
}
