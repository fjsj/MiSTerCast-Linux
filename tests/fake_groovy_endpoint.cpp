#include "fake_groovy_endpoint.hpp"

#include <arpa/inet.h>
#include <unistd.h>

#include <cstring>

namespace mistercast::test {
namespace {
template <class T>
T readPacket(const FakeGroovyEndpoint::Packet& packet, size_t offset) noexcept {
  T value{};
  if (offset + sizeof(value) <= packet.size())
    std::memcpy(&value, packet.data() + offset, sizeof(value));
  return value;
}
}  // namespace

std::array<uint8_t, 13> GroovyAck::encode() const noexcept {
  std::array<uint8_t, 13> bytes{};
  std::memcpy(bytes.data(), &frameEcho, sizeof(frameEcho));
  std::memcpy(bytes.data() + 4, &vCountEcho, sizeof(vCountEcho));
  std::memcpy(bytes.data() + 6, &fpgaFrame, sizeof(fpgaFrame));
  std::memcpy(bytes.data() + 10, &fpgaVCount, sizeof(fpgaVCount));
  bytes[12] = statusBits;
  return bytes;
}

FakeGroovyEndpoint::FakeGroovyEndpoint(Handler handler, uint16_t port)
    : handler_(std::move(handler)) {
  fd_ = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd_ < 0) return;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  if (bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    close(fd_);
    fd_ = -1;
    return;
  }
  socklen_t addressSize = sizeof(address);
  if (getsockname(fd_, reinterpret_cast<sockaddr*>(&address), &addressSize) !=
      0) {
    close(fd_);
    fd_ = -1;
    return;
  }
  port_ = ntohs(address.sin_port);
  timeval timeout{0, 100000};
  setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  running_ = true;
  worker_ = std::thread([this] { serve(); });
}

FakeGroovyEndpoint::~FakeGroovyEndpoint() { stop(); }

void FakeGroovyEndpoint::serve() {
  std::array<uint8_t, 65536> bytes{};
  while (running_) {
    peerSize_ = sizeof(peer_);
    auto size = recvfrom(fd_, bytes.data(), bytes.size(), 0,
                         reinterpret_cast<sockaddr*>(&peer_), &peerSize_);
    if (size <= 0) continue;
    handler_(*this, Packet(bytes.begin(), bytes.begin() + size));
  }
}

void FakeGroovyEndpoint::reply(const void* data, size_t size) noexcept {
  if (fd_ < 0 || !peerSize_) return;
  sendto(fd_, data, size, 0, reinterpret_cast<sockaddr*>(&peer_), peerSize_);
}

void FakeGroovyEndpoint::replyVersion(uint8_t version) noexcept {
  reply(&version, sizeof(version));
}

void FakeGroovyEndpoint::replyAck(const GroovyAck& ack) noexcept {
  const auto bytes = ack.encode();
  reply(bytes.data(), bytes.size());
}

void FakeGroovyEndpoint::stop() noexcept {
  running_ = false;
  if (worker_.joinable()) worker_.join();
  if (fd_ >= 0) close(fd_);
  fd_ = -1;
}

uint32_t packetU32(const FakeGroovyEndpoint::Packet& packet,
                   size_t offset) noexcept {
  return readPacket<uint32_t>(packet, offset);
}

uint16_t packetU16(const FakeGroovyEndpoint::Packet& packet,
                   size_t offset) noexcept {
  return readPacket<uint16_t>(packet, offset);
}

}  // namespace mistercast::test
