#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/ip.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "mistercast/groovy_protocol.hpp"
#include "mistercast/udp_pacing.hpp"

using namespace mistercast;
using testing::ElementsAre;
using testing::HasSubstr;

namespace {

UdpVideoConfig groovyConfig(size_t payloadBytes = GroovyUdpPayloadBytes) {
  return {payloadBytes, GroovyFramebufferBytes, GroovyUdpWireOverheadBytes};
}

// A scripted stand-in for the four syscalls the submitter needs. This is the
// injection seam the production code already exposes, so pacing decisions can be
// checked without depending on real wall-clock timing or a real network.
class FakeUdpSyscalls final : public UdpSubmitSyscalls {
 public:
  int64_t now{}, sleepLateness{};
  int sleepError{0};
  std::vector<int> sendResults, waitResults;
  std::vector<unsigned> requestedCounts;
  std::vector<int64_t> releaseDeadlines;
  std::vector<uint64_t> queueSamples;
  size_t sendIndex{}, waitIndex{}, queueIndex{};

  int64_t monotonicNowNs() noexcept override { return now; }

  int sleepUntil(int64_t deadlineNs) noexcept override {
    releaseDeadlines.push_back(deadlineNs);
    if (sleepError) return sleepError;
    now = std::max(now, deadlineNs) + sleepLateness;
    return 0;
  }

  // Set to zero to model a clock whose resolution is coarser than the whole
  // submission, which is what a low-resolution CLOCK_MONOTONIC looks like.
  int64_t sendDurationNs{1000};

  int sendMessages(int, mmsghdr*, unsigned count, int) noexcept override {
    requestedCounts.push_back(count);
    now += sendDurationNs;
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

// Builds the scatter/gather descriptors the submitter consumes, the same way
// UdpVideoSender does internally.
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
      const size_t length = std::min(config.payloadBytes, payloadBytes - offset);
      vectors[i].iov_base = bytes.data() + offset;
      vectors[i].iov_len = length;
      messages[i].msg_hdr.msg_iov = &vectors[i];
      messages[i].msg_hdr.msg_iovlen = 1;
      offset += length;
    }
  }
};

// ------------------------------------------------------------ plain arithmetic

TEST(VideoDatagramCount, SplitsOnExactPayloadBoundaries) {
  const auto config = groovyConfig();
  const auto payload = config.payloadBytes;
  EXPECT_EQ(videoDatagramCount(0, config), 0u);
  EXPECT_EQ(videoDatagramCount(1, config), 1u);
  EXPECT_EQ(videoDatagramCount(payload, config), 1u);
  EXPECT_EQ(videoDatagramCount(payload + 1, config), 2u);
  EXPECT_EQ(videoDatagramCount(payload * 32, config), 32u);
  EXPECT_EQ(videoDatagramCount(payload * 32 + 1, config), 33u);
  EXPECT_EQ(videoDatagramCount(config.maximumFrameBytes, config), 846u);
}

TEST(VideoDatagramCount, AZeroPayloadSizeCannotSplitAnything) {
  auto config = groovyConfig();
  config.payloadBytes = 0;
  EXPECT_EQ(videoDatagramCount(1000, config), 0u);
}

TEST(VideoWireBytes, AddsPerDatagramOverhead) {
  const auto config = groovyConfig();
  EXPECT_EQ(videoWireBytes(0, config), 0u);
  EXPECT_EQ(videoWireBytes(config.payloadBytes + 1, config),
            config.payloadBytes + 1 + 2 * config.wireOverheadBytes);

  const auto smaller = groovyConfig(1200);
  EXPECT_EQ(videoDatagramCount(2401, smaller), 3u);
  EXPECT_EQ(videoWireBytes(2401, smaller),
            2401 + 3 * smaller.wireOverheadBytes);
}

