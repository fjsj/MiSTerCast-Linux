#include "mistercast/adaptive_timing.hpp"

#include <algorithm>

namespace mistercast {
void AdaptiveDeliveryMargin::configure(uint16_t vTotal,
                                       bool fieldBuffer) noexcept {
  close();
  if (!fieldBuffer || !vTotal) return;
  const uint16_t conservative = std::max<uint16_t>(1, vTotal / 2);
  const uint16_t minimum =
      std::max<uint16_t>(1, uint16_t(uint32_t(vTotal) * 3 / 8));
  vTotal_ = vTotal;
  conservativeReserve_ = conservative;
  minimumReserve_ = minimum;
  reserve_ = conservative;
  latestSafeLine_ = vTotal - conservative;
}

void AdaptiveDeliveryMargin::healthyAck() noexcept {
  if (!vTotal_) return;
  const uint32_t accumulated = healthyAcks_.fetch_add(1) + 1;
  if (accumulated < AdaptiveHealthyAcksPerStep) return;
  healthyAcks_ = 0;
  const uint16_t reserve = reserve_;
  const uint16_t minimum = minimumReserve_;
  if (reserve <= minimum) return;
  const uint16_t reduced = uint16_t(
      std::max<int>(minimum, int(reserve) - AdaptiveReserveStepLines));
  reserve_ = reduced;
  latestSafeLine_ = vTotal_ - reduced;
  ++reductions_;
}

void AdaptiveDeliveryMargin::unhealthyAck() noexcept {
  if (!vTotal_) return;
  const uint16_t conservative = conservativeReserve_;
  if (reserve_ != conservative || healthyAcks_ != 0) ++resets_;
  reserve_ = conservative;
  latestSafeLine_ = vTotal_ - conservative;
  healthyAcks_ = 0;
}

void AdaptiveDeliveryMargin::missingAck() noexcept { healthyAcks_ = 0; }

void AdaptiveDeliveryMargin::close() noexcept {
  vTotal_ = conservativeReserve_ = minimumReserve_ = reserve_ =
      latestSafeLine_ = 0;
  healthyAcks_ = 0;
  reductions_ = resets_ = 0;
}

AdaptiveDeliveryStats AdaptiveDeliveryMargin::stats() const noexcept {
  return {reserve_, latestSafeLine_, healthyAcks_, reductions_, resets_};
}
}  // namespace mistercast
