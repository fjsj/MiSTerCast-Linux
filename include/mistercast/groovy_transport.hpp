#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "mistercast/types.hpp"
namespace mistercast {
struct GroovyTransportStats {
  uint32_t acknowledgedFrame{}, fpgaFrame{};
  uint16_t requestedSyncLine{}, fpgaVCount{};
  uint64_t acknowledgedFrames{}, missedAcks{}, streamTimeUs{}, ackAgeMs{},
      sendErrors{}, networkRttUs{};
  int64_t rasterCorrectionUs{};
  bool vramSynced{}, vgaFrameskip{}, vgaVblank{};
};
class GroovyTransport {
 public:
  GroovyTransport();
  ~GroovyTransport();
  GroovyTransport(const GroovyTransport&) = delete;
  GroovyTransport& operator=(const GroovyTransport&) = delete;
  bool open(const std::string& host, uint32_t audioRate, std::string& error,
            uint16_t port = 32100);
  bool switchMode(const Modeline&, bool progressiveInterlaceBuffer,
                  std::string& error);
  void setSyncOptions(bool syncRefresh, uint16_t frameDelay) noexcept;
  void alignFrame(uint32_t& frame, uint8_t& field) const noexcept;
  bool sendFrame(uint32_t frame, uint8_t field, const std::vector<uint8_t>& rgb,
                 std::string& error);
  bool sendAudio(const int16_t* samples, size_t count, std::string& error);
  void waitSync() noexcept;
  void close() noexcept;
  bool connected() const noexcept { return fd_ >= 0; }
  bool misterAudioEnabled() const noexcept {
    return misterAudioEnabled_.load();
  }
  GroovyTransportStats stats() const noexcept;

 private:
  struct FpgaStatus {
    uint32_t frameEcho{}, frame{};
    uint16_t vCountEcho{}, vCount{};
    uint8_t bits{};
  };
  int fd_{-1};
  uint32_t frameBytes_{};
  uint16_t mtu_{1472}, vTotal_{}, frameDelay_{};
  uint8_t interlaceShift_{};
  std::vector<uint8_t> compressed_;
  bool syncRefresh_{true}, progressiveInterlaceBuffer_{};
  uint64_t frameTimeNs_{}, lineTimeNs_{}, networkRttNs_{}, lastStreamNs_{};
  uint32_t currentFrame_{};
  uint8_t coreVersion_{};
  FpgaStatus fpga_{};
  std::chrono::steady_clock::time_point syncEpoch_{}, lastAckAt_{},
      lastSendEndAt_{};
  bool sendPacket(const void*, size_t, std::string&);
  bool sendChunks(const uint8_t*, size_t, std::string&);
  bool drainStatus(uint32_t expectedFrame) noexcept;
  uint16_t syncLine(uint64_t workNs) const noexcept;
  std::atomic<bool> misterAudioEnabled_{false}, vramSynced_{false},
      vgaFrameskip_{false}, vgaVblank_{false};
  std::atomic<uint32_t> ackFrame_{0}, fpgaFrame_{0};
  std::atomic<uint16_t> syncLine_{0}, fpgaVCount_{0};
  std::atomic<uint64_t> ackedFrames_{0}, missedAcks_{0}, streamTimeUs_{0},
      ackAgeMs_{0}, sendErrors_{0};
  std::atomic<int64_t> rasterCorrectionUs_{0};
};
}  // namespace mistercast