TEST(PacingDuration, ScalesWithWireBytesAndIsZeroWithoutARate) {
  const auto config = groovyConfig();
  const auto duration = pacingDurationNs(470 * 1024, config);
  EXPECT_GT(duration, 4200000u);
  EXPECT_LT(duration, 4300000u);
  EXPECT_GT(pacingDurationNs(940 * 1024, config), duration);

  auto zeroRate = config;
  zeroRate.pacingBitsPerSecond = 0;
  EXPECT_EQ(pacingDurationNs(1000, zeroRate), 0u);
}

TEST(VideoCompletionGrace, IsHalfAFramePeriodClampedToFiveToOneHundredMs) {
  EXPECT_EQ(videoCompletionGraceNs(1000000), 5000000u);
  EXPECT_EQ(videoCompletionGraceNs(16666666), 8333333u);
  EXPECT_EQ(videoCompletionGraceNs(10000000000ull), 100000000u);
}

TEST(Ipv4Mtu, IsThePayloadPlusTheIpv4AndUdpHeaders) {
  EXPECT_EQ(groovyConfig().ipv4MtuBytes(), 1500u);
  EXPECT_EQ(groovyConfig(1200).ipv4MtuBytes(), 1228u);
}

TEST(ValidatePathMtu, AcceptsExactlyTheRequiredMtu) {
  const auto config = groovyConfig();
  std::string error;
  EXPECT_TRUE(validatePathMtu(1500, config, error));
  EXPECT_TRUE(validatePathMtu(9000, config, error));
  EXPECT_THAT(error, testing::IsEmpty());
}

TEST(ValidatePathMtu, ExplainsATooSmallPathAndPointsAtTunnels) {
  const auto config = groovyConfig();
  std::string error;
  EXPECT_FALSE(validatePathMtu(1499, config, error));
  EXPECT_THAT(error, HasSubstr("detected path MTU 1499"));
  EXPECT_THAT(error, HasSubstr("MTU 1500"));
  EXPECT_THAT(error, HasSubstr("VPN"));

  const auto smaller = groovyConfig(1200);
  EXPECT_FALSE(validatePathMtu(1227, smaller, error));
  EXPECT_THAT(error, HasSubstr("MTU 1228"));
  EXPECT_THAT(error, HasSubstr("1200-byte UDP payloads"));
}

TEST(UdpSendError, SpellsOutFragmentationSeparatelyFromOtherFailures) {
  const auto config = groovyConfig();
  EXPECT_THAT(udpSendError(EMSGSIZE, "audio send", config),
              testing::AllOf(HasSubstr("audio send failed"),
                             HasSubstr("MTU 1500 required"),
                             HasSubstr("fragmentation")));
  EXPECT_THAT(udpSendError(EMSGSIZE, "video send", groovyConfig(1200)),
              HasSubstr("MTU 1228 required"));
  EXPECT_THAT(udpSendError(ECONNREFUSED, "UDP send", config),
              testing::AllOf(HasSubstr("UDP send failed"),
                             HasSubstr(std::strerror(ECONNREFUSED))));
}

// ---------------------------------------------------------- submission control

TEST(SubmitVideoDatagrams, NothingToSendSucceedsImmediately) {
  const auto config = groovyConfig();
  FakeUdpSyscalls syscalls;
  VideoSubmissionStats stats;
  std::string error;
  EXPECT_TRUE(submitVideoDatagrams(-1, nullptr, 0, 16666666, config, syscalls,
                                   stats, error));
  EXPECT_THAT(syscalls.requestedCounts, testing::IsEmpty());
  EXPECT_EQ(stats.submittedDatagrams, 0u);
}

TEST(SubmitVideoDatagrams, RejectsAnUnusableConfiguration) {
  TestMessages messages(100, groovyConfig());
  FakeUdpSyscalls syscalls;
  VideoSubmissionStats stats;
  std::string error;
  for (auto broken : {[] {
                        auto config = groovyConfig();
                        config.batchDatagrams = 0;
                        return config;
                      }(),
                      [] {
                        auto config = groovyConfig();
                        config.pacingBitsPerSecond = 0;
                        return config;
                      }()}) {
    EXPECT_FALSE(submitVideoDatagrams(-1, messages.messages.data(),
                                      messages.messages.size(), 16666666,
                                      broken, syscalls, stats, error));
    EXPECT_EQ(error, "invalid UDP video pacing configuration");
  }
}

