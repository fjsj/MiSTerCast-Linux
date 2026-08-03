#pragma once

#include <cstdint>
#include <mutex>
#include <optional>

namespace mistercast {
inline constexpr uint32_t AdaptiveHealthyAcksPerStep = 300;
inline constexpr uint16_t AdaptiveReserveStepLines = 4;

struct AdaptiveDeliveryStats {
  uint16_t reserveLines{}, latestSafeLine{};
  uint32_t healthyAcks{};
  uint64_t reductions{}, resets{};
  bool eligible{};
};

enum class DeliveryObservation { Healthy, Unhealthy, Missing };

class AdaptiveDeliveryMargin {
 public:
  void configure(uint16_t vTotal, bool fieldBuffer, bool automatic) noexcept;
  void setAutomatic(bool automatic) noexcept;
  void phaseLocked() noexcept;
  void observe(DeliveryObservation observation) noexcept;
  void reset() noexcept;
  std::optional<uint16_t> latestSafeLine() const noexcept;
  AdaptiveDeliveryStats stats() const noexcept;

 private:
  bool eligible() const noexcept;
  void restoreConservative() noexcept;

  mutable std::mutex mutex_;
  uint16_t vTotal_{}, conservativeReserve_{}, minimumReserve_{}, reserve_{};
  uint32_t healthyAcks_{};
  uint64_t reductions_{}, resets_{};
  bool fieldBuffer_{}, automatic_{}, phaseLocked_{};
};
}  // namespace mistercast
