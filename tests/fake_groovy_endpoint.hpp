#pragma once

#include <sys/socket.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

namespace mistercast::test {

struct GroovyAck {
  uint32_t frameEcho{};
  uint16_t vCountEcho{};
  uint32_t fpgaFrame{};
  uint16_t fpgaVCount{};
  uint8_t statusBits{};

  std::array<uint8_t, 13> encode() const noexcept;
};

class FakeGroovyEndpoint {
 public:
  using Packet = std::vector<uint8_t>;
  using Handler = std::function<void(FakeGroovyEndpoint&, const Packet&)>;

  explicit FakeGroovyEndpoint(Handler handler, uint16_t port = 0);
  ~FakeGroovyEndpoint();
  FakeGroovyEndpoint(const FakeGroovyEndpoint&) = delete;
  FakeGroovyEndpoint& operator=(const FakeGroovyEndpoint&) = delete;

  bool valid() const noexcept { return fd_ >= 0; }
  uint16_t port() const noexcept { return port_; }
  void reply(const void* data, size_t size) noexcept;
  void replyVersion(uint8_t version = 1) noexcept;
  void replyAck(const GroovyAck& ack) noexcept;
  void stop() noexcept;

 private:
  void serve();

  Handler handler_;
  std::atomic<bool> running_{false};
  int fd_{-1};
  uint16_t port_{};
  sockaddr_storage peer_{};
  socklen_t peerSize_{};
  std::thread worker_;
};

uint32_t packetU32(const FakeGroovyEndpoint::Packet&, size_t offset) noexcept;
uint16_t packetU16(const FakeGroovyEndpoint::Packet&, size_t offset) noexcept;

}  // namespace mistercast::test
