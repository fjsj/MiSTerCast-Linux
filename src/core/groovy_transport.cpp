#include "mistercast/groovy_transport.hpp"

#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <thread>
#ifdef MISTERCAST_HAVE_LZ4
extern "C" int LZ4_compress_default(const char*, char*, int, int);
#endif

namespace mistercast {
namespace {
constexpr uint8_t CMD_CLOSE = 1, CMD_INIT = 2, CMD_SWITCHRES = 3, CMD_AUDIO = 4,
                  CMD_BLIT_FIELD_VSYNC = 7;
constexpr uint8_t INTERLACE_FIELD_BUFFER = 1, INTERLACE_PROGRESSIVE_BUFFER = 2;
constexpr uint64_t kAutoMarginNs = 1500000;

template <class T>
T readLe(const uint8_t* p) {
  T value{};
  std::memcpy(&value, p, sizeof(value));
  return value;
}
}  // namespace

GroovyTransport::GroovyTransport() = default;
GroovyTransport::~GroovyTransport() { close(); }

bool GroovyTransport::drainStatus(uint32_t expectedFrame) noexcept {
  if (fd_ < 0) return false;
  uint8_t ack[32];
  bool matched = false;
  for (;;) {
    auto received = recv(fd_, ack, sizeof(ack), MSG_DONTWAIT);
    if (received < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) return matched;
      return matched;
    }
    if (received != 13) continue;
    FpgaStatus status;
    status.frameEcho = readLe<uint32_t>(ack);
    status.vCountEcho = readLe<uint16_t>(ack + 4);
    status.frame = readLe<uint32_t>(ack + 6);
    status.vCount = readLe<uint16_t>(ack + 10);
    status.bits = ack[12];
    if (status.frameEcho < fpga_.frameEcho) continue;
    fpga_ = status;
    lastAckAt_ = std::chrono::steady_clock::now();
    ackFrame_ = status.frameEcho;
    fpgaFrame_ = status.frame;
    fpgaVCount_ = status.vCount;
    vramSynced_ = (status.bits & 0x04) != 0;
    vgaFrameskip_ = (status.bits & 0x08) != 0;
    vgaVblank_ = (status.bits & 0x10) != 0;
    misterAudioEnabled_ = (status.bits & 0x40) != 0;
    if (status.frameEcho == expectedFrame) matched = true;
  }
}

bool GroovyTransport::sendPacket(const void* p, size_t n, std::string& e) {
  if (fd_ < 0) {
    e = "transport is closed";
    return false;
  }
  auto r = ::send(fd_, p, n, MSG_NOSIGNAL);
  if (r < 0 || size_t(r) != n) {
    e = std::string("UDP send failed: ") + std::strerror(errno);
    return false;
  }
  return true;
}

bool GroovyTransport::sendChunks(const uint8_t* p, size_t n, std::string& e) {
  while (n) {
    size_t z = std::min<size_t>(mtu_, n);
    if (!sendPacket(p, z, e)) return false;
    p += z;
    n -= z;
  }
  return true;
}

