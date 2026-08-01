#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

#include "mistercast/audio_pacer.hpp"
#include "mistercast/audio_ring.hpp"
#include "mistercast/config.hpp"
#include "mistercast/groovy_transport.hpp"
#include "mistercast/interfaces.hpp"
#include "mistercast/stream_session.hpp"
#include "mistercast/transform.hpp"
using namespace mistercast;
static int failed = 0;
#define CHECK(x)                                                              \
  do {                                                                        \
    if (!(x)) {                                                               \
      std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #x "\n"; \
      ++failed;                                                               \
    }                                                                         \
  } while (0)

static Modeline testModeline(uint16_t width, uint16_t height,
                             bool interlaced = false) {
  return {"test", 1.0, width, uint16_t(width + 1), uint16_t(width + 2),
          uint16_t(width + 3), height, uint16_t(height + 1),
          uint16_t(height + 2), uint16_t(height + 3), interlaced};
}

static Frame grayFrame(uint32_t width, uint32_t height,
                       const std::vector<uint8_t>& values) {
  Frame frame;
  frame.width = width;
  frame.height = height;
  frame.stride = width * 4;
  frame.bgra.resize(size_t(frame.stride) * height);
  for (size_t i = 0; i < values.size(); ++i) {
    frame.bgra[i * 4] = values[i];
    frame.bgra[i * 4 + 1] = values[i];
    frame.bgra[i * 4 + 2] = values[i];
    frame.bgra[i * 4 + 3] = 255;
  }
  return frame;
}

static std::vector<uint8_t> blueValues(const std::vector<uint8_t>& rgb) {
  std::vector<uint8_t> result;
  for (size_t i = 0; i < rgb.size(); i += 3) result.push_back(rgb[i]);
  return result;
}

