#include <netinet/ip.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "mistercast/groovy_protocol.hpp"
#include "mistercast/udp_pacing.hpp"

using namespace mistercast;

namespace {
int failed = 0;
#define CHECK(x)                                                              \
  do {                                                                        \
    if (!(x)) {                                                               \
      std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #x "\n"; \
      ++failed;                                                               \
    }                                                                         \
  } while (0)

UdpVideoConfig groovyConfig(size_t payloadBytes = GroovyUdpPayloadBytes) {
  return {payloadBytes, GroovyFramebufferBytes,
          GroovyUdpWireOverheadBytes, 950000000, 32, 100000};
}

class FakeUdpSyscalls final : public UdpSubmitSyscalls {
 public:
  int64_t now{}, sleepLateness{};
  std::vector<int> sendResults, waitResults;
  std::vector<unsigned> requestedCounts;
  std::vector<int64_t> releaseDeadlines;
  std::vector<uint64_t> queueSamples;
  size_t sendIndex{}, waitIndex{}, queueIndex{};

  int64_t monotonicNowNs() noexcept override { return now; }
  int sleepUntil(int64_t deadlineNs) noexcept override {
    releaseDeadlines.push_back(deadlineNs);
    now = std::max(now, deadlineNs) + sleepLateness;
    return 0;
  }
  int sendMessages(int, mmsghdr*, unsigned count, int) noexcept override {
    requestedCounts.push_back(count);
    now += 1000;
    if (sendIndex >= sendResults.size()) return int(count);
    const int result = sendResults[sendIndex++];
    return result > int(count) ? int(count) : result;
  }
  int waitWritable(int, int64_t deadlineNs) noexcept override {
    if (waitIndex >= waitResults.size()) return 1;
    const int result = waitResults[waitIndex++];
    if (!result) now = deadlineNs;
    return result;
  }
  bool outputQueueBytes(int, uint64_t& bytes) noexcept override {
    if (queueIndex >= queueSamples.size()) return false;
    bytes = queueSamples[queueIndex++];
    return true;
  }
};

struct TestMessages {
  std::vector<uint8_t> bytes;
  std::vector<iovec> vectors;
  std::vector<mmsghdr> messages;

  TestMessages(size_t payloadBytes, const UdpVideoConfig& config)
      : bytes(payloadBytes) {
    const auto count = videoDatagramCount(payloadBytes, config);
    vectors.resize(count);
    messages.resize(count);
    for (size_t i = 0, offset = 0; i < count; ++i) {
      const size_t length =
          std::min(config.payloadBytes, payloadBytes - offset);
      vectors[i].iov_base = bytes.data() + offset;
      vectors[i].iov_len = length;
      messages[i].msg_hdr.msg_iov = &vectors[i];
      messages[i].msg_hdr.msg_iovlen = 1;
      offset += length;
    }
  }
};

void checkCalculationsAndMtu() {
  const auto config = groovyConfig();
  auto zeroRate = config;
  zeroRate.pacingBitsPerSecond = 0;
  CHECK(pacingDurationNs(1000, zeroRate) == 0);
  CHECK(videoDatagramCount(config.payloadBytes * 32, config) == 32);
  CHECK(videoDatagramCount(config.payloadBytes * 32 + 1, config) == 33);
  CHECK(videoWireBytes(config.payloadBytes + 1, config) ==
        config.payloadBytes + 1 + 2 * config.wireOverheadBytes);
  CHECK(videoDatagramCount(config.maximumFrameBytes, config) == 846);
  const auto duration = pacingDurationNs(470 * 1024, config);
  CHECK(duration > 4200000 && duration < 4300000);
  CHECK(videoCompletionGraceNs(1000000) == 5000000);
  CHECK(videoCompletionGraceNs(16666666) == 8333333);
  CHECK(videoCompletionGraceNs(10000000000) == 100000000);

  auto smallerPackets = groovyConfig(1200);
  CHECK(videoDatagramCount(2401, smallerPackets) == 3);
  CHECK(videoWireBytes(2401, smallerPackets) ==
        2401 + 3 * smallerPackets.wireOverheadBytes);

  std::string error;
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd >= 0) {
    CHECK(configureStrictPathMtu(fd, error));
    int mode = 0;
    socklen_t modeSize = sizeof(mode);
    CHECK(getsockopt(fd, IPPROTO_IP, IP_MTU_DISCOVER, &mode, &modeSize) == 0 &&
          mode == IP_PMTUDISC_DO);
    close(fd);
  } else {
    CHECK(errno == EPERM);
  }
  CHECK(!configureStrictPathMtu(-1, error) && !error.empty());
  CHECK(validatePathMtu(1500, config, error));
  CHECK(!validatePathMtu(1499, config, error) &&
        error.find("detected path MTU 1499") != std::string::npos &&
        error.find("VPN") != std::string::npos);
  CHECK(udpSendError(EMSGSIZE, "audio send", config)
            .find("MTU 1500 required") != std::string::npos);
}

