#include <climits>
#include <cstdint>
#include <iostream>
#include <vector>

#include "fake_groovy_endpoint.hpp"
#include "mistercast/groovy_transport.hpp"

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
  CHECK(transport.open("localhost", 48000, error, endpoint.port()));
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
  CHECK(transport.open("localhost", 48000, error, endpoint.port()));
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
  CHECK(transport.open("localhost", 48000, error, endpoint.port()));
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
  CHECK(transport.open("localhost", 48000, error, endpoint.port()));
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
  CHECK(transport.open("localhost", 48000, error, endpoint.port()));
  status = transport.stats();
  CHECK(status.fpgaStatusSamples == 0 && status.fpgaFallbackSamples == 0 &&
        status.vramUnsyncedSamples == 0 && status.vramQueueEmptySamples == 0 &&
        !status.vramQueuePresent);
  transport.close();
}

void checkSendErrorsAreFatal() {
  FakeGroovyEndpoint endpoint([](auto& peer, const auto& packet) {
    if (!packet.empty() && packet[0] == kInit) peer.replyAck({});
  });
  if (!endpoint.valid()) return;
  std::string error;
  GroovyTransport transport;
  CHECK(transport.open("localhost", 48000, error, endpoint.port()));
  endpoint.stop();
  bool sendFailed = false;
  for (int attempt = 0; attempt < 10 && !sendFailed; ++attempt)
    sendFailed = !transport.switchMode(tinyMode(false), false, error);
  CHECK(sendFailed && !error.empty());
  CHECK(transport.stats().sendErrors > 0);
  transport.close();
}

void checkAutomaticSyncLine(bool interlaced) {
  FakeGroovyEndpoint endpoint([](auto& peer, const auto& packet) {
    if (!packet.empty() && packet[0] == kInit) peer.replyAck({});
  });
  if (!endpoint.valid()) return;
  std::string error;
  GroovyTransport transport;
  CHECK(transport.open("localhost", 48000, error, endpoint.port()));
  Modeline vga{"vga", 25.175, 640, 656, 752, 800, 480, 490, 492, 525,
               interlaced};
  CHECK(transport.switchMode(vga, false, error));
  transport.setSyncOptions(true, 0);
  std::vector<uint8_t> pixels(size_t(640) * (interlaced ? 240 : 480) * 3, 42);
  uint16_t warmUpLine = 0, steadyLine = 0;
  for (uint32_t frame = 1; frame <= 12; ++frame) {
    CHECK(transport.sendFrame(frame, 0, pixels, error));
    transport.waitSync();
    if (frame == 5) warmUpLine = transport.stats().requestedSyncLine;
    if (frame == 12) steadyLine = transport.stats().requestedSyncLine;
  }
  transport.close();
  CHECK(transport.stats().acknowledgedFrames == 0);
  CHECK(warmUpLine == 525 / 2);
  if (interlaced)
    CHECK(steadyLine > 0 && steadyLine <= 525 / 2);
  else
    CHECK(steadyLine != 525 / 2 && steadyLine > 0);
}
}  // namespace

int main() {
  checkInterlaceTransport(false, 1, 1, 1, 6);
  checkInterlaceTransport(true, 1, 2, 0, 12);
  checkFieldAlignment();
  checkFieldAlignmentWraparound();
  checkFpgaHealthDiagnostics();
  checkSendErrorsAreFatal();
  checkAutomaticSyncLine(false);
  checkAutomaticSyncLine(true);
  return failed ? 1 : 0;
}
