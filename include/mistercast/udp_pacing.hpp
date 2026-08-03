#pragma once

#include <sys/socket.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace mistercast {

struct UdpVideoConfig {
  // Shared datagram payload size for command-associated audio and video data.
  size_t payloadBytes{};
  // Largest complete video payload for which descriptors are preallocated.
  size_t maximumFrameBytes{};
  size_t wireOverheadBytes{};
  uint64_t pacingBitsPerSecond{950000000};
  size_t batchDatagrams{32};
  uint64_t lateToleranceNs{100000};
};

size_t videoDatagramCount(size_t payloadBytes,
                          const UdpVideoConfig& config) noexcept;
uint64_t videoWireBytes(size_t payloadBytes,
                        const UdpVideoConfig& config) noexcept;
uint64_t pacingDurationNs(size_t payloadBytes,
                          const UdpVideoConfig& config) noexcept;
uint64_t videoCompletionGraceNs(uint64_t framePeriodNs) noexcept;
bool configureStrictPathMtu(int fd, std::string& error) noexcept;
bool validatePathMtu(uint32_t pathMtu, const UdpVideoConfig& config,
                     std::string& error) noexcept;
std::string udpSendError(int errorNumber, const char* operation,
                         const UdpVideoConfig& config);

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
                          uint64_t framePeriodNs,
                          const UdpVideoConfig& config,
                          UdpSubmitSyscalls& syscalls,
                          VideoSubmissionStats& stats, std::string& error);

struct UdpVideoSenderStats {
  uint64_t pacedPayloads{}, pacedDatagrams{}, lateBatchReleases{},
      maxReleaseLatenessNs{}, observedQueueHighWater{};
};

class UdpVideoSender {
 public:
  explicit UdpVideoSender(UdpVideoConfig config,
                          UdpSubmitSyscalls* syscalls = nullptr);
  ~UdpVideoSender();
  UdpVideoSender(const UdpVideoSender&) = delete;
  UdpVideoSender& operator=(const UdpVideoSender&) = delete;

  const UdpVideoConfig& config() const noexcept;
  void reset() noexcept;
  void observeQueue(int fd) noexcept;
  bool submit(int fd, const uint8_t* payload, size_t payloadBytes,
              uint64_t framePeriodNs, VideoSubmissionStats& submission,
              std::string& error);
  UdpVideoSenderStats stats() const noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

UdpSubmitSyscalls& systemUdpSubmitSyscalls() noexcept;
}  // namespace mistercast
