#pragma once

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <netinet/ip.h>
#include <sys/socket.h>

#include <cstdint>
#include <memory>
#include <optional>

#include "mistercast/groovy_protocol.hpp"
#include "mistercast/groovy_transport.hpp"
#include "mistercast/udp_pacing.hpp"
#include "support/fake_groovy_endpoint.hpp"
#include "support/groovy_wire.hpp"

namespace mistercast {
// The transport keeps its socket and status decoding private on purpose; this
// declared friend is the single seam the tests use, rather than widening the
// production interface.
class GroovyTransportTestPeer {
 public:
  static std::unique_ptr<GroovyTransport> withVideoConfig(
      UdpVideoConfig config, UdpSubmitSyscalls* syscalls = nullptr) {
    return std::unique_ptr<GroovyTransport>(
        new GroovyTransport(std::make_unique<UdpVideoSender>(config, syscalls)));
  }
  static std::unique_ptr<GroovyTransport> withVideoSyscalls(
      UdpSubmitSyscalls& syscalls) {
    return withVideoConfig(groovyVideoConfig(), &syscalls);
  }
  static std::optional<int> pathMtuDiscoveryMode(
      const GroovyTransport& transport) {
    int mode = 0;
    socklen_t size = sizeof(mode);
    if (getsockopt(transport.fd_, IPPROTO_IP, IP_MTU_DISCOVER, &mode, &size) !=
        0)
      return std::nullopt;
    return mode;
  }
  static void drainPendingStatus(GroovyTransport& transport,
                                 uint32_t expectedFrame) {
    transport.drainStatus(expectedFrame);
  }
};
}  // namespace mistercast

namespace mistercast::test {

// 2x2 active, 5x5 total: small enough that a frame is a handful of bytes and the
// automatic sync-line calculation is always in its "work exceeds a frame period"
// regime.
inline Modeline tinyMode(bool interlaced = true) {
  return {"tiny", 1, 2, 3, 4, 5, 2, 3, 4, 5, interlaced};
}

inline Modeline vgaMode(bool interlaced = false) {
  return {"vga", 25.175, 640, 656, 752, 800, 480, 490, 492, 525, interlaced};
}

// Replies to CMD_INIT only, which is all most tests need to get connected.
inline FakeGroovyEndpoint::Handler acknowledgeInit() {
  return [](FakeGroovyEndpoint& peer, const FakeGroovyEndpoint::Packet& packet) {
    if (!packet.empty() && packet[0] == kInit) peer.replyVersion();
  };
}

inline void acknowledgeBlit(FakeGroovyEndpoint& endpoint,
                            const FakeGroovyEndpoint::Packet& packet,
                            uint32_t fpgaFrame, uint16_t fpgaLine,
                            uint8_t statusBits) {
  endpoint.replyAck({packetU32(packet, 1), packetU16(packet, 6), fpgaFrame,
                     fpgaLine, statusBits});
}

}  // namespace mistercast::test