TEST(SubmitVideoDatagrams, OneBatchIsSentUnpacedWithNoReleaseSleep) {
  const auto config = groovyConfig();
  TestMessages messages(config.payloadBytes * 32, config);
  FakeUdpSyscalls syscalls;
  VideoSubmissionStats stats;
  std::string error;
  ASSERT_TRUE(submitVideoDatagrams(-1, messages.messages.data(),
                                   messages.messages.size(), 16666666, config,
                                   syscalls, stats, error))
      << error;
  EXPECT_FALSE(stats.paced) << "exactly one batch needs no pacing";
  EXPECT_THAT(syscalls.releaseDeadlines, testing::IsEmpty());
  EXPECT_EQ(stats.submittedDatagrams, 32u);
  EXPECT_GT(stats.activeSubmissionNs, 0u);
  EXPECT_GT(stats.wallSubmissionNs, 0u);
}

TEST(SubmitVideoDatagrams, MoreThanOneBatchIsPacedAndLatenessIsRecorded) {
  const auto config = groovyConfig();
  TestMessages messages(config.payloadBytes * 32 + 17, config);
  FakeUdpSyscalls syscalls;
  syscalls.sleepLateness = 150000;  // beyond the 100 us tolerance
  syscalls.queueSamples = {100, 250};
  VideoSubmissionStats stats;
  std::string error;
  ASSERT_TRUE(submitVideoDatagrams(-1, messages.messages.data(),
                                   messages.messages.size(), 16666666, config,
                                   syscalls, stats, error))
      << error;
  EXPECT_TRUE(stats.paced);
  EXPECT_EQ(stats.submittedDatagrams, 33u);
  EXPECT_THAT(syscalls.requestedCounts, ElementsAre(32u, 1u));
  ASSERT_EQ(syscalls.releaseDeadlines.size(), 1u);
  EXPECT_GT(syscalls.releaseDeadlines[0], 0);
  EXPECT_EQ(stats.lateBatchReleases, 1u);
  EXPECT_EQ(stats.maxReleaseLatenessNs, 150000u);
  EXPECT_EQ(stats.observedQueueHighWater, 250u)
      << "the high-water mark keeps the largest sample, not the last";
}

TEST(SubmitVideoDatagrams, LatenessInsideToleranceIsNotCountedAsLate) {
  const auto config = groovyConfig();
  TestMessages messages(config.payloadBytes * 33, config);
  FakeUdpSyscalls syscalls;
  syscalls.sleepLateness = 50000;  // within the 100 us tolerance
  VideoSubmissionStats stats;
  std::string error;
  ASSERT_TRUE(submitVideoDatagrams(-1, messages.messages.data(),
                                   messages.messages.size(), 16666666, config,
                                   syscalls, stats, error));
  EXPECT_EQ(stats.lateBatchReleases, 0u);
  EXPECT_EQ(stats.maxReleaseLatenessNs, 50000u);
}

TEST(SubmitVideoDatagrams, ReleaseDeadlinesAdvanceWithBytesAlreadyReleased) {
  const auto config = groovyConfig();
  const size_t payload = config.payloadBytes * 69 + 5;
  TestMessages messages(payload, config);
  FakeUdpSyscalls syscalls;
  VideoSubmissionStats stats;
  std::string error;
  ASSERT_TRUE(submitVideoDatagrams(-1, messages.messages.data(),
                                   messages.messages.size(), 16666666, config,
                                   syscalls, stats, error));
  ASSERT_EQ(syscalls.releaseDeadlines.size(), 2u);
  EXPECT_GT(syscalls.releaseDeadlines[1], syscalls.releaseDeadlines[0]);
  EXPECT_EQ(stats.estimatedWireNs, pacingDurationNs(payload, config));
}

