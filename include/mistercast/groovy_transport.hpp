#pragma once
#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "mistercast/types.hpp"
#include "mistercast/adaptive_timing.hpp"
namespace mistercast {
class GroovyTransportTestPeer;
class UdpVideoSender;
struct GroovyTransportStats {
  uint32_t acknowledgedFrame{}, fpgaFrame{};
  uint16_t requestedSyncLine{}, fpgaVCount{};
  uint64_t acknowledgedFrames{}, missedAcks{}, streamTimeUs{}, ackAgeMs{},
      sendErrors{}, networkRttUs{}, fieldRealignments{}, fpgaStatusSamples{},
      fpgaFallbackSamples{}, vramUnsyncedSamples{}, vramQueueEmptySamples{},
      compressionTimeUs{}, submissionTimeUs{}, estimatedWireTimeUs{},
      pacedVideoPayloads{}, pacedDatagrams{}, lateBatchReleases{},
      maxBatchReleaseLatenessNs{}, observedUdpQueueHighWater{},
      socketSendBufferBytes{}, pathMtu{}, adaptiveReductions{},
      adaptiveResets{};
  int64_t rasterCorrectionUs{};
  uint8_t outgoingField{}, fpgaField{};
  uint16_t deliveryReserveLines{}, adaptiveLatestSafeLine{};
  uint32_t adaptiveHealthyAcks{};
  bool vramSynced{}, vgaFrameskip{}, vgaVblank{}, vramQueuePresent{},
      interlacedFieldBuffer{}, fieldPhaseValid{}, adaptiveTimingEligible{};
};
// False when the build has no liblz4, in which case frames go out uncompressed
// at roughly 3-5x the bandwidth. Windows always compressed.
bool compressionAvailable() noexcept;
bool encodeInitCommand(bool compression, bool audioEnabled,
                       uint32_t audioRate, std::array<uint8_t, 5>& command,
                       std::string& error) noexcept;
class GroovyTransport {
 public:
  GroovyTransport();
  ~GroovyTransport();
  GroovyTransport(const GroovyTransport&) = delete;
  GroovyTransport& operator=(const GroovyTransport&) = delete;
  bool open(const std::string& host, bool audioEnabled, uint32_t audioRate,
            std::string& error, uint16_t port = 32100);
  bool switchMode(const Modeline&, bool progressiveInterlaceBuffer,
                  std::string& error);
  void setSyncOptions(bool syncRefresh, uint16_t frameDelay) noexcept;
  void alignFrame(uint32_t& frame, uint8_t& field) noexcept;
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
  explicit GroovyTransport(std::unique_ptr<UdpVideoSender> videoSender);
  friend class GroovyTransportTestPeer;
  struct FpgaStatus {
    uint32_t frameEcho{}, frame{};
    uint16_t vCountEcho{}, vCount{};
    uint8_t bits{};
  };
  int fd_{-1};
  uint32_t frameBytes_{};
  uint16_t vTotal_{}, frameDelay_{};
  uint8_t interlaceShift_{};
  std::vector<uint8_t> compressed_;
  std::unique_ptr<UdpVideoSender> videoSender_;
  bool syncRefresh_{true}, progressiveInterlaceBuffer_{}, firstFrame_{true},
      fatalPayloadError_{};
  bool phaseValid_{}, fallbackPhaseSet_{}, lastAligned_{}, haveFpgaStatus_{},
      haveDiagnosticFrame_{};
  uint64_t frameTimeNs_{}, lineTimeNs_{}, networkRttNs_{}, lastStreamNs_{},
      lastWireDeliveryNs_{};
  uint32_t currentFrame_{}, fallbackFrame_{}, lastAlignedFrame_{},
      diagnosticFrame_{};
  uint8_t coreVersion_{}, lastAlignedField_{};
  FpgaStatus fpga_{};
  AdaptiveDeliveryMargin adaptiveMargin_;
  std::chrono::steady_clock::time_point syncEpoch_{}, lastAckAt_{},
      lastSendEndAt_{};
  bool sendPacket(const void*, size_t, std::string&);
  bool sendChunks(const uint8_t*, size_t, std::string&);
  static bool decodeStatus(const uint8_t*, size_t, FpgaStatus&) noexcept;
  void applyStatus(const FpgaStatus&) noexcept;
  bool drainStatus(uint32_t expectedFrame) noexcept;
  uint16_t syncLine(uint64_t workNs) const noexcept;
  bool adaptiveTimingEligible() const noexcept;
  int64_t rasterCorrection() const noexcept;
  std::atomic<bool> misterAudioEnabled_{false}, vramSynced_{false},
      vgaFrameskip_{false}, vgaVblank_{false}, vramQueuePresent_{false},
      interlacedFieldBuffer_{false}, fieldPhaseValid_{false};
  std::atomic<uint32_t> ackFrame_{0}, fpgaFrame_{0};
  std::atomic<uint32_t> pathMtu_{0};
  std::atomic<uint16_t> syncLine_{0}, fpgaVCount_{0};
  std::atomic<uint8_t> outgoingField_{0}, fpgaField_{0};
  std::atomic<uint64_t> ackedFrames_{0}, missedAcks_{0}, streamTimeUs_{0},
      ackAgeMs_{0}, sendErrors_{0}, fieldRealignments_{0},
      fpgaStatusSamples_{0}, fpgaFallbackSamples_{0},
      vramUnsyncedSamples_{0}, vramQueueEmptySamples_{0},
      compressionTimeUs_{0}, submissionTimeUs_{0}, estimatedWireTimeUs_{0},
      socketSendBufferBytes_{0};
  std::atomic<int64_t> rasterCorrectionUs_{0};
};
}  // namespace mistercast
