#include "mistercast/udp_pacing.hpp"

#include <poll.h>
#include <sys/ioctl.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>

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

uint64_t messageWireBytes(const mmsghdr& message) noexcept {
  uint64_t payload = 0;
  for (size_t i = 0; i < message.msg_hdr.msg_iovlen; ++i)
    payload += message.msg_hdr.msg_iov[i].iov_len;
  return payload + UdpWireOverheadBytes;
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

size_t videoDatagramCount(size_t payloadBytes) noexcept {
  return (payloadBytes + UdpPayloadBytes - 1) / UdpPayloadBytes;
}

uint64_t videoWireBytes(size_t payloadBytes) noexcept {
  if (!payloadBytes) return 0;
  return payloadBytes + videoDatagramCount(payloadBytes) * UdpWireOverheadBytes;
}

uint64_t pacingDurationNs(size_t payloadBytes,
                          uint64_t bitsPerSecond) noexcept {
  return durationForWireBytes(videoWireBytes(payloadBytes), bitsPerSecond);
}

uint64_t videoCompletionGraceNs(uint64_t framePeriodNs) noexcept {
  return std::clamp(framePeriodNs / 2, kMinimumGraceNs, kMaximumGraceNs);
}

bool submitVideoDatagrams(int fd, mmsghdr* messages, size_t count,
                          uint64_t framePeriodNs, UdpSubmitSyscalls& syscalls,
                          VideoSubmissionStats& stats, std::string& error) {
  stats = {};
  if (!count) return true;
  stats.paced = count > VideoPacingBatchDatagrams;
  uint64_t totalWireBytes = 0;
  for (size_t i = 0; i < count; ++i) totalWireBytes += messageWireBytes(messages[i]);
  stats.estimatedWireNs =
      durationForWireBytes(totalWireBytes, VideoPacingBitsPerSecond);
  const int64_t started = syscalls.monotonicNowNs();
  const int64_t completionDeadline =
      started + int64_t(stats.estimatedWireNs +
                        videoCompletionGraceNs(framePeriodNs));
  uint64_t releasedWireBytes = 0;
  size_t cursor = 0;
  while (cursor < count) {
    const size_t batchStart = cursor;
    const size_t batchEnd = std::min(count, cursor + VideoPacingBatchDatagrams);
    if (stats.paced && batchStart) {
      const int64_t releaseDeadline =
          started + int64_t(durationForWireBytes(releasedWireBytes,
                                                 VideoPacingBitsPerSecond));
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
        if (late > VideoPacingLateToleranceNs) ++stats.lateBatchReleases;
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
      error = std::string("video payload send failed: ") +
              std::strerror(result ? -result : EIO);
      return false;
    }
    for (size_t i = batchStart; i < batchEnd; ++i)
      releasedWireBytes += messageWireBytes(messages[i]);
    sampleQueue(fd, syscalls, stats);
  }
  const int64_t finished = syscalls.monotonicNowNs();
  if (finished > started) stats.wallSubmissionNs = uint64_t(finished - started);
  return true;
}

UdpSubmitSyscalls& systemUdpSubmitSyscalls() noexcept {
  static SystemUdpSubmitSyscalls syscalls;
  return syscalls;
}
}  // namespace mistercast