TEST(SubmitVideoDatagrams, ResumesFromWhereAPartialSendmmsgStopped) {
  const auto config = groovyConfig();
  TestMessages messages(config.payloadBytes * 9 + 7, config);
  FakeUdpSyscalls syscalls;
  syscalls.sendResults = {3, 7};
  VideoSubmissionStats stats;
  std::string error;
  ASSERT_TRUE(submitVideoDatagrams(-1, messages.messages.data(),
                                   messages.messages.size(), 16666666, config,
                                   syscalls, stats, error));
  EXPECT_THAT(syscalls.requestedCounts, ElementsAre(10u, 7u));
  EXPECT_EQ(stats.submittedDatagrams, 10u);
}

TEST(SubmitVideoDatagrams, AClockThatDoesNotAdvanceReportsNoElapsedTime) {
  const auto config = groovyConfig();
  TestMessages messages(config.payloadBytes, config);
  FakeUdpSyscalls syscalls;
  syscalls.sendDurationNs = 0;
  VideoSubmissionStats stats;
  std::string error;
  ASSERT_TRUE(submitVideoDatagrams(-1, messages.messages.data(),
                                   messages.messages.size(), 16666666, config,
                                   syscalls, stats, error));
  EXPECT_EQ(stats.activeSubmissionNs, 0u);
  EXPECT_EQ(stats.wallSubmissionNs, 0u)
      << "a duration must never be inferred from a clock that stood still";
  EXPECT_EQ(stats.submittedDatagrams, 1u);
}

TEST(SubmitVideoDatagrams, RetriesAnInterruptedSend) {
  const auto config = groovyConfig();
  TestMessages messages(config.payloadBytes * 9 + 7, config);
  FakeUdpSyscalls syscalls;
  syscalls.sendResults = {-EINTR, 100};
  VideoSubmissionStats stats;
  std::string error;
  ASSERT_TRUE(submitVideoDatagrams(-1, messages.messages.data(),
                                   messages.messages.size(), 16666666, config,
                                   syscalls, stats, error));
  EXPECT_EQ(syscalls.requestedCounts.size(), 2u);
  EXPECT_EQ(stats.submittedDatagrams, 10u);
}

TEST(SubmitVideoDatagrams, WaitsForWritabilityAfterBackpressure) {
  const auto config = groovyConfig();
  TestMessages messages(config.payloadBytes * 9 + 7, config);
  VideoSubmissionStats stats;
  std::string error;
  for (int blocked : {-EAGAIN, -EWOULDBLOCK}) {
    FakeUdpSyscalls syscalls;
    syscalls.sendResults = {blocked, 100};
    syscalls.waitResults = {1};
    ASSERT_TRUE(submitVideoDatagrams(-1, messages.messages.data(),
                                     messages.messages.size(), 16666666, config,
                                     syscalls, stats, error))
        << error;
    EXPECT_EQ(stats.submittedDatagrams, 10u);
  }
}

TEST(SubmitVideoDatagrams, FailsWhenTheCompletionDeadlineExpires) {
  const auto config = groovyConfig();
  TestMessages messages(config.payloadBytes * 9 + 7, config);
  FakeUdpSyscalls syscalls;
  syscalls.sendResults = {-EAGAIN};
  syscalls.waitResults = {0};
  VideoSubmissionStats stats;
  std::string error;
  EXPECT_FALSE(submitVideoDatagrams(-1, messages.messages.data(),
                                    messages.messages.size(), 10000000000ull,
                                    config, syscalls, stats, error));
  EXPECT_THAT(error, HasSubstr("deadline expired"));
  // The grace period is capped at 100 ms however long the frame period is.
  EXPECT_LE(syscalls.now, int64_t(100000000 + stats.estimatedWireNs));
}

