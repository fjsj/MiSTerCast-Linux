#pragma once

#include <arpa/inet.h>
#include <netinet/ip.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "support/fake_groovy_endpoint.hpp"
#include "support/groovy_wire.hpp"

namespace mistercast::test {

// A Groovy_MiSTer endpoint bound to the real protocol port. StreamSession always
// dials 32100, so a session-level test needs the fake to own it; when the port is
// already taken, `bound()` is false and the test skips instead of failing.
//
// It acknowledges every blit by echoing the requested frame and sync line back as
// both the echo and the current raster, which is what keeps a session's ACK
// tracking and adaptive timing progressing, and counts the traffic so a test can
// assert on it afterwards.
//
// This keeps its own receive loop rather than reusing FakeGroovyEndpoint, which
// exists to record traffic for the transport tests to inspect. The suites that
// use this fake stream real full-resolution frames — roughly 850 datagrams per
// frame at 60 Hz — and a per-datagram vector, let alone keeping them all, makes
// the fake the bottleneck: the receive queue overflows, acknowledgements are lost
// and the session under test reports errors that nothing in production caused.
// Measured: reusing the recording endpoint made the GUI suite fail about one run
// in eight. So each command is decoded in place out of the stack buffer, and the
// two share the wire vocabulary and the acknowledgement encoder instead.
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

  // Assert on DELTAS across the step under test, not on absolute totals. These
  // counters can over-report: an uncompressed blit's 8-byte header carries no
  // payload length, so the pixel datagrams that follow it cannot be skipped, and
  // any one of them that happens to begin with a command opcode at that
  // command's exact size is counted as a command. Compressed blits carry the
  // length and are skipped correctly, which is the usual case with liblz4 built
  // in, but nothing in the protocol guarantees it.
  uint32_t blits() const noexcept { return blits_; }
  uint32_t audioPackets() const noexcept { return audioPackets_; }
  uint64_t audioBytes() const noexcept { return audioBytes_; }
  uint32_t switchModes() const noexcept { return switchModes_; }
  uint32_t closes() const noexcept { return closes_; }
  uint8_t initRateCode() const noexcept { return initRate_; }
  uint8_t initChannelCode() const noexcept { return initChannels_; }
  uint16_t lastSyncLine() const noexcept { return lastSyncLine_; }
  uint8_t lastInterlaceMode() const noexcept { return lastInterlace_; }

  // Mode switches counted before the first blit — the one switch count that is
  // exact in every build. Video payload is the only traffic this fake can
  // misread, and it can never precede the blit header it belongs to, so nothing
  // spurious is counted this early. Use it to assert "the session switched mode
  // exactly once before streaming" without depending on liblz4 being present to
  // make blits carry their payload length.
  uint32_t switchModesBeforeFirstBlit() const noexcept {
    return switchModesBeforeFirstBlit_;
  }

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
    // that carry no opcode and must not be decoded as commands.
    size_t payloadRemaining = 0;
    while (running_) {
      const auto received = recvfrom(fd_, packet, sizeof(packet), 0,
                                     reinterpret_cast<sockaddr*>(&peer),
                                     &peerSize);
      if (received <= 0) continue;
      const auto size = size_t(received);
      if (payloadRemaining) {
        payloadRemaining -= std::min(payloadRemaining, size);
        continue;
      }
      const auto opcode = packet[0];
      if (isGroovyCommand(opcode, size, kInit)) {
        initRate_ = packet[2];
        initChannels_ = packet[3];
        reply(peer, peerSize, {0, 0, 0, 0, statusBits_});
      } else if (isGroovyCommand(opcode, size, kSwitchMode)) {
        ++switchModes_;
        lastInterlace_ = packet[25];
        std::lock_guard<std::mutex> lock(mutex_);
        activeHeights_.push_back(readWire<uint16_t>(packet, size, 17));
      } else if (isGroovyCommand(opcode, size, kAudio)) {
        ++audioPackets_;
        const auto bytes = readWire<uint16_t>(packet, size, 1);
        audioBytes_ += bytes;
        payloadRemaining = bytes;
      } else if (isGroovyCommand(opcode, size, kBlit)) {
        if (!blits_) switchModesBeforeFirstBlit_ = switchModes_.load();
        ++blits_;
        const auto frame = readWire<uint32_t>(packet, size, 1);
        const auto line = readWire<uint16_t>(packet, size, 6);
        lastSyncLine_ = line;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          fields_.push_back(packet[5]);
        }
        payloadRemaining = size == kBlitCompressedBytes
                               ? readWire<uint32_t>(packet, size, 8)
                               : 0;
        if (acknowledge_)
          reply(peer, peerSize, {frame, line, frame, line, statusBits_});
      } else if (isGroovyCommand(opcode, size, kClose)) {
        ++closes_;
      }
    }
  }

  // GroovyAck owns the 13-byte acknowledgement layout for both fakes.
  void reply(const sockaddr_storage& peer, socklen_t peerSize,
             const GroovyAck& ack) const {
    const auto bytes = ack.encode();
    sendto(fd_, bytes.data(), bytes.size(), 0,
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
  std::atomic<uint32_t> switchModesBeforeFirstBlit_{0};
  std::thread worker_;
  mutable std::mutex mutex_;
  std::vector<uint8_t> fields_;
  std::vector<uint16_t> activeHeights_;
};

}  // namespace mistercast::test