bool GroovyTransport::open(const std::string& host, uint32_t rate,
                           std::string& e, uint16_t port) {
  close();
  ackFrame_ = fpgaFrame_ = 0;
  syncLine_ = fpgaVCount_ = 0;
  ackedFrames_ = missedAcks_ = streamTimeUs_ = ackAgeMs_ = 0;
  rasterCorrectionUs_ = 0;
  fpga_ = {};
  if (host.empty()) {
    e = "target address is required";
    return false;
  }
  addrinfo hint{};
  hint.ai_family = AF_INET;
  hint.ai_socktype = SOCK_DGRAM;
  addrinfo* list = nullptr;
  auto service = std::to_string(port);
  int rc = getaddrinfo(host.c_str(), service.c_str(), &hint, &list);
  if (rc) {
    e = std::string("cannot resolve IPv4 target: ") + gai_strerror(rc);
    return false;
  }
  for (auto* p = list; p; p = p->ai_next) {
    int fd =
        socket(p->ai_family, p->ai_socktype | SOCK_CLOEXEC, p->ai_protocol);
    if (fd < 0) continue;
    int snd = 2 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &snd, sizeof(snd));
    if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) {
      fd_ = fd;
      break;
    }
    ::close(fd);
  }
  freeaddrinfo(list);
  if (fd_ < 0) {
    e = "cannot create UDP connection to target";
    return false;
  }
  uint8_t cmd[5] = {CMD_INIT,
#ifdef MISTERCAST_HAVE_LZ4
                    1,
#else
                    0,
#endif
                    uint8_t(rate == 22050   ? 1
                            : rate == 44100 ? 2
                            : rate == 48000 ? 3
                                            : 0),
                    2, 0};
  if (!cmd[2]) {
    e = "unsupported audio sample rate";
    close();
    return false;
  }
  auto pingStart = std::chrono::steady_clock::now();
  if (!sendPacket(cmd, sizeof(cmd), e)) {
    close();
    return false;
  }
  pollfd p{fd_, POLLIN, 0};
  if (poll(&p, 1, 250) <= 0) {
    e = "target did not acknowledge CMD_INIT (UDP port 32100)";
    close();
    return false;
  }
  uint8_t ack[32];
  auto received = recv(fd_, ack, sizeof(ack), 0);
  if (received <= 0) {
    e = "invalid CMD_INIT acknowledgment";
    close();
    return false;
  }
  networkRttNs_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now() - pingStart)
                      .count();
  if (received == 13) {
    fpga_.frameEcho = readLe<uint32_t>(ack);
    fpga_.vCountEcho = readLe<uint16_t>(ack + 4);
    fpga_.frame = readLe<uint32_t>(ack + 6);
    fpga_.vCount = readLe<uint16_t>(ack + 10);
    fpga_.bits = ack[12];
    fpgaFrame_ = fpga_.frame;
    fpgaVCount_ = fpga_.vCount;
    misterAudioEnabled_ = (ack[12] & 0x40) != 0;
  }
  return true;
}

bool GroovyTransport::switchMode(const Modeline& m,
                                 bool progressiveInterlaceBuffer,
                                 std::string& e) {
  if (auto x = m.validate()) {
    e = *x;
    return false;
  }
  progressiveInterlaceBuffer_ = m.interlaced && progressiveInterlaceBuffer;
  uint8_t b[26]{};
  b[0] = CMD_SWITCHRES;
  std::memcpy(b + 1, &m.pixelClockMHz, 8);
  std::memcpy(b + 9, &m.hActive, 2);
  std::memcpy(b + 11, &m.hBegin, 2);
  std::memcpy(b + 13, &m.hEnd, 2);
  std::memcpy(b + 15, &m.hTotal, 2);
  std::memcpy(b + 17, &m.vActive, 2);
  std::memcpy(b + 19, &m.vBegin, 2);
  std::memcpy(b + 21, &m.vEnd, 2);
  std::memcpy(b + 23, &m.vTotal, 2);
  b[25] = m.interlaced
              ? (progressiveInterlaceBuffer_ ? INTERLACE_PROGRESSIVE_BUFFER
                                             : INTERLACE_FIELD_BUFFER)
              : 0;
  frameBytes_ = uint32_t(m.hActive) * m.vActive * 3 /
                ((m.interlaced && !progressiveInterlaceBuffer_) ? 2 : 1);
  vTotal_ = m.vTotal;
  interlaceShift_ = m.interlaced ? 1 : 0;
  lineTimeNs_ =
      uint64_t(std::llround(double(m.hTotal) * 1000.0 / m.pixelClockMHz));
  frameTimeNs_ = (lineTimeNs_ * m.vTotal) >> interlaceShift_;
  syncEpoch_ = std::chrono::steady_clock::now();
  lastAckAt_ = {};
  lastStreamNs_ = 0;
  currentFrame_ = 0;
  return sendPacket(b, sizeof(b), e);
}