static void checkSamplingTransforms() {
  SourceOptions source;
  source.crop = CropMode::Custom;
  std::vector<uint8_t> rgb;
  std::string error;

  SamplingMode parsed = SamplingMode::Point;
  CHECK(parseSamplingMode("point", parsed) && parsed == SamplingMode::Point);
  CHECK(parseSamplingMode("bilinear", parsed) &&
        parsed == SamplingMode::Bilinear);
  CHECK(parseSamplingMode("line-blend", parsed) &&
        parsed == SamplingMode::LineBlend);
  CHECK(!parseSamplingMode("area", parsed));
  CHECK(toString(SamplingMode::LineBlend) == "line-blend");

  source.sampling = SamplingMode::Bilinear;
  auto frame = grayFrame(2, 2, {0, 64, 128, 255});
  CHECK(transformRgb24(frame, {0, 0, 2, 2}, source, testModeline(1, 1), 0,
                       rgb, error));
  CHECK(blueValues(rgb) == std::vector<uint8_t>({112}));

  frame = grayFrame(2, 1, {10, 110});
  CHECK(transformRgb24(frame, {0, 0, 2, 1}, source, testModeline(4, 1), 0,
                       rgb, error));
  CHECK(blueValues(rgb) == std::vector<uint8_t>({10, 35, 85, 110}));

  frame = grayFrame(18, 9, std::vector<uint8_t>(162));
  for (uint32_t y = 0; y < 9; ++y)
    for (uint32_t x = 0; x < 18; ++x)
      for (unsigned channel = 0; channel < 3; ++channel)
        frame.bgra[(size_t(y) * 18 + x) * 4 + channel] = uint8_t(y * 18 + x);
  CHECK(transformRgb24(frame, {0, 0, 18, 9}, source, testModeline(2, 1), 0,
                       rgb, error));
  CHECK(blueValues(rgb) == std::vector<uint8_t>({76, 85}));

  source.sampling = SamplingMode::LineBlend;
  frame = grayFrame(1, 4, {10, 30, 50, 70});
  CHECK(transformRgb24(frame, {0, 0, 1, 4}, source, testModeline(1, 2), 0,
                       rgb, error));
  CHECK(blueValues(rgb) == std::vector<uint8_t>({20, 60}));
  frame = grayFrame(1, 5, {0, 10, 20, 30, 40});
  CHECK(transformRgb24(frame, {0, 0, 1, 5}, source, testModeline(1, 2), 0,
                       rgb, error));
  CHECK(blueValues(rgb) == std::vector<uint8_t>({8, 32}));
  std::vector<uint8_t> hdLines(1080);
  for (size_t y = 0; y < hdLines.size(); ++y) hdLines[y] = uint8_t(y % 251);
  frame = grayFrame(1, 1080, hdLines);
  CHECK(transformRgb24(frame, {0, 0, 1, 1080}, source,
                       testModeline(1, 240), 0, rgb, error));
  const auto reducedLines = blueValues(rgb);
  CHECK(reducedLines.size() == 240 && reducedLines[0] == 2 &&
        reducedLines[1] == 6);
  frame = grayFrame(1, 2, {20, 80});
  CHECK(transformRgb24(frame, {0, 0, 1, 2}, source, testModeline(1, 3), 0,
                       rgb, error));
  CHECK(blueValues(rgb) == std::vector<uint8_t>({20, 50, 80}));
  frame = grayFrame(3, 7, std::vector<uint8_t>(21, 123));
  CHECK(transformRgb24(frame, {0, 0, 3, 7}, source, testModeline(2, 4), 0,
                       rgb, error));
  CHECK(std::all_of(rgb.begin(), rgb.end(), [](uint8_t x) { return x == 123; }));

  frame = grayFrame(2, 3, {0, 1, 2, 3, 4, 5});
  struct RotationCase {
    Rotation rotation;
    uint16_t width, height;
    std::vector<uint8_t> expected;
  };
  const RotationCase rotations[] = {
      {Rotation::None, 2, 3, {0, 1, 2, 3, 4, 5}},
      {Rotation::Flip180, 2, 3, {5, 4, 3, 2, 1, 0}},
      {Rotation::CW90, 3, 2, {4, 2, 0, 5, 3, 1}},
      {Rotation::CCW90, 3, 2, {1, 3, 5, 0, 2, 4}},
  };
  for (const auto mode : {SamplingMode::Point, SamplingMode::Bilinear,
                          SamplingMode::LineBlend}) {
    source.sampling = mode;
    for (const auto& rotation : rotations) {
      source.rotation = rotation.rotation;
      CHECK(transformRgb24(frame, {0, 0, 2, 3}, source,
                           testModeline(rotation.width, rotation.height), 0,
                           rgb, error));
      CHECK(blueValues(rgb) == rotation.expected);
    }
  }

  source.rotation = Rotation::None;
  frame = grayFrame(2, 4, {0, 1, 2, 3, 4, 5, 6, 7});
  for (const auto mode : {SamplingMode::Point, SamplingMode::Bilinear,
                          SamplingMode::LineBlend}) {
    source.sampling = mode;
    auto interlaced = testModeline(2, 4, true);
    source.progressiveInterlaceBuffer = false;
    CHECK(transformRgb24(frame, {0, 0, 2, 4}, source, interlaced, 0, rgb,
                         error));
    CHECK(blueValues(rgb) == std::vector<uint8_t>({2, 3, 6, 7}));
    CHECK(transformRgb24(frame, {0, 0, 2, 4}, source, interlaced, 1, rgb,
                         error));
    CHECK(blueValues(rgb) == std::vector<uint8_t>({0, 1, 4, 5}));
    source.progressiveInterlaceBuffer = true;
    CHECK(transformRgb24(frame, {0, 0, 2, 4}, source, interlaced, 1, rgb,
                         error));
    CHECK(blueValues(rgb) == std::vector<uint8_t>({0, 1, 2, 3, 4, 5, 6, 7}));
    Frame malformed = frame;
    malformed.bgra.pop_back();
    CHECK(!transformRgb24(malformed, {0, 0, 2, 4}, source, interlaced, 0,
                          rgb, error));
    CHECK(!transformRgb24(frame, {1, 0, 2, 4}, source, interlaced, 0, rgb,
                          error));
  }
}
static void checkInterlaceTransport(bool progressive, uint8_t sentField,
                                    uint8_t expectedInterlace,
                                    uint8_t expectedField, size_t pixelValues) {
  int server = socket(AF_INET, SOCK_DGRAM, 0);
  if (server < 0) return;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) !=
      0) {
    close(server);
    return;
  }
  socklen_t alen = sizeof(address);
  getsockname(server, reinterpret_cast<sockaddr*>(&address), &alen);
  timeval timeout{2, 0};
  setsockopt(server, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  std::atomic<bool> sawMode = false, sawFrame = false, sawAudio = false,
                    sawClose = false;
  std::atomic<uint8_t> receivedInterlace = 0xff, receivedField = 0xff;
  std::atomic<uint16_t> receivedSyncLine = 0;
  std::thread endpoint([&] {
    uint8_t packet[2048];
    sockaddr_storage peer{};
    socklen_t plen = sizeof(peer);
    for (int i = 0; i < 12 && !sawClose; ++i) {
      auto n = recvfrom(server, packet, sizeof(packet), 0,
                        reinterpret_cast<sockaddr*>(&peer), &plen);
      if (n <= 0) break;
      if (packet[0] == 2) {
        uint8_t ack[13]{};
        sendto(server, ack, sizeof(ack), 0, reinterpret_cast<sockaddr*>(&peer),
               plen);
      } else if (packet[0] == 3) {
        sawMode = true;
        receivedInterlace = packet[25];
      } else if (packet[0] == 7) {
        sawFrame = true;
        receivedField = packet[5];
        uint32_t frame;
        uint16_t line;
        std::memcpy(&frame, packet + 1, 4);
        std::memcpy(&line, packet + 6, 2);
        receivedSyncLine = line;
        uint8_t ack[13]{};
        std::memcpy(ack, &frame, 4);
        std::memcpy(ack + 4, &line, 2);
        uint32_t fpgaFrame = frame + 1;
        uint16_t fpgaLine = 1;
        std::memcpy(ack + 6, &fpgaFrame, 4);
        std::memcpy(ack + 10, &fpgaLine, 2);
        ack[12] = 0x44;
        sendto(server, ack, sizeof(ack), 0, reinterpret_cast<sockaddr*>(&peer),
               plen);
      } else if (packet[0] == 4)
        sawAudio = true;
      else if (packet[0] == 1)
        sawClose = true;
    }
  });
  std::string error;
  {
    GroovyTransport transport;
    CHECK(transport.open("localhost", 48000, error, ntohs(address.sin_port)));
    Modeline tiny{"tiny", 1, 2, 3, 4, 5, 2, 3, 4, 5, true};
    CHECK(transport.switchMode(tiny, progressive, error));
    transport.setSyncOptions(true, 0);
    std::vector<uint8_t> pixels(pixelValues, 42);
    CHECK(transport.sendFrame(1, sentField, pixels, error));
    transport.waitSync();
    auto status = transport.stats();
    CHECK(status.acknowledgedFrame == 1 && status.acknowledgedFrames == 1 &&
          status.rasterCorrectionUs < 0 && status.vramSynced &&
          transport.misterAudioEnabled());
    int16_t sound[4]{};
    CHECK(transport.sendAudio(sound, 4, error));
    transport.close();
  }
  endpoint.join();
  CHECK(sawMode && sawFrame && sawAudio && sawClose);
  CHECK(receivedInterlace == expectedInterlace &&
        receivedField == expectedField && receivedSyncLine == 2);
  close(server);
}