TEST(SubmitVideoDatagrams, ReportsAFailedWritabilityWait) {
  const auto config = groovyConfig();
  TestMessages messages(config.payloadBytes * 9 + 7, config);
  FakeUdpSyscalls syscalls;
  syscalls.sendResults = {-EAGAIN};
  syscalls.waitResults = {-EBADF};
  VideoSubmissionStats stats;
  std::string error;
  EXPECT_FALSE(submitVideoDatagrams(-1, messages.messages.data(),
                                    messages.messages.size(), 16666666, config,
                                    syscalls, stats, error));
  EXPECT_THAT(error, HasSubstr("video payload poll failed"));
}

TEST(SubmitVideoDatagrams, ReportsAFailedPacingSleep) {
  const auto config = groovyConfig();
  TestMessages messages(config.payloadBytes * 33, config);
  FakeUdpSyscalls syscalls;
  syscalls.sleepError = EINVAL;
  VideoSubmissionStats stats;
  std::string error;
  EXPECT_FALSE(submitVideoDatagrams(-1, messages.messages.data(),
                                    messages.messages.size(), 16666666, config,
                                    syscalls, stats, error));
  EXPECT_THAT(error, HasSubstr("video pacing sleep failed"));
}

TEST(SubmitVideoDatagrams, ReportsAHardSendFailure) {
  const auto config = groovyConfig();
  TestMessages messages(config.payloadBytes * 9 + 7, config);
  FakeUdpSyscalls syscalls;
  syscalls.sendResults = {-EIO};
  VideoSubmissionStats stats;
  std::string error;
  EXPECT_FALSE(submitVideoDatagrams(-1, messages.messages.data(),
                                    messages.messages.size(), 16666666, config,
                                    syscalls, stats, error));
  EXPECT_THAT(error, HasSubstr("video payload send failed"));
  EXPECT_EQ(stats.submittedDatagrams, 0u);
}

TEST(SubmitVideoDatagrams, ReportsFragmentationAsAnMtuProblem) {
  const auto config = groovyConfig();
  TestMessages messages(config.payloadBytes * 9 + 7, config);
  FakeUdpSyscalls syscalls;
  syscalls.sendResults = {-EMSGSIZE};
  VideoSubmissionStats stats;
  std::string error;
  EXPECT_FALSE(submitVideoDatagrams(-1, messages.messages.data(),
                                    messages.messages.size(), 16666666, config,
                                    syscalls, stats, error));
  EXPECT_THAT(error, HasSubstr("path MTU"));
}

TEST(SubmitVideoDatagrams, AZeroReturnFromSendIsTreatedAsAnIoFailure) {
  const auto config = groovyConfig();
  TestMessages messages(config.payloadBytes * 9 + 7, config);
  FakeUdpSyscalls syscalls;
  syscalls.sendResults = {0};
  VideoSubmissionStats stats;
  std::string error;
  EXPECT_FALSE(submitVideoDatagrams(-1, messages.messages.data(),
                                    messages.messages.size(), 16666666, config,
                                    syscalls, stats, error));
  EXPECT_THAT(error, HasSubstr("video payload send failed"));
}

// --------------------------------------------------------------- UdpVideoSender

TEST(UdpVideoSender, RefusesToBeBuiltFromAnUnusableConfiguration) {
  for (auto broken : {[] {
                        auto config = groovyConfig();
                        config.payloadBytes = 0;
                        return config;
                      }(),
                      [] {
                        auto config = groovyConfig();
                        config.maximumFrameBytes = 0;
                        return config;
                      }(),
                      [] {
                        auto config = groovyConfig();
                        config.batchDatagrams = 0;
                        return config;
                      }(),
                      [] {
                        auto config = groovyConfig();
                        config.pacingBitsPerSecond = 0;
                        return config;
                      }()})
    EXPECT_THROW({ UdpVideoSender sender(broken); },
                 std::invalid_argument);
}

