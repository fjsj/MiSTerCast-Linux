#include "mistercast/groovy_transport.hpp"

#include <netdb.h>
#include <netinet/ip.h>
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

// Frame counters wrap naturally. Signed serial-number comparison keeps a
// nearby wrapped value ordered without treating UINT32_MAX as permanently
// newer than zero.
bool frameAfter(uint32_t a, uint32_t b) {
  return int32_t(a - b) > 0;
}
}  // namespace

bool compressionAvailable() noexcept {
#ifdef MISTERCAST_HAVE_LZ4
  return true;
#else
  return false;
#endif
}

bool encodeInitCommand(bool compression, bool audioEnabled,
                       uint32_t audioRate, std::array<uint8_t, 5>& command,
                       std::string& error) noexcept {
  uint8_t rateCode = 0;
  if (audioEnabled) {
    rateCode = audioRate == 22050   ? 1
               : audioRate == 44100 ? 2
               : audioRate == 48000 ? 3
                                    : 0;
    if (!rateCode) {
      error = "unsupported audio sample rate";
      return false;
    }
  }
  command = {CMD_INIT, uint8_t(compression), rateCode,
             uint8_t(audioEnabled ? 2 : 0), 0};
  return true;
}

GroovyTransport::GroovyTransport(UdpSubmitSyscalls* syscalls)
    : udpSyscalls_(syscalls ? syscalls : &systemUdpSubmitSyscalls()) {}
GroovyTransport::~GroovyTransport() { close(); }

bool GroovyTransport::decodeStatus(const uint8_t* data, size_t size,
                                   FpgaStatus& status) noexcept {
  if (size != 13) return false;
  status.frameEcho = readLe<uint32_t>(data);
  status.vCountEcho = readLe<uint16_t>(data + 4);
  status.frame = readLe<uint32_t>(data + 6);
  status.vCount = readLe<uint16_t>(data + 10);
  status.bits = data[12];
  return true;
}

void GroovyTransport::applyStatus(const FpgaStatus& status) noexcept {
  fpga_ = status;
  haveFpgaStatus_ = true;
  ackFrame_ = status.frameEcho;
  fpgaFrame_ = status.frame;
  fpgaVCount_ = status.vCount;
  fpgaField_ = (status.bits >> 5) & 1;
  vramSynced_ = (status.bits & 0x04) != 0;
  vgaFrameskip_ = (status.bits & 0x08) != 0;
  vgaVblank_ = (status.bits & 0x10) != 0;
  misterAudioEnabled_ = (status.bits & 0x40) != 0;
  vramQueuePresent_ = (status.bits & 0x80) != 0;
}

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
    FpgaStatus status;
    if (!decodeStatus(ack, size_t(received), status)) continue;
    if (haveFpgaStatus_ && frameAfter(fpga_.frameEcho, status.frameEcho))
      continue;
    applyStatus(status);
    lastAckAt_ = std::chrono::steady_clock::now();
    if (status.frameEcho == expectedFrame) {
      matched = true;
      // switchMode invalidates the old raster phase. Only an ACK echoing a
      // blit sent after that switch may establish the new mode's phase.
      phaseValid_ = true;
      fieldPhaseValid_ = true;
      if (!haveDiagnosticFrame_ || status.frameEcho != diagnosticFrame_) {
        haveDiagnosticFrame_ = true;
        diagnosticFrame_ = status.frameEcho;
        ++fpgaStatusSamples_;
        if (vgaFrameskip_) ++fpgaFallbackSamples_;
        if (!vramSynced_) ++vramUnsyncedSamples_;
        if (!vramQueuePresent_) ++vramQueueEmptySamples_;
        if (adaptiveTimingEligible()) {
          if (vramSynced_ && !vgaFrameskip_ && vramQueuePresent_)
            adaptiveMargin_.healthyAck();
          else
            adaptiveMargin_.unhealthyAck();
        }
      }
    }
  }
}