static void checkFieldAlignment() {
  int server = socket(AF_INET, SOCK_DGRAM, 0);
  if (server < 0) return;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) !=
      0) {
    close(server);
    return;
  }
  socklen_t alen = sizeof(address);
  getsockname(server, reinterpret_cast<sockaddr*>(&address), &alen);
  timeval timeout{2, 0};
  setsockopt(server, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  std::atomic<bool> done{false};
  std::atomic<unsigned> blits{0};
  std::thread endpoint([&] {
    uint8_t packet[2048];
    sockaddr_storage peer{};
    socklen_t plen = sizeof(peer);
    while (!done) {
      auto n = recvfrom(server, packet, sizeof(packet), 0,
                        reinterpret_cast<sockaddr*>(&peer), &plen);
      if (n <= 0) break;
      if (packet[0] == 2) {
        const uint8_t version = 1;
        sendto(server, &version, sizeof(version), 0,
               reinterpret_cast<sockaddr*>(&peer), plen);
      } else if (packet[0] == 7) {
        uint32_t frame;
        uint16_t line;
        std::memcpy(&frame, packet + 1, 4);
        std::memcpy(&line, packet + 6, 2);
        const unsigned index = blits++;
        const uint32_t fpgaFrame = index == 0 ? 42 : 43;
        uint8_t ack[13]{};
        std::memcpy(ack, &frame, 4);
        std::memcpy(ack + 4, &line, 2);
        std::memcpy(ack + 6, &fpgaFrame, 4);
        const uint16_t fpgaLine = 1;
        std::memcpy(ack + 10, &fpgaLine, 2);
        ack[12] = index == 0 ? 0 : 0x20;
        sendto(server, ack, sizeof(ack), 0,
               reinterpret_cast<sockaddr*>(&peer), plen);
      }
    }
  });

  std::string error;
  GroovyTransport transport;
  CHECK(transport.open("localhost", 48000, error, ntohs(address.sin_port)));
  Modeline tiny{"tiny", 1, 2, 3, 4, 5, 2, 3, 4, 5, true};
  CHECK(transport.switchMode(tiny, false, error));
  transport.setSyncOptions(true, 0);
  std::vector<uint8_t> pixels(6, 42);

  // A mode switch starts in deterministic local phase and does not consume an
  // old ACK. The caller's stale field value is deliberately overwritten.
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

  // The FPGA is ahead, so rebase to its next frame. Its returned F1 changes
  // the fallback phase once, which is reported rather than silently hidden.
  frame = 41;
  transport.alignFrame(frame, field);
  status = transport.stats();
  CHECK(frame == 43 && field == 0 && status.fieldRealignments == 1);
  CHECK(transport.sendFrame(frame, field, pixels, error));
  transport.waitSync();

  // Both FPGA field values and missed-ACK extrapolation retain parity.
  frame = 44;
  transport.alignFrame(frame, field);
  CHECK(field == 1 && transport.stats().fpgaField == 1);
  frame = 45;
  transport.alignFrame(frame, field);
  CHECK(field == 0 && transport.stats().fieldRealignments == 1);

  // A second mode switch must invalidate the phase established above.
  CHECK(transport.switchMode(tiny, false, error));
  frame = 46;
  field = 1;
  transport.alignFrame(frame, field);
  status = transport.stats();
  CHECK(field == 0 && !status.fieldPhaseValid &&
        status.fieldRealignments == 1);
  transport.close();
  done = true;
  endpoint.join();
  close(server);
}

