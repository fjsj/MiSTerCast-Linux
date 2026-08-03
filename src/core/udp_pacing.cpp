#include "mistercast/udp_pacing.hpp"

#include <poll.h>
#include <sys/ioctl.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace mistercast {
namespace {
constexpr uint64_t kNanosecondsPerSecond = 1000000000;
constexpr uint64_t kMinimumGraceNs = 5000000;
constexpr uint64_t kMaximumGraceNs = 100000000;

uint64_t durationForWireBytes(uint64_t bytes, uint64_t rate) noexcept {
  if (!rate) return 0;
  const uint64_t whole = bytes / rate;
  const uint64_t remainder = bytes % rate;
  return whole * 8 * kNanosecondsPerSecond +
         (remainder * 8 * kNanosecondsPerSecond + rate - 1) / rate;
}

uint64_t messageWireBytes(const mmsghdr& message,
                          const UdpVideoConfig& config) noexcept {
  uint64_t payload = 0;
  for (size_t i = 0; i < message.msg_hdr.msg_iovlen; ++i)
    payload += message.msg_hdr.msg_iov[i].iov_len;
  return payload + config.wireOverheadBytes;
}

class SystemUdpSubmitSyscalls final : public UdpSubmitSyscalls {
 public:
  int64_t monotonicNowNs() noexcept override {
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return int64_t(now.tv_sec) * int64_t(kNanosecondsPerSecond) + now.tv_nsec;
  }

  int sleepUntil(int64_t deadlineNs) noexcept override {
    timespec deadline{deadlineNs / int64_t(kNanosecondsPerSecond),
                      deadlineNs % int64_t(kNanosecondsPerSecond)};
    int result;
    do {
      result = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline,
                               nullptr);
    } while (result == EINTR);
    return result;
  }

  int sendMessages(int fd, mmsghdr* messages, unsigned count,
                   int flags) noexcept override {
    const int result = sendmmsg(fd, messages, count, flags);
    return result < 0 ? -errno : result;
  }

  int waitWritable(int fd, int64_t deadlineNs) noexcept override {
    for (;;) {
      const int64_t remaining = deadlineNs - monotonicNowNs();
      if (remaining <= 0) return 0;
      const int timeoutMs = int(std::min<int64_t>(
          INT_MAX, (remaining + 999999) / 1000000));
      pollfd descriptor{fd, POLLOUT, 0};
      const int result = poll(&descriptor, 1, timeoutMs);
      if (result < 0 && errno == EINTR) continue;
      return result < 0 ? -errno : result;
    }
  }

  bool outputQueueBytes(int fd, uint64_t& bytes) noexcept override {
    int queued = 0;
    if (ioctl(fd, TIOCOUTQ, &queued) != 0) return false;
    bytes = queued > 0 ? uint64_t(queued) : 0;
    return true;
  }
};

void sampleQueue(int fd, UdpSubmitSyscalls& syscalls,
                 VideoSubmissionStats& stats) noexcept {
  uint64_t queued = 0;
  if (syscalls.outputQueueBytes(fd, queued))
    stats.observedQueueHighWater =
        std::max(stats.observedQueueHighWater, queued);
}
}  // namespace

size_t videoDatagramCount(size_t payloadBytes,
                          const UdpVideoConfig& config) noexcept {
  const size_t packetBytes = config.payloadBytes;
  return packetBytes
             ? (payloadBytes + packetBytes - 1) / packetBytes
             : 0;
}

uint64_t videoWireBytes(size_t payloadBytes,
                        const UdpVideoConfig& config) noexcept {
  if (!payloadBytes) return 0;
  return payloadBytes +
         videoDatagramCount(payloadBytes, config) * config.wireOverheadBytes;
}

uint64_t pacingDurationNs(size_t payloadBytes,
                          const UdpVideoConfig& config) noexcept {
  return durationForWireBytes(videoWireBytes(payloadBytes, config),
                              config.pacingBitsPerSecond);
}

uint64_t videoCompletionGraceNs(uint64_t framePeriodNs) noexcept {
  return std::clamp(framePeriodNs / 2, kMinimumGraceNs, kMaximumGraceNs);
}

