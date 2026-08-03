#pragma once

#include <sys/socket.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "mistercast/types.hpp"

namespace mistercast {
inline constexpr size_t UdpPayloadBytes = 1472;
inline constexpr size_t UdpWireOverheadBytes = 66;
inline constexpr uint64_t VideoPacingBitsPerSecond = 950000000;
inline constexpr size_t VideoPacingBatchDatagrams = 32;
inline constexpr uint64_t VideoPacingLateToleranceNs = 100000;
inline constexpr size_t MaxVideoDatagrams =
    (ProtocolFramebufferBytes + UdpPayloadBytes - 1) / UdpPayloadBytes;

size_t videoDatagramCount(size_t payloadBytes) noexcept;
uint64_t videoWireBytes(size_t payloadBytes) noexcept;
uint64_t pacingDurationNs(size_t payloadBytes, uint64_t bitsPerSecond) noexcept;
uint64_t videoCompletionGraceNs(uint64_t framePeriodNs) noexcept;
bool configureStrictPathMtu(int fd, std::string& error) noexcept;
bool validatePathMtu(uint32_t pathMtu, std::string& error) noexcept;
std::string udpSendError(int errorNumber, const char* operation);

class UdpSubmitSyscalls {
 public:
  virtual ~UdpSubmitSyscalls() = default;
  virtual int64_t monotonicNowNs() noexcept = 0;
  // Returns zero on success or a positive errno value.
  virtual int sleepUntil(int64_t deadlineNs) noexcept = 0;
  // Returns a message count or a negative errno value.
  virtual int sendMessages(int fd, mmsghdr* messages, unsigned count,
                           int flags) noexcept = 0;
  // Returns one when writable, zero at the deadline, or a negative errno.
  virtual int waitWritable(int fd, int64_t deadlineNs) noexcept = 0;
  virtual bool outputQueueBytes(int fd, uint64_t& bytes) noexcept = 0;
};

struct VideoSubmissionStats {
  uint64_t activeSubmissionNs{}, wallSubmissionNs{}, estimatedWireNs{},
      maxReleaseLatenessNs{}, observedQueueHighWater{};
  size_t submittedDatagrams{};
  uint64_t lateBatchReleases{};
  bool paced{};
};

bool submitVideoDatagrams(int fd, mmsghdr* messages, size_t count,
                          uint64_t framePeriodNs, UdpSubmitSyscalls& syscalls,
                          VideoSubmissionStats& stats, std::string& error);
UdpSubmitSyscalls& systemUdpSubmitSyscalls() noexcept;
}  // namespace mistercast