void checkSubmissionState() {
  const auto config = groovyConfig();
  std::string error;
  VideoSubmissionStats stats;

  TestMessages boundary32(config.payloadBytes * 32, config);
  FakeUdpSyscalls full32;
  CHECK(submitVideoDatagrams(-1, boundary32.messages.data(),
                             boundary32.messages.size(), 16666666, config,
                             full32, stats, error));
  CHECK(!stats.paced && full32.releaseDeadlines.empty() &&
        stats.submittedDatagrams == 32);

  TestMessages boundary33(config.payloadBytes * 32 + 17, config);
  FakeUdpSyscalls full33;
  full33.sleepLateness = 150000;
  full33.queueSamples = {100, 250};
  CHECK(submitVideoDatagrams(-1, boundary33.messages.data(),
                             boundary33.messages.size(), 16666666, config,
                             full33, stats, error));
  CHECK(stats.paced && stats.submittedDatagrams == 33 &&
        full33.requestedCounts.size() == 2 &&
        full33.requestedCounts[0] == 32 && full33.requestedCounts[1] == 1);
  CHECK(full33.releaseDeadlines.size() == 1 &&
        full33.releaseDeadlines[0] > 0 && stats.lateBatchReleases == 1 &&
        stats.maxReleaseLatenessNs == 150000 &&
        stats.observedQueueHighWater == 250);

  TestMessages seventy(config.payloadBytes * 69 + 5, config);
  FakeUdpSyscalls monotonic;
  CHECK(submitVideoDatagrams(-1, seventy.messages.data(),
                             seventy.messages.size(), 16666666, config,
                             monotonic, stats, error));
  CHECK(monotonic.releaseDeadlines.size() == 2 &&
        monotonic.releaseDeadlines[1] > monotonic.releaseDeadlines[0] &&
        stats.estimatedWireNs ==
            pacingDurationNs(config.payloadBytes * 69 + 5, config));

  TestMessages ten(config.payloadBytes * 9 + 7, config);
  FakeUdpSyscalls partial;
  partial.sendResults = {3, 7};
  CHECK(submitVideoDatagrams(-1, ten.messages.data(), ten.messages.size(),
                             16666666, config, partial, stats, error));
  CHECK(partial.requestedCounts.size() == 2 &&
        partial.requestedCounts[0] == 10 && partial.requestedCounts[1] == 7 &&
        stats.submittedDatagrams == 10);

  FakeUdpSyscalls interruptedSend;
  interruptedSend.sendResults = {-EINTR, 100};
  CHECK(submitVideoDatagrams(-1, ten.messages.data(), ten.messages.size(),
                             16666666, config, interruptedSend, stats, error));
  CHECK(interruptedSend.requestedCounts.size() == 2 &&
        stats.submittedDatagrams == 10);

  FakeUdpSyscalls recovers;
  recovers.sendResults = {-EAGAIN, 100};
  recovers.waitResults = {1};
  CHECK(submitVideoDatagrams(-1, ten.messages.data(), ten.messages.size(),
                             16666666, config, recovers, stats, error));
  CHECK(stats.submittedDatagrams == 10);

  FakeUdpSyscalls timesOut;
  timesOut.sendResults = {-EAGAIN};
  timesOut.waitResults = {0};
  CHECK(!submitVideoDatagrams(-1, ten.messages.data(), ten.messages.size(),
                              10000000000, config, timesOut, stats, error));
  CHECK(error.find("deadline") != std::string::npos &&
        timesOut.now <= 100000000 + stats.estimatedWireNs);

  FakeUdpSyscalls hardError;
  hardError.sendResults = {-EIO};
  CHECK(!submitVideoDatagrams(-1, ten.messages.data(), ten.messages.size(),
                              16666666, config, hardError, stats, error));
  CHECK(error.find("video payload send failed") != std::string::npos &&
        stats.submittedDatagrams == 0);

  FakeUdpSyscalls mtuError;
  mtuError.sendResults = {-EMSGSIZE};
  CHECK(!submitVideoDatagrams(-1, ten.messages.data(), ten.messages.size(),
                              16666666, config, mtuError, stats, error));
  CHECK(error.find("path MTU") != std::string::npos);
}

void checkSenderPacketization() {
  auto config = groovyConfig(1200);
  FakeUdpSyscalls syscalls;
  UdpVideoSender sender(config, &syscalls);
  std::vector<uint8_t> payload(2401);
  VideoSubmissionStats submission;
  std::string error;
  CHECK(sender.submit(-1, payload.data(), payload.size(), 16666666,
                      submission, error));
  CHECK(syscalls.requestedCounts.size() == 1 &&
        syscalls.requestedCounts[0] == 3 &&
        submission.submittedDatagrams == 3);
}
}  // namespace

int main() {
  checkCalculationsAndMtu();
  checkSubmissionState();
  checkSenderPacketization();
  return failed ? 1 : 0;
}