bool validatePathMtu(uint32_t pathMtu, const UdpVideoConfig& config,
                     std::string& error) noexcept {
  const size_t requiredMtu = config.ipv4MtuBytes();
  if (pathMtu >= requiredMtu) return true;
  error = "detected path MTU " + std::to_string(pathMtu) +
          ", but MiSTerCast requires MTU " + std::to_string(requiredMtu) +
          " for " + std::to_string(config.payloadBytes) +
          "-byte UDP payloads; check tunnel, VPN, and interface MTU settings";
  return false;
}

std::string udpSendError(int errorNumber, const char* operation,
                         const UdpVideoConfig& config) {
  if (errorNumber == EMSGSIZE)
    return std::string(operation) + " failed: path MTU cannot carry a " +
           std::to_string(config.payloadBytes) +
           "-byte UDP payload without IPv4 fragmentation (MTU " +
           std::to_string(config.ipv4MtuBytes()) + " required)";
  return std::string(operation) + " failed: " + std::strerror(errorNumber);
}

bool submitVideoDatagrams(int fd, mmsghdr* messages, size_t count,
                          uint64_t framePeriodNs,
                          const UdpVideoConfig& config,
                          UdpSubmitSyscalls& syscalls,
                          VideoSubmissionStats& stats, std::string& error) {
  stats = {};
  if (!count) return true;
  if (!config.batchDatagrams || !config.pacingBitsPerSecond) {
    error = "invalid UDP video pacing configuration";
    return false;
  }
  stats.paced = count > config.batchDatagrams;
  uint64_t totalWireBytes = 0;
  for (size_t i = 0; i < count; ++i)
    totalWireBytes += messageWireBytes(messages[i], config);
  stats.estimatedWireNs =
      durationForWireBytes(totalWireBytes, config.pacingBitsPerSecond);
  const int64_t started = syscalls.monotonicNowNs();
  const int64_t completionDeadline =
      started + int64_t(stats.estimatedWireNs +
                        videoCompletionGraceNs(framePeriodNs));
  uint64_t releasedWireBytes = 0;
  size_t cursor = 0;
  while (cursor < count) {
    const size_t batchStart = cursor;
    const size_t batchEnd =
        std::min(count, cursor + config.batchDatagrams);
    if (stats.paced && batchStart) {
      const int64_t releaseDeadline =
          started + int64_t(durationForWireBytes(
                        releasedWireBytes, config.pacingBitsPerSecond));
      const int sleepError = syscalls.sleepUntil(releaseDeadline);
      if (sleepError) {
        error = std::string("video pacing sleep failed: ") +
                std::strerror(sleepError);
        return false;
      }
      const int64_t releasedAt = syscalls.monotonicNowNs();
      if (releasedAt > releaseDeadline) {
        const uint64_t late = uint64_t(releasedAt - releaseDeadline);
        stats.maxReleaseLatenessNs =
            std::max(stats.maxReleaseLatenessNs, late);
        if (late > config.lateToleranceNs) ++stats.lateBatchReleases;
      }
    }

    while (cursor < batchEnd) {
      const int64_t callStarted = syscalls.monotonicNowNs();
      const int result = syscalls.sendMessages(
          fd, messages + cursor, unsigned(batchEnd - cursor),
          MSG_NOSIGNAL | MSG_DONTWAIT);
      const int64_t callEnded = syscalls.monotonicNowNs();
      if (callEnded > callStarted)
        stats.activeSubmissionNs += uint64_t(callEnded - callStarted);
      if (result > 0) {
        cursor += size_t(result);
        stats.submittedDatagrams += size_t(result);
        continue;
      }
      if (result == -EINTR) continue;
      if (result == -EAGAIN || result == -EWOULDBLOCK) {
        const int ready = syscalls.waitWritable(fd, completionDeadline);
        if (ready > 0) continue;
        if (ready == 0) {
          error = "video payload submission deadline expired";
          return false;
        }
        error = std::string("video payload poll failed: ") +
                std::strerror(-ready);
        return false;
      }
      error = udpSendError(result ? -result : EIO, "video payload send",
                           config);
      return false;
    }
    for (size_t i = batchStart; i < batchEnd; ++i)
      releasedWireBytes += messageWireBytes(messages[i], config);
    sampleQueue(fd, syscalls, stats);
  }
  const int64_t finished = syscalls.monotonicNowNs();
  if (finished > started) stats.wallSubmissionNs = uint64_t(finished - started);
  return true;
}