TEST(UdpVideoSender, SplitsAPayloadIntoOwnDescriptorsAndKeepsTheConfiguration) {
  const auto config = groovyConfig(1200);
  FakeUdpSyscalls syscalls;
  UdpVideoSender sender(config, &syscalls);
  EXPECT_EQ(sender.config().payloadBytes, 1200u);

  std::vector<uint8_t> payload(2401);
  VideoSubmissionStats submission;
  std::string error;
  ASSERT_TRUE(sender.submit(-1, payload.data(), payload.size(), 16666666,
                            submission, error))
      << error;
  EXPECT_THAT(syscalls.requestedCounts, ElementsAre(3u));
  EXPECT_EQ(submission.submittedDatagrams, 3u);
}

TEST(UdpVideoSender, RefusesAPayloadLargerThanItsCapacity) {
  auto config = groovyConfig(1200);
  config.maximumFrameBytes = 2400;
  FakeUdpSyscalls syscalls;
  UdpVideoSender sender(config, &syscalls);
  std::vector<uint8_t> payload(2401);
  VideoSubmissionStats submission;
  std::string error;
  EXPECT_FALSE(sender.submit(-1, payload.data(), payload.size(), 16666666,
                             submission, error));
  EXPECT_EQ(error, "video payload exceeds configured UDP sender capacity");
}

TEST(UdpVideoSender, AccumulatesPacedStatisticsAcrossPayloads) {
  auto config = groovyConfig(100);
  config.maximumFrameBytes = 100000;
  config.batchDatagrams = 4;
  FakeUdpSyscalls syscalls;
  syscalls.sleepLateness = 200000;
  UdpVideoSender sender(config, &syscalls);

  std::vector<uint8_t> small(100), large(1000);
  VideoSubmissionStats submission;
  std::string error;
  ASSERT_TRUE(
      sender.submit(-1, small.data(), small.size(), 16666666, submission, error));
  EXPECT_FALSE(submission.paced);
  auto stats = sender.stats();
  EXPECT_EQ(stats.pacedPayloads, 0u);

  for (int i = 0; i < 2; ++i)
    ASSERT_TRUE(sender.submit(-1, large.data(), large.size(), 16666666,
                              submission, error));
  EXPECT_TRUE(submission.paced);
  stats = sender.stats();
  EXPECT_EQ(stats.pacedPayloads, 2u);
  EXPECT_EQ(stats.pacedDatagrams, 20u);
  EXPECT_GT(stats.lateBatchReleases, 0u);
  // Each release wakes 200 us late and the clock never catches up, so later
  // batches are later still; the high-water mark keeps the worst of them.
  EXPECT_GE(stats.maxReleaseLatenessNs, 200000u);

  sender.reset();
  stats = sender.stats();
  EXPECT_EQ(stats.pacedPayloads, 0u);
  EXPECT_EQ(stats.pacedDatagrams, 0u);
  EXPECT_EQ(stats.lateBatchReleases, 0u);
  EXPECT_EQ(stats.maxReleaseLatenessNs, 0u);
}

TEST(UdpVideoSender, ObserveQueueRecordsTheHighWaterMarkAndToleratesFailure) {
  FakeUdpSyscalls syscalls;
  syscalls.queueSamples = {4096, 512};
  UdpVideoSender sender(groovyConfig(), &syscalls);
  sender.observeQueue(-1);
  EXPECT_EQ(sender.stats().observedQueueHighWater, 4096u);
  sender.observeQueue(-1);
  EXPECT_EQ(sender.stats().observedQueueHighWater, 4096u);
  sender.observeQueue(-1);  // no samples left: the query fails and is ignored
  EXPECT_EQ(sender.stats().observedQueueHighWater, 4096u);
}

// ------------------------------------------------------ the real syscall bridge

