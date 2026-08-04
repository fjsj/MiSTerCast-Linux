#pragma once

#include <arpa/inet.h>
#include <netinet/ip.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace mistercast::test {

// A Groovy_MiSTer endpoint bound to the real protocol port. StreamSession always
// dials 32100, so a session-level test needs the fake to own it; when the port is
// already taken, `bound()` is false and the test skips instead of failing.
//
// Unlike FakeGroovyEndpoint (arbitrary port, used for transport-level tests),
// this acknowledges every blit by echoing the requested frame and sync line back
// as both the echo and the current raster, which is what keeps a session's ACK
// tracking and adaptive timing progressing.
class FakeMister {
 public:
  explicit FakeMister(uint8_t statusBits) : statusBits_(statusBits) {
    fd_ = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd_ < 0) return;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(32100);
    if (bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) !=
        0) {
      close(fd_);
      fd_ = -1;
      return;
    }
    timeval timeout{0, 200000};
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    running_ = true;
    worker_ = std::thread([this] { serve(); });
  }

  ~FakeMister() {
    running_ = false;
    if (worker_.joinable()) worker_.join();
    if (fd_ >= 0) close(fd_);
  }

  FakeMister(const FakeMister&) = delete;
  FakeMister& operator=(const FakeMister&) = delete;

  bool bound() const noexcept { return fd_ >= 0; }

  // Flipping the core's audio bit mid-stream is how the session's
  // "core has audio off" path is reached without restarting.
  void setStatusBits(uint8_t bits) noexcept { statusBits_ = bits; }
  void setAcknowledge(bool value) noexcept { acknowledge_ = value; }

  uint32_t blits() const noexcept { return blits_; }
  uint32_t audioPackets() const noexcept { return audioPackets_; }
  uint64_t audioBytes() const noexcept { return audioBytes_; }
  uint32_t switchModes() const noexcept { return switchModes_; }
  uint32_t closes() const noexcept { return closes_; }
  uint8_t initRateCode() const noexcept { return initRate_; }
  uint8_t initChannelCode() const noexcept { return initChannels_; }
  uint16_t lastSyncLine() const noexcept { return lastSyncLine_; }
  uint8_t lastInterlaceMode() const noexcept { return lastInterlace_; }

  std::vector<uint8_t> fields() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return fields_;
  }

  std::vector<uint16_t> activeHeights() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return activeHeights_;
  }

 private:
  void serve() {
    uint8_t packet[2048];
    sockaddr_storage peer{};
    socklen_t peerSize = sizeof(peer);
    // Command payloads (audio PCM, video pixels) arrive as follow-up datagrams
    // that must not be mistaken for commands.
    size_t payloadRemaining = 0;
    while (running_) {
      const auto size = recvfrom(fd_, packet, sizeof(packet), 0,
                                 reinterpret_cast<sockaddr*>(&peer), &peerSize);
      if (size <= 0) continue;
      if (payloadRemaining) {
        payloadRemaining -= std::min(payloadRemaining, size_t(size));
        continue;
      }
      switch (packet[0]) {
        case 2:
          if (size != 5) break;
          initRate_ = packet[2];
          initChannels_ = packet[3];
          replyStatus(peer, peerSize, 0, 0, 0, 0);
          break;
        case 3: {
          if (size != 26) break;
          ++switchModes_;
          uint16_t vActive = 0;
          std::memcpy(&vActive, packet + 17, 2);
          lastInterlace_ = packet[25];
          std::lock_guard<std::mutex> lock(mutex_);
          activeHeights_.push_back(vActive);
          break;
        }
        case 4: {
          if (size != 3) break;
          ++audioPackets_;
          uint16_t bytes = 0;
          std::memcpy(&bytes, packet + 1, 2);
          audioBytes_ += bytes;
          payloadRemaining = bytes;
          break;
        }
        case 7: {
          if (size != 8 && size != 12) break;
          ++blits_;
          uint32_t frame = 0, compressed = 0;
          uint16_t line = 0;
          std::memcpy(&frame, packet + 1, 4);
          std::memcpy(&line, packet + 6, 2);
          if (size == 12) std::memcpy(&compressed, packet + 8, 4);
          lastSyncLine_ = line;
          {
            std::lock_guard<std::mutex> lock(mutex_);
            fields_.push_back(packet[5]);
          }
          payloadRemaining = compressed;
          if (acknowledge_) replyStatus(peer, peerSize, frame, line, frame, line);
          break;
        }
        case 1:
          ++closes_;
          break;
        default:
          break;
      }
    }
  }

  void replyStatus(const sockaddr_storage& peer, socklen_t peerSize,
                   uint32_t frameEcho, uint16_t lineEcho, uint32_t frame,
                   uint16_t line) {
    uint8_t status[13]{};
    std::memcpy(status, &frameEcho, 4);
    std::memcpy(status + 4, &lineEcho, 2);
    std::memcpy(status + 6, &frame, 4);
    std::memcpy(status + 10, &line, 2);
    status[12] = statusBits_;
    sendto(fd_, status, sizeof(status), 0,
           reinterpret_cast<const sockaddr*>(&peer), peerSize);
  }

  int fd_{-1};
  std::atomic<bool> running_{false}, acknowledge_{true};
  std::atomic<uint8_t> statusBits_;
  std::atomic<uint32_t> blits_{0}, audioPackets_{0}, switchModes_{0},
      closes_{0};
  std::atomic<uint64_t> audioBytes_{0};
  std::atomic<uint8_t> initRate_{0xff}, initChannels_{0xff}, lastInterlace_{
                                                                 0xff};
  std::atomic<uint16_t> lastSyncLine_{0};
  std::thread worker_;
  mutable std::mutex mutex_;
  std::vector<uint8_t> fields_;
  std::vector<uint16_t> activeHeights_;
};

}  // namespace mistercast::test