class UdpVideoSender::Impl {
 public:
  Impl(UdpVideoConfig value, UdpSubmitSyscalls* seam)
      : config(std::move(value)),
        syscalls(seam ? seam : &systemUdpSubmitSyscalls()),
        messages(videoDatagramCount(config.maximumFrameBytes, config)),
        iovecs(messages.size()) {}

  UdpVideoConfig config;
  UdpSubmitSyscalls* syscalls;
  std::vector<mmsghdr> messages;
  std::vector<iovec> iovecs;
  mutable std::mutex statsMutex;
  UdpVideoSenderStats cumulative;
};

UdpVideoSender::UdpVideoSender(UdpVideoConfig config,
                               UdpSubmitSyscalls* syscalls)
    : impl_(nullptr) {
  if (!config.payloadBytes || !config.maximumFrameBytes ||
      !config.batchDatagrams || !config.pacingBitsPerSecond)
    throw std::invalid_argument("invalid UDP video sender configuration");
  impl_ = std::make_unique<Impl>(std::move(config), syscalls);
}

UdpVideoSender::~UdpVideoSender() = default;

const UdpVideoConfig& UdpVideoSender::config() const noexcept {
  return impl_->config;
}

void UdpVideoSender::reset() noexcept {
  std::lock_guard<std::mutex> lock(impl_->statsMutex);
  impl_->cumulative = {};
}

void UdpVideoSender::observeQueue(int fd) noexcept {
  uint64_t queued = 0;
  if (!impl_->syscalls->outputQueueBytes(fd, queued)) return;
  std::lock_guard<std::mutex> lock(impl_->statsMutex);
  impl_->cumulative.observedQueueHighWater =
      std::max(impl_->cumulative.observedQueueHighWater, queued);
}

bool UdpVideoSender::submit(int fd, const uint8_t* payload,
                            size_t payloadBytes, uint64_t framePeriodNs,
                            VideoSubmissionStats& submission,
                            std::string& error) {
  submission = {};
  const auto& config = impl_->config;
  if (!config.payloadBytes || payloadBytes > config.maximumFrameBytes) {
    error = "video payload exceeds configured UDP sender capacity";
    return false;
  }
  const size_t count = videoDatagramCount(payloadBytes, config);
  if (count > impl_->messages.size()) {
    error = "video payload exceeds descriptor capacity";
    return false;
  }
  for (size_t i = 0, offset = 0; i < count; ++i) {
    const size_t bytes =
        std::min(config.payloadBytes, payloadBytes - offset);
    impl_->iovecs[i].iov_base = const_cast<uint8_t*>(payload + offset);
    impl_->iovecs[i].iov_len = bytes;
    impl_->messages[i] = {};
    impl_->messages[i].msg_hdr.msg_iov = &impl_->iovecs[i];
    impl_->messages[i].msg_hdr.msg_iovlen = 1;
    offset += bytes;
  }
  const bool result = submitVideoDatagrams(
      fd, impl_->messages.data(), count, framePeriodNs, config,
      *impl_->syscalls, submission, error);
  std::lock_guard<std::mutex> lock(impl_->statsMutex);
  if (submission.paced) {
    ++impl_->cumulative.pacedPayloads;
    impl_->cumulative.pacedDatagrams += submission.submittedDatagrams;
  }
  impl_->cumulative.lateBatchReleases += submission.lateBatchReleases;
  impl_->cumulative.maxReleaseLatenessNs =
      std::max(impl_->cumulative.maxReleaseLatenessNs,
               submission.maxReleaseLatenessNs);
  impl_->cumulative.observedQueueHighWater =
      std::max(impl_->cumulative.observedQueueHighWater,
               submission.observedQueueHighWater);
  return result;
}

UdpVideoSenderStats UdpVideoSender::stats() const noexcept {
  std::lock_guard<std::mutex> lock(impl_->statsMutex);
  return impl_->cumulative;
}

UdpSubmitSyscalls& systemUdpSubmitSyscalls() noexcept {
  static SystemUdpSubmitSyscalls syscalls;
  return syscalls;
}
}  // namespace mistercast