// Everything above injects the syscalls; this exercises the default
// implementation against a real connected UDP socket so its clock, sleep, send,
// poll and queue-length calls are covered too.
TEST(SystemUdpSubmitSyscalls, DeliversAPacedPayloadOverALoopbackSocket) {
  const int receiver = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  ASSERT_GE(receiver, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_EQ(
      ::bind(receiver, reinterpret_cast<sockaddr*>(&address), sizeof(address)),
      0);
  socklen_t size = sizeof(address);
  ASSERT_EQ(
      ::getsockname(receiver, reinterpret_cast<sockaddr*>(&address), &size), 0);
  int receiveBuffer = 4 * 1024 * 1024;
  ::setsockopt(receiver, SOL_SOCKET, SO_RCVBUF, &receiveBuffer,
               sizeof(receiveBuffer));

  const int sender = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  ASSERT_GE(sender, 0);
  ASSERT_EQ(
      ::connect(sender, reinterpret_cast<sockaddr*>(&address), sizeof(address)),
      0);

  auto& syscalls = systemUdpSubmitSyscalls();
  const auto before = syscalls.monotonicNowNs();
  EXPECT_GT(before, 0);
  EXPECT_EQ(syscalls.sleepUntil(before + 1000000), 0);
  EXPECT_GE(syscalls.monotonicNowNs(), before + 1000000);
  uint64_t queued = 0;
  EXPECT_TRUE(syscalls.outputQueueBytes(sender, queued));
  EXPECT_EQ(syscalls.waitWritable(sender, syscalls.monotonicNowNs() + 1000000),
            1);

  // Enough datagrams to require more than one batch, so the pacing path runs
  // against the real clock and the real sendmmsg.
  const auto config = groovyConfig();
  const size_t payloadBytes = config.payloadBytes * 40;
  std::vector<uint8_t> payload(payloadBytes, 0xa5);
  UdpVideoSender videoSender(config, &syscalls);
  VideoSubmissionStats submission;
  std::string error;
  ASSERT_TRUE(videoSender.submit(sender, payload.data(), payloadBytes, 16666666,
                                 submission, error))
      << error;
  EXPECT_TRUE(submission.paced);
  EXPECT_EQ(submission.submittedDatagrams, 40u);
  EXPECT_GT(submission.wallSubmissionNs, 0u);
  videoSender.observeQueue(sender);

  size_t received = 0, receivedBytes = 0;
  std::vector<uint8_t> datagram(2048);
  timeval timeout{0, 200000};
  ::setsockopt(receiver, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  for (;;) {
    const auto bytes = ::recv(receiver, datagram.data(), datagram.size(), 0);
    if (bytes <= 0) break;
    ++received;
    receivedBytes += size_t(bytes);
  }
  // Loopback should not drop, but the assertion that matters is that nothing
  // arrived oversized or reordered into a short datagram before the end.
  EXPECT_EQ(received, 40u);
  EXPECT_EQ(receivedBytes, payloadBytes);

  ::close(sender);
  ::close(receiver);
}

TEST(SystemUdpSubmitSyscalls, AnExpiredWritabilityDeadlineReturnsImmediately) {
  const int socket = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  ASSERT_GE(socket, 0);
  auto& syscalls = systemUdpSubmitSyscalls();
  EXPECT_EQ(syscalls.waitWritable(socket, syscalls.monotonicNowNs() - 1), 0)
      << "a deadline in the past must not poll at all";
  ::close(socket);
}

TEST(SystemUdpSubmitSyscalls, ReportsErrnoFromAnUnusableDescriptor) {
  auto& syscalls = systemUdpSubmitSyscalls();
  uint64_t queued = 123;
  EXPECT_FALSE(syscalls.outputQueueBytes(-1, queued));
  EXPECT_EQ(queued, 123u) << "a failed query must not overwrite the caller's value";

  uint8_t byte = 0;
  iovec vector{&byte, 1};
  mmsghdr message{};
  message.msg_hdr.msg_iov = &vector;
  message.msg_hdr.msg_iovlen = 1;
  EXPECT_EQ(syscalls.sendMessages(-1, &message, 1, 0), -EBADF);
}

}  // namespace
