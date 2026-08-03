#include "mistercast/adaptive_timing.hpp"

#include <algorithm>

namespace mistercast {
void AdaptiveDeliveryMargin::configure(uint16_t vTotal, bool fieldBuffer,
                                       bool automatic) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  vTotal_ = conservativeReserve_ = minimumReserve_ = reserve_ = 0;
  healthyAcks_ = 0;
  reductions_ = resets_ = 0;
  fieldBuffer_ = fieldBuffer;
  automatic_ = automatic;
  phaseLocked_ = false;
  if (!fieldBuffer || !vTotal) return;
  const uint16_t conservative = std::max<uint16_t>(1, vTotal / 2);
  const uint16_t minimum =
      std::max<uint16_t>(1, uint16_t(uint32_t(vTotal) * 3 / 8));
  vTotal_ = vTotal;
  conservativeReserve_ = conservative;
  minimumReserve_ = minimum;
  reserve_ = conservative;
}

bool AdaptiveDeliveryMargin::eligible() const noexcept {
  return vTotal_ && fieldBuffer_ && automatic_ && phaseLocked_;
}

void AdaptiveDeliveryMargin::restoreConservative() noexcept {
  reserve_ = conservativeReserve_;
  healthyAcks_ = 0;
}

void AdaptiveDeliveryMargin::setAutomatic(bool automatic) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  if (automatic_ == automatic) return;
  automatic_ = automatic;
  restoreConservative();
}

void AdaptiveDeliveryMargin::phaseLocked() noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  phaseLocked_ = true;
}

void AdaptiveDeliveryMargin::observe(
    DeliveryObservation observation) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!eligible()) return;
  if (observation == DeliveryObservation::Missing) {
    healthyAcks_ = 0;
    return;
  }
  if (observation == DeliveryObservation::Unhealthy) {
    if (reserve_ != conservativeReserve_ || healthyAcks_ != 0) ++resets_;
    restoreConservative();
    return;
  }
  if (++healthyAcks_ < AdaptiveHealthyAcksPerStep) return;
  healthyAcks_ = 0;
  if (reserve_ <= minimumReserve_) return;
  reserve_ = uint16_t(std::max<int>(
      minimumReserve_, int(reserve_) - AdaptiveReserveStepLines));
  ++reductions_;
}

void AdaptiveDeliveryMargin::reset() noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  vTotal_ = conservativeReserve_ = minimumReserve_ = reserve_ = 0;
  healthyAcks_ = 0;
  reductions_ = resets_ = 0;
  fieldBuffer_ = automatic_ = phaseLocked_ = false;
}

std::optional<uint16_t> AdaptiveDeliveryMargin::latestSafeLine() const
    noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!eligible()) return std::nullopt;
  return uint16_t(vTotal_ - reserve_);
}

AdaptiveDeliveryStats AdaptiveDeliveryMargin::stats() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return {reserve_,
          uint16_t(vTotal_ ? vTotal_ - reserve_ : 0),
          healthyAcks_,
          reductions_,
          resets_,
          eligible()};
}
}  // namespace mistercast
