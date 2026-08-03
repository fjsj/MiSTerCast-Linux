#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

#include "mistercast/audio_pacer.hpp"
#include "mistercast/audio_ring.hpp"
#include "mistercast/config.hpp"
#include "mistercast/groovy_transport.hpp"
#include "mistercast/interfaces.hpp"
#include "mistercast/pattern.hpp"
#include "mistercast/stream_session.hpp"
#include "mistercast/udp_pacing.hpp"
using namespace mistercast;
static int failed = 0;
#define CHECK(x)                                                              \
  do {                                                                        \
    if (!(x)) {                                                               \
      std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #x "\n"; \
      ++failed;                                                               \
    }                                                                         \
  } while (0)



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
  std::vector<CaptureWindow> windows(std::string&) override { return {}; }
  bool start(const SourceOptions&, ErrorCallback) override { return true; }
  SourceGeometry selectedGeometry() const override {
    const auto monitor = geometry();
    return {monitor.width, monitor.height};
  }
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
  FakeMister mister(0xcc);
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
  CHECK(stats.transport.fpgaStatusSamples > 0 &&
        stats.transport.fpgaFallbackSamples ==
            stats.transport.fpgaStatusSamples &&
        stats.transport.vramUnsyncedSamples == 0 &&
        stats.transport.vramQueueEmptySamples == 0 &&
        stats.transport.vramSynced && stats.transport.vgaFrameskip &&
        stats.transport.vramQueuePresent);
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
  cfg.source.captureMode = CaptureMode::Window;
  cfg.source.window = CaptureWindow{12345, "Example window", 640, 480};
  auto custom = Modeline::safeDefault();
  custom.name = "My preset";
  cfg.customModelines.push_back(custom);
  CHECK(saveConfig(cfg, path, error));
  {
    std::ifstream saved(path);
    const std::string json((std::istreambuf_iterator<char>(saved)),
                           std::istreambuf_iterator<char>());
    CHECK(json.find("windowId") == std::string::npos);
    CHECK(json.find("windowTitle") == std::string::npos);
  }
  std::string warning;
  auto loaded = loadConfig(path, &warning);
  CHECK(loaded.target == cfg.target &&
        loaded.source.progressiveInterlaceBuffer &&
        loaded.source.sampling == SamplingMode::LineBlend &&
        loaded.source.captureMode == CaptureMode::Window &&
        !loaded.source.window && warning.empty());
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