void GroovyTransport::setSyncOptions(bool syncRefresh,
                                     uint16_t frameDelay) noexcept {
  syncRefresh_ = syncRefresh;
  frameDelay_ = std::min<uint16_t>(frameDelay, 10);
}

void GroovyTransport::alignFrame(uint32_t& frame,
                                 uint8_t& field) const noexcept {
  if (fpga_.frame > frame) frame = fpga_.frame + 1;
  if (interlaceShift_)
    field = uint8_t((!(fpga_.bits & 0x20)) ^ ((frame - fpga_.frame) & 1));
  else
    field = 0;
}

uint16_t GroovyTransport::syncLine(uint64_t workNs) const noexcept {
  if (!syncRefresh_ || !vTotal_) return 0;
  if (frameDelay_) {
    auto line =
        uint32_t(std::llround(double(vTotal_) * frameDelay_ / 10.0)) + 1;
    return uint16_t(std::min<uint32_t>(line, vTotal_));
  }
  if (ackedFrames_.load() < 10) return std::max<uint16_t>(1, vTotal_ >> 1);
  const int64_t leadNs =
      int64_t(networkRttNs_ + kAutoMarginNs + workNs) - int64_t(lastStreamNs_);
  if (leadNs <= 0) return 1;
  if (uint64_t(leadNs) >= frameTimeNs_) return 1;
  auto lines = uint64_t(
      std::llround(double(vTotal_) * double(leadNs) / double(frameTimeNs_)));
  return uint16_t(std::clamp<uint64_t>(
      vTotal_ - std::min<uint64_t>(lines, vTotal_ - 1), 1, vTotal_));
}

bool GroovyTransport::sendFrame(uint32_t frame, uint8_t field,
                                const std::vector<uint8_t>& rgb,
                                std::string& e) {
  if (rgb.size() != frameBytes_) {
    e = "transformed frame has unexpected size";
    return false;
  }
  const auto sendStart = std::chrono::steady_clock::now();
  const uint64_t workNs =
      syncEpoch_.time_since_epoch().count()
          ? std::chrono::duration_cast<std::chrono::nanoseconds>(sendStart -
                                                                 syncEpoch_)
                .count()
          : 0;
  const uint16_t vsync = syncLine(workNs);
  syncLine_ = vsync;
  currentFrame_ = frame;
  const uint8_t* payload = rgb.data();
  size_t bytes = rgb.size();
  uint32_t csize = 0;
#ifdef MISTERCAST_HAVE_LZ4
  compressed_.resize(rgb.size());
  int z = LZ4_compress_default(reinterpret_cast<const char*>(rgb.data()),
                               reinterpret_cast<char*>(compressed_.data()),
                               int(rgb.size()), int(compressed_.size()));
  if (z > 0) {
    csize = uint32_t(z);
    bytes = size_t(z);
    payload = compressed_.data();
  }
#endif
  uint8_t h[12]{};
  h[0] = CMD_BLIT_FIELD_VSYNC;
  std::memcpy(h + 1, &frame, 4);
  h[5] = progressiveInterlaceBuffer_ ? 0 : field;
  std::memcpy(h + 6, &vsync, 2);
  if (csize) std::memcpy(h + 8, &csize, 4);
  size_t hs = csize ? 12 : 8;
  if (!sendPacket(h, hs, e) || !sendChunks(payload, bytes, e)) return false;
  lastStreamNs_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now() - sendStart)
                      .count();
  streamTimeUs_ = lastStreamNs_ / 1000;
  drainStatus(frame);
  return true;
}

