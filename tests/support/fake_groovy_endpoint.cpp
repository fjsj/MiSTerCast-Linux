#include "support/fake_groovy_endpoint.hpp"

#include <arpa/inet.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

#include "support/groovy_wire.hpp"

namespace mistercast::test {
namespace {
bool isCommand(const FakeGroovyEndpoint::Packet& packet,
               uint8_t command) noexcept {
  if (packet.empty()) return false;
  return isGroovyCommand(packet[0], packet.size(), command);
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
    Packet packet(bytes.begin(), bytes.begin() + size);
    {
      std::lock_guard<std::mutex> lock(packetsMutex_);
      packets_.push_back(packet);
    }
    packetsChanged_.notify_all();
    handler_(*this, packet);
    {
      std::lock_guard<std::mutex> lock(packetsMutex_);
      ++handledPackets_;
    }
    packetsChanged_.notify_all();
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

bool FakeGroovyEndpoint::waitForCommand(uint8_t command,
                                        std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(packetsMutex_);
  return packetsChanged_.wait_for(lock, timeout, [&] {
    return std::any_of(packets_.begin(), packets_.end(),
                       [command](const Packet& packet) {
                         return isCommand(packet, command);
                       });
  });
}

bool FakeGroovyEndpoint::waitForHandledCommand(
    uint8_t command, size_t occurrences, std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(packetsMutex_);
  return packetsChanged_.wait_for(lock, timeout, [&] {
    return size_t(std::count_if(
               packets_.begin(), packets_.begin() + handledPackets_,
               [command](const Packet& packet) {
                 return isCommand(packet, command);
               })) >= occurrences;
  });
}

std::vector<FakeGroovyEndpoint::Packet> FakeGroovyEndpoint::packets() const {
  std::lock_guard<std::mutex> lock(packetsMutex_);
  return packets_;
}

void FakeGroovyEndpoint::stop() noexcept {
  running_ = false;
  if (worker_.joinable()) worker_.join();
  if (fd_ >= 0) close(fd_);
  fd_ = -1;
}

uint32_t packetU32(const FakeGroovyEndpoint::Packet& packet,
                   size_t offset) noexcept {
  return readWire<uint32_t>(packet.data(), packet.size(), offset);
}

uint16_t packetU16(const FakeGroovyEndpoint::Packet& packet,
                   size_t offset) noexcept {
  return readWire<uint16_t>(packet.data(), packet.size(), offset);
}

}  // namespace mistercast::test