static void checkFieldAlignmentWraparound() {
  int server = socket(AF_INET, SOCK_DGRAM, 0);
  if (server < 0) return;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) !=
      0) {
    close(server);
    return;
  }
  socklen_t alen = sizeof(address);
  getsockname(server, reinterpret_cast<sockaddr*>(&address), &alen);
  timeval timeout{2, 0};
  setsockopt(server, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  std::thread endpoint([&] {
    uint8_t packet[2048];
    sockaddr_storage peer{};
    socklen_t plen = sizeof(peer);
    for (;;) {
      auto n = recvfrom(server, packet, sizeof(packet), 0,
                        reinterpret_cast<sockaddr*>(&peer), &plen);
      if (n <= 0 || packet[0] == 1) break;
      if (packet[0] == 2) {
        const uint8_t version = 1;
        sendto(server, &version, sizeof(version), 0,
               reinterpret_cast<sockaddr*>(&peer), plen);
      } else if (packet[0] == 7) {
        uint32_t frame;
        uint16_t line;
        std::memcpy(&frame, packet + 1, 4);
        std::memcpy(&line, packet + 6, 2);
        uint8_t ack[13]{};
        std::memcpy(ack, &frame, 4);
        std::memcpy(ack + 4, &line, 2);
        const uint32_t fpgaFrame = UINT32_MAX;
        std::memcpy(ack + 6, &fpgaFrame, 4);
        const uint16_t fpgaLine = 1;
        std::memcpy(ack + 10, &fpgaLine, 2);
        sendto(server, ack, sizeof(ack), 0,
               reinterpret_cast<sockaddr*>(&peer), plen);
      }
    }
  });

  std::string error;
  GroovyTransport transport;
  CHECK(transport.open("localhost", 48000, error, ntohs(address.sin_port)));
  Modeline tiny{"tiny", 1, 2, 3, 4, 5, 2, 3, 4, 5, true};
  CHECK(transport.switchMode(tiny, false, error));
  transport.setSyncOptions(true, 0);
  std::vector<uint8_t> pixels(6, 42);
  uint32_t frame = UINT32_MAX;
  uint8_t field = 1;
  transport.alignFrame(frame, field);
  CHECK(transport.sendFrame(frame, field, pixels, error));
  transport.waitSync();
  frame = 0;
  transport.alignFrame(frame, field);
  CHECK(frame == 0 && field == 0 && transport.stats().fieldPhaseValid);
  transport.close();
  endpoint.join();
  close(server);
}
// Once a connected UDP socket receives an ICMP port-unreachable error, the
// next send must fail rather than silently omit part of a framed payload.
static void checkSendErrorsAreFatal() {
  int server = socket(AF_INET, SOCK_DGRAM, 0);
  if (server < 0) return;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) !=
      0) {
    close(server);
    return;
  }
  socklen_t alen = sizeof(address);
  getsockname(server, reinterpret_cast<sockaddr*>(&address), &alen);
  std::thread endpoint([&] {
    uint8_t packet[2048];
    sockaddr_storage peer{};
    socklen_t plen = sizeof(peer);
    auto n = recvfrom(server, packet, sizeof(packet), 0,
                      reinterpret_cast<sockaddr*>(&peer), &plen);
    if (n > 0 && packet[0] == 2) {
      uint8_t ack[13]{};
      sendto(server, ack, sizeof(ack), 0, reinterpret_cast<sockaddr*>(&peer),
             plen);
    }
  });
  std::string error;
  GroovyTransport transport;
  CHECK(transport.open("localhost", 48000, error, ntohs(address.sin_port)));
  endpoint.join();
  close(server);  // nothing is listening now, so sends get ECONNREFUSED
  Modeline tiny{"tiny", 1, 2, 3, 4, 5, 2, 3, 4, 5, false};
  bool sendFailed = false;
  // A successful send to the closed port triggers ICMP; a following send
  // observes it. Allow several iterations for delivery on slower test hosts.
  for (int attempt = 0; attempt < 10 && !sendFailed; ++attempt)
    sendFailed = !transport.switchMode(tiny, false, error);
  CHECK(sendFailed && !error.empty());
  CHECK(transport.stats().sendErrors > 0);
  transport.close();
}
// With no blit ACKs at all the warm-up gate must still open for progressive
// output. Alternating interlaced field buffers deliberately retain vTotal/2 as
// their latest safe line even after warm-up.
static void checkAutomaticSyncLine(bool interlaced) {
  int server = socket(AF_INET, SOCK_DGRAM, 0);
  if (server < 0) return;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) !=
      0) {
    close(server);
    return;
  }
  socklen_t alen = sizeof(address);
  getsockname(server, reinterpret_cast<sockaddr*>(&address), &alen);
  std::atomic<bool> done{false};
  std::thread endpoint([&] {
    uint8_t packet[2048];
    sockaddr_storage peer{};
    socklen_t plen = sizeof(peer);
    timeval timeout{0, 200000};
    setsockopt(server, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    while (!done) {
      auto n = recvfrom(server, packet, sizeof(packet), 0,
                        reinterpret_cast<sockaddr*>(&peer), &plen);
      if (n <= 0) continue;
      if (packet[0] != 2) continue;  // deliberately never acks a blit
      uint8_t ack[13]{};
      sendto(server, ack, sizeof(ack), 0, reinterpret_cast<sockaddr*>(&peer),
             plen);
    }
  });
  std::string error;
  GroovyTransport transport;
  CHECK(transport.open("localhost", 48000, error, ntohs(address.sin_port)));
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
  done = true;
  endpoint.join();
  close(server);
  CHECK(transport.stats().acknowledgedFrames == 0);
  CHECK(warmUpLine == 525 / 2);
  if (interlaced)
    CHECK(steadyLine > 0 && steadyLine <= 525 / 2);
  else
    CHECK(steadyLine != 525 / 2 && steadyLine > 0);
}
namespace {
// Synthetic capture sources, so session behaviour that depends on the monitor
// changing or on the core's audio bit can be driven deterministically.
class FakeVideo final : public IVideoCapture {
 public:
  std::atomic<uint16_t> width{1920}, height{1080};
  std::atomic<uint32_t> captured{0};
  std::atomic<bool> produce{true};
  mutable std::mutex mutex;
  std::vector<CropRect> regions;
  CropRect region{};
  Monitor geometry() const {
    Monitor m;
    m.name = "fake";
    m.width = width;
    m.height = height;
    m.primary = true;
    return m;
  }
  std::vector<Monitor> monitors(std::string&) override { return {geometry()}; }
  bool start(const std::string&, ErrorCallback) override { return true; }
  Monitor selected() const override { return geometry(); }
  void setRegion(const CropRect& r) override {
    std::lock_guard<std::mutex> l(mutex);
    region = r;
    regions.push_back(r);
  }
  bool next(Frame& out, std::chrono::milliseconds) override {
    if (!produce) return false;
    CropRect r;
    {
      std::lock_guard<std::mutex> l(mutex);
      r = region;
    }
    if (!r.width || !r.height) return false;
    out.width = r.width;
    out.height = r.height;
    out.stride = r.width * 4;
    out.bgra.assign(size_t(out.stride) * out.height, 128);
    ++captured;
    return true;
  }
  void stop() noexcept override {}
};
class FakeAudio final : public IAudioCapture {
 public:
  std::atomic<bool> produce{false};
  bool start(const std::string&, ErrorCallback) override { return true; }
  bool next(PcmBlock& b, std::chrono::milliseconds) override {
    if (!produce) return false;
    b.sampleRate = 48000;
    b.samples.assign(960, 1000);  // 10 ms of stereo tone
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return true;
  }
  void stop() noexcept override {}
  uint32_t sampleRate() const noexcept override { return 48000; }
};
// Minimal Groovy endpoint: acks CMD_INIT and every blit, counting audio
// packets.
class FakeMister {
 public:
  std::atomic<bool> running{true};
  std::atomic<uint32_t> audioPackets{0}, blits{0};
  uint8_t statusBits;
  int fd{-1};
  uint16_t port{};
  // StreamSession always dials the protocol port, so the fake must own it. If
  // it is taken the session tests skip rather than fail.
  explicit FakeMister(uint8_t bits) : statusBits(bits) {
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(32100);
    if (fd < 0 ||
        bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
      if (fd >= 0) close(fd);
      fd = -1;
      return;
    }
    port = 32100;
    timeval timeout{0, 200000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    worker = std::thread([this] { serve(); });
  }
  void serve() {
    uint8_t packet[2048];
    sockaddr_storage peer{};
    socklen_t plen = sizeof(peer);
    while (running) {
      auto n = recvfrom(fd, packet, sizeof(packet), 0,
                        reinterpret_cast<sockaddr*>(&peer), &plen);
      if (n <= 0) continue;
      if (packet[0] == 2) {
        uint8_t ack[13]{};
        ack[12] = statusBits;
        sendto(fd, ack, sizeof(ack), 0, reinterpret_cast<sockaddr*>(&peer),
               plen);
      } else if (packet[0] == 4)
        ++audioPackets;
      else if (packet[0] == 7) {
        ++blits;
        uint32_t frame;
        uint16_t line;
        std::memcpy(&frame, packet + 1, 4);
        std::memcpy(&line, packet + 6, 2);
        uint8_t ack[13]{};
        std::memcpy(ack, &frame, 4);
        std::memcpy(ack + 4, &line, 2);
        std::memcpy(ack + 6, &frame, 4);
        std::memcpy(ack + 10, &line, 2);
        ack[12] = statusBits;
        sendto(fd, ack, sizeof(ack), 0, reinterpret_cast<sockaddr*>(&peer),
               plen);
      }
    }
  }
  ~FakeMister() {
    running = false;
    if (worker.joinable()) worker.join();
    if (fd >= 0) close(fd);
  }
  std::thread worker;
};
// Sanitizer builds run the pacing loop far slower than release, so session
// tests wait on a condition with a generous deadline rather than a fixed frame
// count.
template <class Predicate>
bool waitFor(Predicate ready, int milliseconds = 15000) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(milliseconds);
  while (std::chrono::steady_clock::now() < deadline) {
    if (ready()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}
AppConfig sessionConfig() {
  AppConfig c;
  c.target = "127.0.0.1";
  c.source.audio = false;
  c.source.preview = false;
  c.source.crop = CropMode::Full43;
  c.modeline = Modeline::safeDefault();
  return c;
}
}  // namespace

static void checkTransformStats() {
  FakeMister mister(0x44);
  if (mister.port == 0) return;
  auto video = std::make_unique<FakeVideo>();
  auto* raw = video.get();
  raw->produce = false;
  StreamSession session(std::move(video), std::make_unique<FakeAudio>());
  auto config = sessionConfig();
  config.source.sampling = SamplingMode::LineBlend;
  std::string error;
  CHECK(session.start(config, {}, &error));
  auto stats = session.stats();
  CHECK(stats.transformTimeUs == 0 && stats.transformMaxUs == 0);
  raw->produce = true;
  CHECK(waitFor([&] { return session.stats().sentFrames >= 3; }));
  stats = session.stats();
  CHECK(stats.transformMaxUs >= stats.transformTimeUs);
  session.stop();
  raw->produce = false;
  CHECK(session.start(config, {}, &error));
  stats = session.stats();
  CHECK(stats.transformTimeUs == 0 && stats.transformMaxUs == 0);
  session.stop();
}

// A monitor resized mid-stream must have its crop recomputed, not keep
// streaming a rectangle sized for the old geometry.
static void checkCropFollowsMonitorResize() {
  FakeMister mister(0x44);
  if (mister.port == 0) return;
  auto video = std::make_unique<FakeVideo>();
  auto* raw = video.get();
  auto session = std::make_unique<StreamSession>(std::move(video),
                                                 std::make_unique<FakeAudio>());
  auto config = sessionConfig();
  std::string error;
  // 4:3 of a 1080-tall monitor is 1440x1080.
  if (!session->start(config, {}, &error)) {
    std::cerr << "session start failed: " << error << "\n";
    ++failed;
    return;
  }
  CHECK(waitFor([&] { return raw->captured >= 3; }));
  raw->height = 720;  // 4:3 of 720 is 960x720
  const auto before = raw->captured.load();
  CHECK(waitFor([&] { return raw->captured >= before + 3; }));
  session->stop();
  std::lock_guard<std::mutex> l(raw->mutex);
  CHECK(raw->regions.size() >= 2);
  CHECK(raw->regions.front().width == 1440 &&
        raw->regions.front().height == 1080);
  CHECK(raw->regions.back().width == 960 && raw->regions.back().height == 720);
}

// Timings must be switchable on a live stream, and the crop must follow the new
// active area since 1x-5x crops are relative to it.
static void checkLiveModelineSwitch() {
  FakeMister mister(0x44);
  if (mister.port == 0) return;
  auto video = std::make_unique<FakeVideo>();
  auto* raw = video.get();
  auto session = std::make_unique<StreamSession>(std::move(video),
                                                 std::make_unique<FakeAudio>());
  auto config = sessionConfig();
  config.source.crop = CropMode::X1;
  config.modeline = Modeline::safeDefault();  // 320x240 active
  std::string error;
  if (!session->start(config, {}, &error)) {
    std::cerr << "session start failed: " << error << "\n";
    ++failed;
    return;
  }
  CHECK(waitFor([&] { return raw->captured >= 3; }));
  Modeline vga{"vga", 25.175, 640, 656, 752, 800, 480, 490, 492, 525, false};
  CHECK(session->updateModeline(vga, false, &error));
  CHECK(waitFor([&] {
    std::lock_guard<std::mutex> l(raw->mutex);
    return raw->regions.back().width == 640;
  }));
  const bool alive = session->state() == SessionState::Streaming;
  const auto blits = mister.blits.load();
  session->stop();
  CHECK(alive);
  CHECK(blits > 0);
  std::lock_guard<std::mutex> l(raw->mutex);
  CHECK(raw->regions.front().width == 320 &&
        raw->regions.front().height == 240);
  CHECK(raw->regions.back().width == 640 && raw->regions.back().height == 480);
}

// The core reporting audio off must stop audio being sent at all.
static void checkAudioSkippedWhenCoreHasAudioOff() {
  for (bool coreAudio : {false, true}) {
    FakeMister mister(coreAudio ? 0x44 : 0x04);
    if (mister.port == 0) return;
    auto video = std::make_unique<FakeVideo>();
    auto audio = std::make_unique<FakeAudio>();
    auto* rawVideo = video.get();
    audio->produce = true;
    auto session =
        std::make_unique<StreamSession>(std::move(video), std::move(audio));
    auto config = sessionConfig();
    config.source.audio = true;
    std::string error;
    if (!session->start(config, {}, &error)) {
      std::cerr << "session start failed: " << error << "\n";
      ++failed;
      return;
    }
    // Enough frames that audio would certainly have been sent if it were going
    // to be; with the core reporting audio off, none must appear.
    CHECK(waitFor([&] { return rawVideo->captured >= 30; }));
    if (coreAudio) CHECK(waitFor([&] { return mister.audioPackets > 0; }));
    session->stop();
    if (coreAudio)
      CHECK(mister.audioPackets > 0);
    else
      CHECK(mister.audioPackets == 0);
  }
}

int main() {
  checkSamplingTransforms();
  auto safe = Modeline::safeDefault();
  CHECK(!safe.validate());
  CHECK(safe.refreshHz() > 59 && safe.refreshHz() < 61);
  auto bad = safe;
  bad.hTotal = 300;
  CHECK(bool(bad.validate()));
  Modeline parsed;
  std::string error;
  CHECK(parseModeline("6.7 320 336 367 426 240 244 247 262 0", parsed, error));
  CHECK(!parseModeline("nonsense", parsed, error));
  AudioRing ring(4);
  int16_t a[] = {1, 2, 3};
  CHECK(ring.push(a, 3) == 0);
  int16_t o[4];
  CHECK(ring.pop(o, 4) == 3);
  CHECK(o[0] == 1 && o[2] == 3 && o[3] == 0);
  int16_t b[] = {1, 2, 3, 4, 5};
  CHECK(ring.push(b, 5) == 1);
  CHECK(ring.pop(o, 4) == 4 && o[0] == 2 && o[3] == 5);
  int16_t c[] = {7, 8, 9, 10};
  CHECK(ring.push(c, 4) == 0);
  CHECK(ring.discard(2) == 2 && ring.size() == 2);
  CHECK(ring.pop(o, 2) == 2 && o[0] == 9 && o[1] == 10);
  CHECK(ring.discard(5) == 0 && ring.size() == 0);
  CHECK(ring.push(c, 4) == 0 && ring.discard(9) == 4 && ring.size() == 0);
  CHECK(ring.push(c, 2) == 0 && ring.pop(o, 2) == 2 && o[0] == 7);
  AudioPacer pacer(48000);
  uint64_t pacedValues = 0, totalNs = 0;
  for (int i = 0; i < 1000; ++i) {
    uint64_t elapsed = 16682885 + (i % 3);
    totalNs += elapsed;
    auto due = pacer.valuesDue(elapsed, 32000);
    CHECK((due & 1) == 0);
    pacedValues += due;
  }
  CHECK(pacedValues == (totalNs * 48000 / 1000000000) * 2);
  pacer.reset(48000);
  CHECK(pacer.valuesDue(1000000000, 32000) == 32000);
  CHECK(pacer.valuesDue(0, 100000) == 64000);
  CHECK(pacer.sourceValuesFor(8, 2500, 1600, 800) == 10);
  CHECK(pacer.sourceValuesFor(8, 700, 1600, 800) == 6);
  CHECK(pacer.sourceValuesFor(8, 1600, 1600, 800) == 8);
  int16_t servoSource[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  std::vector<int16_t> servoOutput;
  AudioPacer::conformStereo(servoSource, 10, servoOutput, 8);
  CHECK(servoOutput.size() == 8 && servoOutput[0] == 0 && servoOutput[4] == 6 &&
        servoOutput[7] == 9);
  AudioPacer::conformStereo(servoSource, 6, servoOutput, 8);
  CHECK(servoOutput.size() == 8 && servoOutput[0] == 0 && servoOutput[4] == 4 &&
        servoOutput[6] == 4);
  SourceOptions crop;
  Modeline cropMode = Modeline::safeDefault();  // 320x240 active
  CropRect rect;
  crop.crop = CropMode::X1;
  crop.width = crop.height = 64;  // must be ignored for 1x-5x
  CHECK(calculateCrop(3840, 2160, crop, cropMode, rect, error));
  CHECK(rect.width == 320 && rect.height == 240);
  crop.crop = CropMode::X3;
  CHECK(calculateCrop(3840, 2160, crop, cropMode, rect, error));
  CHECK(rect.width == 960 && rect.height == 720);
  crop.crop = CropMode::Full43;
  CHECK(calculateCrop(3840, 2160, crop, cropMode, rect, error));
  CHECK(rect.width == 2880 && rect.height == 2160);
  crop.rotation = Rotation::CW90;
  CHECK(calculateCrop(3840, 2160, crop, cropMode, rect, error));
  CHECK(rect.width == 1620 && rect.height == 2160);
  crop.rotation = Rotation::CCW90;
  crop.crop = CropMode::Full54;
  CHECK(calculateCrop(3840, 2160, crop, cropMode, rect, error));
  CHECK(rect.width == 1728 && rect.height == 2160);
  crop.rotation = Rotation::None;
  crop.crop = CropMode::Custom;
  crop.width = 640;
  crop.height = 480;
  crop.alignment = Alignment::TopLeft;
  crop.xOffset = 4000;
  crop.yOffset = 2000;
  CHECK(calculateCrop(3840, 2160, crop, cropMode, rect, error));
  CHECK(rect.x == 3200 && rect.y == 1680);
  crop.xOffset = -3000;
  crop.yOffset = -2000;
  CHECK(calculateCrop(3840, 2160, crop, cropMode, rect, error));
  CHECK(rect.x == 0 && rect.y == 0);
  Frame f;
  f.width = 4;
  f.height = 2;
  f.stride = 16;
  f.bgra.resize(32);
  for (size_t i = 0; i < 8; ++i) {
    f.bgra[i * 4] = uint8_t(i);
    f.bgra[i * 4 + 1] = 10;
    f.bgra[i * 4 + 2] = 20;
    f.bgra[i * 4 + 3] = 255;
  }
  SourceOptions so;
  so.crop = CropMode::Custom;
  so.width = 4;
  so.height = 2;
  Modeline m{"test", 1, 2, 3, 4, 5, 2, 3, 4, 5, false};
  std::vector<uint8_t> rgb;
  CHECK(transformRgb24(f, so, m, 0, rgb, error));
  CHECK(rgb.size() == 12 && rgb[0] == 1 && rgb[9] == 7);
  m.interlaced = true;
  CHECK(transformRgb24(f, so, m, 0, rgb, error));
  CHECK(rgb.size() == 6 && rgb[0] == 5);
  CHECK(transformRgb24(f, so, m, 1, rgb, error));
  CHECK(rgb.size() == 6 && rgb[0] == 1);
  so.progressiveInterlaceBuffer = true;
  CHECK(transformRgb24(f, so, m, 1, rgb, error));
  CHECK(rgb.size() == 12 && rgb[0] == 1 && rgb[9] == 7);
  uint8_t px[] = {0x00, 0x00, 0xff, 0x00};
  Frame nf;
  CHECK(normalizeToBgra(px, 4, 1, 1, 4, 32, 0xff0000, 0xff00, 0xff, true, nf,
                        error));
  CHECK(nf.bgra[0] == 0 && nf.bgra[1] == 0 && nf.bgra[2] == 255);
  checkInterlaceTransport(false, 1, 1, 1, 6);
  checkInterlaceTransport(true, 1, 2, 0, 12);
  checkFieldAlignment();
  checkFieldAlignmentWraparound();
  checkSendErrorsAreFatal();
  checkAutomaticSyncLine(false);
  checkAutomaticSyncLine(true);
  checkCropFollowsMonitorResize();
  checkLiveModelineSwitch();
  checkAudioSkippedWhenCoreHasAudioOff();
  checkTransformStats();
  auto dir = std::filesystem::temp_directory_path() / "mistercast-core-test";
  std::filesystem::create_directories(dir);
  auto path = dir / "config.json";
  AppConfig cfg;
  cfg.target = "mister.local";
  cfg.source.progressiveInterlaceBuffer = true;
  cfg.source.sampling = SamplingMode::LineBlend;
  auto custom = Modeline::safeDefault();
  custom.name = "My preset";
  cfg.customModelines.push_back(custom);
  CHECK(saveConfig(cfg, path, error));
  std::string warning;
  auto loaded = loadConfig(path, &warning);
  CHECK(loaded.target == cfg.target &&
        loaded.source.progressiveInterlaceBuffer &&
        loaded.source.sampling == SamplingMode::LineBlend && warning.empty());
  CHECK(loaded.customModelines.size() == 1 &&
        loaded.customModelines[0].name == "My preset");
  {
    std::ofstream f2(path);
    f2 << "broken";
  }
  loaded = loadConfig(path, &warning);
  CHECK(loaded.target.empty() && !warning.empty());
  // Top-level keys must not be shadowed by same-named keys inside
  // customModelines, whatever order the keys appear in.
  {
    std::ofstream reordered(path);
    reordered << R"({
  "version": 1,
  "customModelines": [
    {"name":"decoy","pixelClockMHz":9.9,"hActive":111,"hBegin":112,
     "hEnd":113,"hTotal":140,"vActive":222,"vBegin":223,"vEnd":224,
     "vTotal":240,"interlaced":true}
  ],
  "target": "real.local",
  "pixelClockMHz": 25.175,
  "hActive": 640, "hBegin": 656, "hEnd": 752, "hTotal": 800,
  "vActive": 480, "vBegin": 490, "vEnd": 492, "vTotal": 525,
  "interlaced": false
})";
  }
  loaded = loadConfig(path, &warning);
  CHECK(loaded.target == "real.local" && warning.empty());
  CHECK(loaded.modeline.hActive == 640 && loaded.modeline.vTotal == 525);
  CHECK(!loaded.modeline.interlaced);
  CHECK(loaded.customModelines.size() == 1 &&
        loaded.customModelines[0].hActive == 111);
  CHECK(loaded.source.sampling == SamplingMode::Point);
  {
    std::ofstream invalidSampling(path);
    invalidSampling << R"({"version":1,"sampling":"area"})";
  }
  loaded = loadConfig(path, &warning);
  CHECK(loaded.source.sampling == SamplingMode::Point && !warning.empty());
  std::filesystem::remove_all(dir);
  auto audioConfigPath = std::filesystem::temp_directory_path() /
                         "mistercast-audio-config-test.json";
  AppConfig audioConfig;
  audioConfig.source.audioSink = SilentAudioSink;
  CHECK(saveConfig(audioConfig, audioConfigPath, error));
  auto loadedAudioConfig = loadConfig(audioConfigPath);
  CHECK(loadedAudioConfig.source.audioSink == SilentAudioSink);
  std::filesystem::remove(audioConfigPath);
  if (failed) std::cerr << failed << " test(s) failed\n";
  return failed ? 1 : 0;
}