bool GroovyTransport::sendPacket(const void* p, size_t n, std::string& e) {
  if (fd_ < 0) {
    e = "transport is closed";
    return false;
  }
  if (fatalPayloadError_) {
    e = "transport requires reconnect after video payload failure";
    return false;
  }
  // A frame is one header followed by an exact byte count split across ordered
  // datagrams. Silently dropping a chunk leaves the receiver consuming bytes
  // from later commands as the unfinished frame, so a header ACK is not proof
  // that it is safe to continue. Let the socket apply backpressure (bounded by
  // SO_SNDTIMEO) and fail explicitly if a complete datagram cannot be queued.
  ssize_t r;
  do {
    r = ::send(fd_, p, n, MSG_NOSIGNAL);
  } while (r < 0 && errno == EINTR);
  if (r < 0 || size_t(r) != n) {
    ++sendErrors_;
    e = r < 0 ? udpSendError(errno, "UDP send")
              : "UDP send failed: incomplete datagram";
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

bool GroovyTransport::open(const std::string& host, bool audioEnabled,
                           uint32_t rate, std::string& e, uint16_t port) {
  close();
  ackFrame_ = fpgaFrame_ = 0;
  syncLine_ = fpgaVCount_ = 0;
  ackedFrames_ = missedAcks_ = streamTimeUs_ = ackAgeMs_ = sendErrors_ = 0;
  fieldRealignments_ = 0;
  fpgaStatusSamples_ = fpgaFallbackSamples_ = vramUnsyncedSamples_ =
      vramQueueEmptySamples_ = 0;
  compressionTimeUs_ = submissionTimeUs_ = estimatedWireTimeUs_ = 0;
  pacedVideoPayloads_ = pacedDatagrams_ = lateBatchReleases_ =
      maxBatchReleaseLatenessNs_ = observedUdpQueueHighWater_ = 0;
  socketSendBufferBytes_ = 0;
  pathMtu_ = 0;
  coreVersion_ = 0;
  lastSendEndAt_ = {};
  lastWireDeliveryNs_ = 0;
  rasterCorrectionUs_ = 0;
  outgoingField_ = fpgaField_ = 0;
  interlacedFieldBuffer_ = fieldPhaseValid_ = false;
  phaseValid_ = fallbackPhaseSet_ = lastAligned_ = haveDiagnosticFrame_ =
      false;
  diagnosticFrame_ = 0;
  fatalPayloadError_ = false;
  adaptiveMargin_.close();
  fpga_ = {};
  haveFpgaStatus_ = false;
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
  std::string candidateError;
  for (auto* p = list; p; p = p->ai_next) {
    int fd =
        socket(p->ai_family, p->ai_socktype | SOCK_CLOEXEC, p->ai_protocol);
    if (fd < 0) continue;
    if (!configureStrictPathMtu(fd, candidateError)) {
      ::close(fd);
      continue;
    }
    int snd = 2 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &snd, sizeof(snd));
    // stop() joins the rendering thread before closing the transport, so bound
    // backpressure to keep shutdown responsive even if an interface stalls.
    timeval sendTimeout{0, 100000};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &sendTimeout,
               sizeof(sendTimeout));
    if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) {
      int routeMtu = 0;
      socklen_t routeMtuSize = sizeof(routeMtu);
      if (getsockopt(fd, IPPROTO_IP, IP_MTU, &routeMtu, &routeMtuSize) == 0 &&
          routeMtu > 0) {
        if (!validatePathMtu(uint32_t(routeMtu), candidateError)) {
          ::close(fd);
          continue;
        }
        pathMtu_ = uint32_t(routeMtu);
      }
      fd_ = fd;
      break;
    }
    candidateError = std::string("cannot connect UDP target: ") +
                     std::strerror(errno);
    ::close(fd);
  }
  freeaddrinfo(list);
  if (fd_ < 0) {
    e = candidateError.empty() ? "cannot create UDP connection to target"
                               : candidateError;
    return false;
  }
  int actualSendBuffer = 0;
  socklen_t actualSendBufferSize = sizeof(actualSendBuffer);
  if (getsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &actualSendBuffer,
                 &actualSendBufferSize) == 0 &&
      actualSendBuffer > 0)
    socketSendBufferBytes_ = uint64_t(actualSendBuffer);
  std::array<uint8_t, 5> cmd{};
  if (!encodeInitCommand(
#ifdef MISTERCAST_HAVE_LZ4
          true,
#else
          false,
#endif
          audioEnabled, rate, cmd, e)) {
    close();
    return false;
  }
  auto pingStart = std::chrono::steady_clock::now();
  if (!sendPacket(cmd.data(), cmd.size(), e)) {
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
    FpgaStatus status;
    decodeStatus(ack, size_t(received), status);
    applyStatus(status);
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
  // CMD_SWITCHRES can reset the FPGA raster and field to a different phase.
  // Keep pre-switch status available for ordinary diagnostics, but never use
  // it to select a field in the new mode. The first matching post-switch blit
  // ACK re-establishes authoritative FPGA phase.
  phaseValid_ = fallbackPhaseSet_ = lastAligned_ = false;
  interlacedFieldBuffer_ = m.interlaced && !progressiveInterlaceBuffer_;
  fieldPhaseValid_ = !interlacedFieldBuffer_;
  outgoingField_ = 0;
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
  compressed_.resize(frameBytes_);
  vTotal_ = m.vTotal;
  interlaceShift_ = m.interlaced ? 1 : 0;
  lineTimeNs_ =
      uint64_t(std::llround(double(m.hTotal) * 1000.0 / m.pixelClockMHz));
  frameTimeNs_ = (lineTimeNs_ * m.vTotal) >> interlaceShift_;
  adaptiveMargin_.configure(vTotal_, interlacedFieldBuffer_);
  syncEpoch_ = std::chrono::steady_clock::now();
  lastAckAt_ = {};
  lastStreamNs_ = 0;
  lastWireDeliveryNs_ = 0;
  currentFrame_ = 0;
  firstFrame_ = true;
  return sendPacket(b, sizeof(b), e);
}