void GroovyTransport::waitSync() noexcept {
  if (!frameTimeNs_) return;
  bool matched = fpga_.frameEcho == currentFrame_ || drainStatus(currentFrame_);
  if (!matched && fd_ >= 0) {
    const auto ackDeadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
    while (!matched && std::chrono::steady_clock::now() < ackDeadline) {
      pollfd descriptor{fd_, POLLIN, 0};
      const auto remaining =
          std::chrono::duration_cast<std::chrono::microseconds>(
              ackDeadline - std::chrono::steady_clock::now());
      const int timeoutMs = std::max(0, int((remaining.count() + 999) / 1000));
      if (poll(&descriptor, 1, timeoutMs) <= 0) break;
      matched = drainStatus(currentFrame_);
    }
  }
  if (matched)
    ++ackedFrames_;
  else
    ++missedAcks_;
  int64_t correctionNs = 0;
  if (syncRefresh_ && fpga_.frameEcho == currentFrame_ && fpga_.vCountEcho &&
      vTotal_) {
    const int64_t requested =
        (int64_t(fpga_.frameEcho) - 1) * vTotal_ + fpga_.vCountEcho;
    const int64_t raster = int64_t(fpga_.frame) * vTotal_ + fpga_.vCount;
    correctionNs =
        (int64_t(lineTimeNs_) * ((requested - raster) >> interlaceShift_)) / 2;
    correctionNs = std::clamp<int64_t>(correctionNs, -int64_t(frameTimeNs_),
                                       int64_t(frameTimeNs_));
  }
  rasterCorrectionUs_ = correctionNs / 1000;
  const auto now = std::chrono::steady_clock::now();
  const auto workNs =
      std::chrono::duration_cast<std::chrono::nanoseconds>(now - syncEpoch_)
          .count();
  const int64_t waitNs =
      std::clamp<int64_t>(int64_t(frameTimeNs_) - workNs + correctionNs, 0,
                          int64_t(frameTimeNs_ * 2));
  if (waitNs > 200000)
    std::this_thread::sleep_for(std::chrono::nanoseconds(waitNs - 100000));
  while (std::chrono::steady_clock::now() <
         now + std::chrono::nanoseconds(waitNs))
    std::this_thread::yield();
  syncEpoch_ = std::chrono::steady_clock::now();
  if (lastAckAt_.time_since_epoch().count())
    ackAgeMs_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                    syncEpoch_ - lastAckAt_)
                    .count();
}

bool GroovyTransport::sendAudio(const int16_t* s, size_t n, std::string& e) {
  size_t bytes = n * sizeof(int16_t);
  if (bytes > 65535) {
    e = "audio packet is too large";
    return false;
  }
  uint8_t h[3] = {CMD_AUDIO, 0, 0};
  uint16_t z = uint16_t(bytes);
  std::memcpy(h + 1, &z, 2);
  return sendPacket(h, 3, e) &&
         sendChunks(reinterpret_cast<const uint8_t*>(s), bytes, e);
}

GroovyTransportStats GroovyTransport::stats() const noexcept {
  GroovyTransportStats s;
  s.acknowledgedFrame = ackFrame_;
  s.fpgaFrame = fpgaFrame_;
  s.requestedSyncLine = syncLine_;
  s.fpgaVCount = fpgaVCount_;
  s.acknowledgedFrames = ackedFrames_;
  s.missedAcks = missedAcks_;
  s.streamTimeUs = streamTimeUs_;
  s.ackAgeMs = ackAgeMs_;
  s.rasterCorrectionUs = rasterCorrectionUs_;
  s.vramSynced = vramSynced_;
  s.vgaFrameskip = vgaFrameskip_;
  s.vgaVblank = vgaVblank_;
  return s;
}

void GroovyTransport::close() noexcept {
  if (fd_ >= 0) {
    uint8_t c = CMD_CLOSE;
    std::string ignored;
    sendPacket(&c, 1, ignored);
    ::close(fd_);
    fd_ = -1;
  }
  misterAudioEnabled_ = false;
  vramSynced_ = false;
  vgaFrameskip_ = false;
  vgaVblank_ = false;
  progressiveInterlaceBuffer_ = false;
  frameBytes_ = 0;
  frameTimeNs_ = lineTimeNs_ = 0;
  compressed_.clear();
}
}  // namespace mistercast
