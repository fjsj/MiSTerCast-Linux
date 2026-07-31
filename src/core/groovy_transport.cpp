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
  // Non-blocking, and a failed datagram is counted rather than fatal. A core
  // reload makes the MiSTer answer ICMP port unreachable, so the next send
  // returns ECONNREFUSED; a burst can return ENOBUFS. Tearing the session down
  // for either is worse than dropping the datagram, which this protocol already
  // tolerates via its ACKs. A blocking send would instead stall the render
  // thread whenever the socket buffer filled.
  auto r = ::send(fd_, p, n, MSG_NOSIGNAL | MSG_DONTWAIT);
  if (r < 0 || size_t(r) != n) {
    ++sendErrors_;
    return true;
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
  ackedFrames_ = missedAcks_ = streamTimeUs_ = ackAgeMs_ = sendErrors_ = 0;
  coreVersion_ = 0;
  lastSendEndAt_ = {};
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
  // Groovy answers CMD_INIT with a one-byte core version, and blit ACKs are 13
  // bytes. Anything else is not this protocol, so do not accept it as proof the
  // target is a MiSTer.
  if (received != 1 && received != 13) {
    e = "invalid CMD_INIT acknowledgment";
    close();
    return false;
  }
  // Seeds the round-trip estimate that biases the automatic sync line; waitSync
  // then keeps refining it from real ACKs, so one unlucky startup sample cannot
  // skew the whole session.
  networkRttNs_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now() - pingStart)
                      .count();
  if (received == 1) coreVersion_ = ack[0];
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
  firstFrame_ = true;
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
  // Warm-up is keyed on the frame number, as Windows did. Keying it on
  // successful ACKs meant that on a link where ACKs miss the poll window,
  // ackedFrames_ never reached ten, so the sync line stayed pinned at vTotal/2
  // and the raster correction never applied for the whole session.
  if (currentFrame_ <= 10) return std::max<uint16_t>(1, vTotal_ >> 1);
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
  if (firstFrame_) {
    // Start pacing from the first real frame. syncEpoch_ was set when the mode
    // was sent, and the thread startup and audio prebuffer in between are not
    // work this frame should be charged for. Windows instead skipped blitting
    // frame one, but that was to hide MAME loading its roms; the part worth
    // keeping is resetting the timing baseline.
    syncEpoch_ = sendStart;
    firstFrame_ = false;
  }
  const uint64_t workNs =
      syncEpoch_.time_since_epoch().count()
          ? std::chrono::duration_cast<std::chrono::nanoseconds>(sendStart -
                                                                 syncEpoch_)
                .count()
          : 0;
  currentFrame_ = frame;
  const uint16_t vsync = syncLine(workNs);
  syncLine_ = vsync;
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
  lastSendEndAt_ = std::chrono::steady_clock::now();
  lastStreamNs_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      lastSendEndAt_ - sendStart)
                      .count();
  streamTimeUs_ = lastStreamNs_ / 1000;
  drainStatus(frame);
  return true;
}

int64_t GroovyTransport::rasterCorrection() const noexcept {
  if (!syncRefresh_ || fpga_.frameEcho != currentFrame_ || !fpga_.vCountEcho ||
      !vTotal_)
    return 0;
  const int64_t requested =
      (int64_t(fpga_.frameEcho) - 1) * vTotal_ + fpga_.vCountEcho;
  const int64_t raster = int64_t(fpga_.frame) * vTotal_ + fpga_.vCount;
  const int64_t correctionNs =
      (int64_t(lineTimeNs_) * ((requested - raster) >> interlaceShift_)) / 2;
  return std::clamp<int64_t>(correctionNs, -int64_t(frameTimeNs_),
                             int64_t(frameTimeNs_));
}

void GroovyTransport::waitSync() noexcept {
  if (!frameTimeNs_) return;
  const auto start = std::chrono::steady_clock::now();
  const auto workNs =
      std::chrono::duration_cast<std::chrono::nanoseconds>(start - syncEpoch_)
          .count();
  auto pacingDeadline = [&](int64_t correctionNs) {
    return start + std::chrono::nanoseconds(std::clamp<int64_t>(
                       int64_t(frameTimeNs_) - workNs + correctionNs, 0,
                       int64_t(frameTimeNs_ * 2)));
  };
  bool matched = fpga_.frameEcho == currentFrame_ || drainStatus(currentFrame_);
  int64_t correctionNs = rasterCorrection();
  auto deadline = pacingDeadline(correctionNs);
  // While the ACK is still outstanding, spend the wait sleeping on the socket
  // rather than on a timer, so an ACK that arrives late still corrects this
  // frame instead of being counted as missed. Windows likewise polled the raster
  // inside WaitSync rather than only before it. The window is a short guaranteed
  // minimum extended across the rest of the pacing wait, which for a real
  // modeline is most of a frame period rather than the previous fixed 2 ms.
  const auto ackFloor = start + std::chrono::milliseconds(2);
  while (!matched && fd_ >= 0) {
    const auto until =
        std::max(ackFloor, deadline - std::chrono::milliseconds(1));
    const auto now = std::chrono::steady_clock::now();
    if (now >= until) break;
    pollfd descriptor{fd_, POLLIN, 0};
    const int timeoutMs = std::max(
        1, int(std::chrono::duration_cast<std::chrono::milliseconds>(until - now)
                   .count()));
    if (poll(&descriptor, 1, timeoutMs) <= 0) break;
    if (!drainStatus(currentFrame_)) continue;
    matched = true;
    correctionNs = rasterCorrection();
    deadline = pacingDeadline(correctionNs);
  }
  if (matched) {
    ++ackedFrames_;
    // Track the round trip continuously instead of trusting the single startup
    // sample. Implausible samples are ignored so a late ACK cannot poison it.
    if (lastSendEndAt_.time_since_epoch().count() &&
        lastAckAt_ >= lastSendEndAt_) {
      const auto sample = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              lastAckAt_ - lastSendEndAt_)
                              .count();
      if (uint64_t(sample) < frameTimeNs_)
        networkRttNs_ = (networkRttNs_ * 7 + uint64_t(sample)) / 8;
    }
  } else
    ++missedAcks_;
  rasterCorrectionUs_ = correctionNs / 1000;
  const auto sleepUntil = deadline - std::chrono::microseconds(100);
  if (sleepUntil > std::chrono::steady_clock::now())
    std::this_thread::sleep_until(sleepUntil);
  while (std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
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
  s.sendErrors = sendErrors_;
  s.networkRttUs = networkRttNs_ / 1000;
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