void GroovyTransport::setSyncOptions(bool syncRefresh,
                                     uint16_t frameDelay) noexcept {
  syncRefresh_ = syncRefresh;
  frameDelay_ = std::min<uint16_t>(frameDelay, 10);
  if (!syncRefresh_ || frameDelay_) adaptiveMargin_.missingAck();
}

void GroovyTransport::alignFrame(uint32_t& frame, uint8_t& field) noexcept {
  if (!interlaceShift_) {
    field = 0;
  } else if (phaseValid_) {
    if (frameAfter(fpga_.frame, frame)) frame = fpga_.frame + 1;
    field = uint8_t((!(fpga_.bits & 0x20)) ^ ((frame - fpga_.frame) & 1));
  } else {
    // The mode reset deterministically starts from field zero. Continue that
    // phase locally until a post-switch ACK arrives, rather than using stale
    // pre-switch FPGA state or repeatedly updating the same field buffer.
    if (!fallbackPhaseSet_) {
      fallbackFrame_ = frame;
      fallbackPhaseSet_ = true;
    }
    field = uint8_t((frame - fallbackFrame_) & 1);
  }

  if (interlaceShift_ && phaseValid_ && lastAligned_) {
    const uint8_t continued =
        uint8_t(lastAlignedField_ ^ ((frame - lastAlignedFrame_) & 1));
    if (field != continued) ++fieldRealignments_;
  }
  lastAlignedFrame_ = frame;
  lastAlignedField_ = field;
  lastAligned_ = true;
  outgoingField_ = progressiveInterlaceBuffer_ ? 0 : field;
  fieldPhaseValid_ = !interlacedFieldBuffer_ || phaseValid_;
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
  // Field-buffer interlace has only one field period to finish and publish the
  // next alternating buffer.  Once capture moved off this thread, the generic
  // automatic calculation started requesting lines near vActive/vTotal (480-
  // 516 for 640x480i).  That lets an upload race the bottom of the field; one
  // late or dropped datagram leaves garbage there and can strand the receiver
  // waiting for the rest of the frame.  The old synchronous capture path
  // happened to keep the request in the first half of the raster.  Make that
  // safety requirement explicit for alternating field buffers. Adaptive mode
  // begins from that conservative cap only after a matching post-switch ACK,
  // and can move it later after sustained healthy receiver feedback. The
  // opt-in progressive framebuffer keeps the normal low-latency calculation.
  uint16_t latestSafeLine = vTotal_;
  if (interlaceShift_ && !progressiveInterlaceBuffer_) {
    latestSafeLine = adaptiveTimingEligible()
                         ? adaptiveMargin_.stats().latestSafeLine
                         : std::max<uint16_t>(1, vTotal_ >> 1);
  }
  // lastStreamNs_ contains only active compression/syscall work because the
  // historical formula subtracts it. Paced wire delivery is future work for
  // the next field, so account for its estimate with the opposite sign.
  const int64_t leadNs =
      int64_t(networkRttNs_ + kAutoMarginNs + workNs + lastWireDeliveryNs_) -
      int64_t(lastStreamNs_);
  if (leadNs <= 0) return 1;
  if (uint64_t(leadNs) >= frameTimeNs_) return 1;
  auto lines = uint64_t(
      std::llround(double(vTotal_) * double(leadNs) / double(frameTimeNs_)));
  return uint16_t(std::clamp<uint64_t>(
      vTotal_ - std::min<uint64_t>(lines, vTotal_ - 1), 1, latestSafeLine));
}

