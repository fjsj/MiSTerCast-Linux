#pragma once

#include <atomic>
#include <cstdint>

namespace mistercast {
inline constexpr uint32_t AdaptiveHealthyAcksPerStep = 300;
inline constexpr uint16_t AdaptiveReserveStepLines = 4;

struct AdaptiveDeliveryStats {
  uint16_t reserveLines{}, latestSafeLine{};
  uint32_t healthyAcks{};
  uint64_t reductions{}, resets{};
};

class AdaptiveDeliveryMargin {
 public:
  void configure(uint16_t vTotal, bool fieldBuffer) noexcept;
  void healthyAck() noexcept;
  void unhealthyAck() noexcept;
  void missingAck() noexcept;
  void close() noexcept;
  AdaptiveDeliveryStats stats() const noexcept;

 private:
  std::atomic<uint16_t> vTotal_{0}, conservativeReserve_{0},
      minimumReserve_{0}, reserve_{0}, latestSafeLine_{0};
  std::atomic<uint32_t> healthyAcks_{0};
  std::atomic<uint64_t> reductions_{0}, resets_{0};
};
}  // namespace mistercast