bool GroovyTransport::adaptiveTimingEligible() const noexcept {
  // Every video payload uses submitVideoDatagrams, which applies pacing when
  // its packet count requires it; there is no unpaced large-payload mode.
  return interlacedFieldBuffer_.load() && fieldPhaseValid_.load() &&
         syncRefresh_ && frameDelay_ == 0;
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
  const auto compressionStarted = std::chrono::steady_clock::now();
#ifdef MISTERCAST_HAVE_LZ4
  int z = LZ4_compress_default(reinterpret_cast<const char*>(rgb.data()),
                               reinterpret_cast<char*>(compressed_.data()),
                               int(rgb.size()), int(compressed_.size()));
  if (z > 0) {
    csize = uint32_t(z);
    bytes = size_t(z);
    payload = compressed_.data();
  }
#endif
  const uint64_t compressionNs =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - compressionStarted)
          .count();
  compressionTimeUs_ = compressionNs / 1000;
  uint8_t h[12]{};
  h[0] = CMD_BLIT_FIELD_VSYNC;
  std::memcpy(h + 1, &frame, 4);
  h[5] = progressiveInterlaceBuffer_ ? 0 : field;
  std::memcpy(h + 6, &vsync, 2);
  if (csize) std::memcpy(h + 8, &csize, 4);
  size_t hs = csize ? 12 : 8;
  const size_t packetCount = videoDatagramCount(bytes);
  if (packetCount > messages_.size()) {
    e = "video payload exceeds descriptor capacity";
    return false;
  }
  for (size_t i = 0, offset = 0; i < packetCount; ++i) {
    const size_t packetBytes = std::min<size_t>(UdpPayloadBytes, bytes - offset);
    iovecs_[i].iov_base = const_cast<uint8_t*>(payload + offset);
    iovecs_[i].iov_len = packetBytes;
    messages_[i].msg_hdr.msg_iov = &iovecs_[i];
    messages_[i].msg_hdr.msg_iovlen = 1;
    messages_[i].msg_len = 0;
    offset += packetBytes;
  }
  auto& syscalls = *udpSyscalls_;
  uint64_t queueBefore = 0;
  if (syscalls.outputQueueBytes(fd_, queueBefore)) {
    auto peak = observedUdpQueueHighWater_.load();
    while (peak < queueBefore &&
           !observedUdpQueueHighWater_.compare_exchange_weak(peak,
                                                             queueBefore)) {}
  }
  const auto submissionStarted = std::chrono::steady_clock::now();
  const auto headerStarted = submissionStarted;
  if (!sendPacket(h, hs, e)) return false;
  const uint64_t headerNs =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - headerStarted)
          .count();
  VideoSubmissionStats submission;
  const bool submitted = submitVideoDatagrams(
      fd_, messages_.data(), packetCount, frameTimeNs_, syscalls, submission, e);
  submissionTimeUs_ =
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - submissionStarted)
          .count();
  estimatedWireTimeUs_ = submission.estimatedWireNs / 1000;
  lastWireDeliveryNs_ = submission.estimatedWireNs;
  if (submission.paced) {
    ++pacedVideoPayloads_;
    pacedDatagrams_ += submission.submittedDatagrams;
  }
  lateBatchReleases_ += submission.lateBatchReleases;
  auto maxLate = maxBatchReleaseLatenessNs_.load();
  while (maxLate < submission.maxReleaseLatenessNs &&
         !maxBatchReleaseLatenessNs_.compare_exchange_weak(
             maxLate, submission.maxReleaseLatenessNs)) {}
  auto queuePeak = observedUdpQueueHighWater_.load();
  while (queuePeak < submission.observedQueueHighWater &&
         !observedUdpQueueHighWater_.compare_exchange_weak(
             queuePeak, submission.observedQueueHighWater)) {}
  if (!submitted) {
    ++sendErrors_;
    fatalPayloadError_ = true;
    return false;
  }
  lastSendEndAt_ = std::chrono::steady_clock::now();
  // Intentional release sleeps are delivery scheduling, not evidence that the
  // sender began its work late. Keep syncLine's historical active-work input
  // separate from the paced wall-clock submission duration.
  lastStreamNs_ = compressionNs + headerNs + submission.activeSubmissionNs;
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
  // frame instead of being counted as missed. Windows likewise polled the
  // raster inside WaitSync rather than only before it. The window is a short
  // guaranteed minimum extended across the rest of the pacing wait, which for a
  // real modeline is most of a frame period rather than the previous fixed 2
  // ms.
  const auto ackFloor = start + std::chrono::milliseconds(2);
  while (!matched && fd_ >= 0) {
    const auto until =
        std::max(ackFloor, deadline - std::chrono::milliseconds(1));
    const auto now = std::chrono::steady_clock::now();
    if (now >= until) break;
    pollfd descriptor{fd_, POLLIN, 0};
    const int timeoutMs = std::max(
        1,
        int(std::chrono::duration_cast<std::chrono::milliseconds>(until - now)
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
  } else {
    ++missedAcks_;
    adaptiveMargin_.missingAck();
  }
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
  s.fieldRealignments = fieldRealignments_;
  s.fpgaStatusSamples = fpgaStatusSamples_;
  s.fpgaFallbackSamples = fpgaFallbackSamples_;
  s.vramUnsyncedSamples = vramUnsyncedSamples_;
  s.vramQueueEmptySamples = vramQueueEmptySamples_;
  s.compressionTimeUs = compressionTimeUs_;
  s.submissionTimeUs = submissionTimeUs_;
  s.estimatedWireTimeUs = estimatedWireTimeUs_;
  s.pacedVideoPayloads = pacedVideoPayloads_;
  s.pacedDatagrams = pacedDatagrams_;
  s.lateBatchReleases = lateBatchReleases_;
  s.maxBatchReleaseLatenessNs = maxBatchReleaseLatenessNs_;
  s.observedUdpQueueHighWater = observedUdpQueueHighWater_;
  s.socketSendBufferBytes = socketSendBufferBytes_;
  s.pathMtu = pathMtu_;
  const auto adaptive = adaptiveMargin_.stats();
  s.deliveryReserveLines = adaptive.reserveLines;
  s.adaptiveLatestSafeLine = adaptive.latestSafeLine;
  s.adaptiveHealthyAcks = adaptive.healthyAcks;
  s.adaptiveReductions = adaptive.reductions;
  s.adaptiveResets = adaptive.resets;
  s.adaptiveTimingEligible = adaptiveTimingEligible();
  s.outgoingField = outgoingField_;
  s.fpgaField = fpgaField_;
  s.vramSynced = vramSynced_;
  s.vgaFrameskip = vgaFrameskip_;
  s.vgaVblank = vgaVblank_;
  s.vramQueuePresent = vramQueuePresent_;
  s.interlacedFieldBuffer = interlacedFieldBuffer_;
  s.fieldPhaseValid = fieldPhaseValid_;
  return s;
}

void GroovyTransport::close() noexcept {
  if (fd_ >= 0) {
    uint8_t c = CMD_CLOSE;
    std::string ignored;
    if (!fatalPayloadError_) sendPacket(&c, 1, ignored);
    ::close(fd_);
    fd_ = -1;
  }
  misterAudioEnabled_ = false;
  vramSynced_ = false;
  vgaFrameskip_ = false;
  vgaVblank_ = false;
  vramQueuePresent_ = false;
  interlacedFieldBuffer_ = false;
  fieldPhaseValid_ = false;
  phaseValid_ = fallbackPhaseSet_ = lastAligned_ = haveDiagnosticFrame_ =
      false;
  diagnosticFrame_ = 0;
  fatalPayloadError_ = false;
  adaptiveMargin_.close();
  fpgaStatusSamples_ = fpgaFallbackSamples_ = vramUnsyncedSamples_ =
      vramQueueEmptySamples_ = 0;
  compressionTimeUs_ = submissionTimeUs_ = estimatedWireTimeUs_ = 0;
  pacedVideoPayloads_ = pacedDatagrams_ = lateBatchReleases_ =
      maxBatchReleaseLatenessNs_ = observedUdpQueueHighWater_ = 0;
  socketSendBufferBytes_ = 0;
  pathMtu_ = 0;
  progressiveInterlaceBuffer_ = false;
  frameBytes_ = 0;
  frameTimeNs_ = lineTimeNs_ = 0;
  lastWireDeliveryNs_ = 0;
  compressed_.clear();
}
}  // namespace mistercast
