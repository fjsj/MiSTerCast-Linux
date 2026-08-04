#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "mistercast/adaptive_timing.hpp"

using namespace mistercast;

namespace {

// 525 total lines is 480i NTSC: the conservative reserve is vTotal/2 (262) and
// the floor is 3/8 of vTotal (196).
constexpr uint16_t kNtscTotal = 525;
constexpr uint16_t kNtscConservative = 262;
constexpr uint16_t kNtscFloor = 196;

class AdaptiveMargin : public testing::Test {
 protected:
  void lockNtsc() {
    margin.configure(kNtscTotal, /*fieldBuffer=*/true, /*automatic=*/true);
    margin.phaseLocked();
  }
  void healthy(unsigned times = 1) {
    for (unsigned i = 0; i < times; ++i)
      margin.observe(DeliveryObservation::Healthy);
  }
  void unhealthy() { margin.observe(DeliveryObservation::Unhealthy); }
  void missing() { margin.observe(DeliveryObservation::Missing); }

  AdaptiveDeliveryMargin margin;
};

TEST_F(AdaptiveMargin, StartsUnconfiguredAndIneligible) {
  const auto stats = margin.stats();
  EXPECT_EQ(stats.reserveLines, 0);
  EXPECT_EQ(stats.latestSafeLine, 0);
  EXPECT_FALSE(stats.eligible);
  EXPECT_FALSE(margin.latestSafeLine().has_value());
}

TEST_F(AdaptiveMargin, ConfiguringSetsTheConservativeReserveButStaysIneligible) {
  margin.configure(kNtscTotal, true, true);
  const auto stats = margin.stats();
  EXPECT_EQ(stats.reserveLines, kNtscConservative);
  EXPECT_EQ(stats.latestSafeLine, kNtscTotal - kNtscConservative);
  EXPECT_EQ(stats.healthyAcks, 0u);
  EXPECT_EQ(stats.reductions, 0u);
  EXPECT_EQ(stats.resets, 0u);
  EXPECT_FALSE(stats.eligible)
      << "no post-switch ACK has established the raster phase yet";
  EXPECT_FALSE(margin.latestSafeLine().has_value());
}

TEST_F(AdaptiveMargin, BecomesEligibleOnlyOnceThePhaseIsLocked) {
  margin.configure(kNtscTotal, true, true);
  margin.phaseLocked();
  EXPECT_TRUE(margin.stats().eligible);
  EXPECT_EQ(margin.latestSafeLine(), kNtscTotal - kNtscConservative);
}

TEST_F(AdaptiveMargin, ProgressiveModesAreNeverEligible) {
  margin.configure(kNtscTotal, /*fieldBuffer=*/false, /*automatic=*/true);
  margin.phaseLocked();
  const auto stats = margin.stats();
  EXPECT_FALSE(stats.eligible);
  EXPECT_EQ(stats.reserveLines, 0);
  EXPECT_EQ(stats.latestSafeLine, 0);
  EXPECT_FALSE(margin.latestSafeLine().has_value());
}

TEST_F(AdaptiveMargin, AZeroLineTotalIsNeverEligible) {
  margin.configure(0, true, true);
  margin.phaseLocked();
  EXPECT_FALSE(margin.stats().eligible);
  EXPECT_FALSE(margin.latestSafeLine().has_value());
}

TEST_F(AdaptiveMargin, ManualFrameDelayIsNeverEligible) {
  margin.configure(kNtscTotal, /*fieldBuffer=*/true, /*automatic=*/false);
  margin.phaseLocked();
  EXPECT_FALSE(margin.stats().eligible);
  healthy(AdaptiveHealthyAcksPerStep * 2);
  EXPECT_EQ(margin.stats().reserveLines, kNtscConservative)
      << "observations must be ignored while the margin is ineligible";
}

TEST_F(AdaptiveMargin, ObservationsBeforeThePhaseLockAreIgnored) {
  margin.configure(kNtscTotal, true, true);
  healthy(AdaptiveHealthyAcksPerStep * 2);
  unhealthy();
  const auto stats = margin.stats();
  EXPECT_EQ(stats.healthyAcks, 0u);
  EXPECT_EQ(stats.reductions, 0u);
  EXPECT_EQ(stats.resets, 0u);
}

TEST_F(AdaptiveMargin, OneStepNeedsAFullRunOfHealthyAcknowledgements) {
  lockNtsc();
  healthy(AdaptiveHealthyAcksPerStep - 1);
  auto stats = margin.stats();
  EXPECT_EQ(stats.reserveLines, kNtscConservative);
  EXPECT_EQ(stats.healthyAcks, AdaptiveHealthyAcksPerStep - 1);
  EXPECT_EQ(stats.reductions, 0u);

  healthy();
  stats = margin.stats();
  EXPECT_EQ(stats.reserveLines, kNtscConservative - AdaptiveReserveStepLines);
  EXPECT_EQ(stats.latestSafeLine,
            kNtscTotal - kNtscConservative + AdaptiveReserveStepLines);
  EXPECT_EQ(stats.healthyAcks, 0u) << "the run restarts after each step";
  EXPECT_EQ(stats.reductions, 1u);
}

TEST_F(AdaptiveMargin, UnhealthyFeedbackRestoresTheConservativeReserve) {
  lockNtsc();
  healthy(AdaptiveHealthyAcksPerStep);
  ASSERT_LT(margin.stats().reserveLines, kNtscConservative);

  unhealthy();
  auto stats = margin.stats();
  EXPECT_EQ(stats.reserveLines, kNtscConservative);
  EXPECT_EQ(stats.latestSafeLine, kNtscTotal - kNtscConservative);
  EXPECT_EQ(stats.resets, 1u);

  unhealthy();
  EXPECT_EQ(margin.stats().resets, 1u)
      << "already conservative with no progress, so this is not a new reset";

  healthy(10);
  unhealthy();
  stats = margin.stats();
  EXPECT_EQ(stats.resets, 2u) << "discarding partial progress counts as a reset";
  EXPECT_EQ(stats.healthyAcks, 0u);
}

TEST_F(AdaptiveMargin, AMissingAcknowledgementOnlyBreaksTheHealthyRun) {
  lockNtsc();
  healthy(AdaptiveHealthyAcksPerStep + 10);
  const auto reducedReserve = margin.stats().reserveLines;
  ASSERT_EQ(margin.stats().healthyAcks, 10u);

  missing();
  const auto stats = margin.stats();
  EXPECT_EQ(stats.reserveLines, reducedReserve)
      << "a lost ACK is not evidence the receiver is unhealthy";
  EXPECT_EQ(stats.healthyAcks, 0u);
  EXPECT_EQ(stats.resets, 0u);
}

TEST_F(AdaptiveMargin, StepsDownNoFurtherThanThreeEighthsOfTheRaster) {
  lockNtsc();
  for (unsigned step = 0; step < 40; ++step) healthy(AdaptiveHealthyAcksPerStep);
  auto stats = margin.stats();
  EXPECT_EQ(stats.reserveLines, kNtscFloor);
  EXPECT_EQ(stats.latestSafeLine, kNtscTotal - kNtscFloor);
  const auto reductionsAtFloor = stats.reductions;

  healthy(AdaptiveHealthyAcksPerStep);
  stats = margin.stats();
  EXPECT_EQ(stats.reserveLines, kNtscFloor);
  EXPECT_EQ(stats.reductions, reductionsAtFloor)
      << "no reduction is counted once the floor is reached";
}

TEST_F(AdaptiveMargin, PalRasterUsesItsOwnConservativeReserveAndFloor) {
  margin.configure(625, true, true);
  margin.phaseLocked();
  EXPECT_EQ(margin.stats().reserveLines, 312);
  for (unsigned step = 0; step < 40; ++step) healthy(AdaptiveHealthyAcksPerStep);
  const auto stats = margin.stats();
  EXPECT_EQ(stats.reserveLines, 234);  // 625 * 3 / 8
  EXPECT_EQ(stats.latestSafeLine, 625 - 234);
}

TEST_F(AdaptiveMargin, AVeryShortRasterKeepsAtLeastOneReservedLine) {
  margin.configure(1, true, true);
  margin.phaseLocked();
  EXPECT_EQ(margin.stats().reserveLines, 1);
  EXPECT_EQ(margin.latestSafeLine(), 0);
  healthy(AdaptiveHealthyAcksPerStep * 3);
  EXPECT_EQ(margin.stats().reserveLines, 1);
}

TEST_F(AdaptiveMargin, ReconfiguringDiscardsAllProgressAndThePhaseLock) {
  lockNtsc();
  healthy(AdaptiveHealthyAcksPerStep);
  ASSERT_EQ(margin.stats().reductions, 1u);

  margin.configure(625, true, true);
  const auto stats = margin.stats();
  EXPECT_EQ(stats.reserveLines, 312);
  EXPECT_EQ(stats.healthyAcks, 0u);
  EXPECT_EQ(stats.reductions, 0u);
  EXPECT_EQ(stats.resets, 0u);
  EXPECT_FALSE(stats.eligible) << "the new mode needs a fresh phase lock";
}

TEST_F(AdaptiveMargin, SwitchingToAProgressiveModeClearsTheReserve) {
  lockNtsc();
  healthy(AdaptiveHealthyAcksPerStep);
  margin.configure(kNtscTotal, /*fieldBuffer=*/false, true);
  const auto stats = margin.stats();
  EXPECT_EQ(stats.reserveLines, 0);
  EXPECT_EQ(stats.latestSafeLine, 0);
  EXPECT_EQ(stats.reductions, 0u);
  EXPECT_EQ(stats.resets, 0u);
}

TEST_F(AdaptiveMargin, TurningAutomaticTimingOffRestoresTheConservativeReserve) {
  lockNtsc();
  healthy(AdaptiveHealthyAcksPerStep);
  ASSERT_LT(margin.stats().reserveLines, kNtscConservative);

  margin.setAutomatic(false);
  EXPECT_FALSE(margin.stats().eligible);
  EXPECT_EQ(margin.stats().reserveLines, kNtscConservative);

  margin.setAutomatic(true);
  EXPECT_TRUE(margin.stats().eligible) << "the phase lock survives the switch";
  EXPECT_EQ(margin.stats().reserveLines, kNtscConservative);
}

TEST_F(AdaptiveMargin, SettingTheSameAutomaticStateChangesNothing) {
  lockNtsc();
  healthy(AdaptiveHealthyAcksPerStep + 5);
  const auto before = margin.stats();
  margin.setAutomatic(true);
  const auto after = margin.stats();
  EXPECT_EQ(after.reserveLines, before.reserveLines);
  EXPECT_EQ(after.healthyAcks, before.healthyAcks);
}

TEST_F(AdaptiveMargin, ResetReturnsToTheUnconfiguredState) {
  lockNtsc();
  healthy(AdaptiveHealthyAcksPerStep);
  margin.reset();
  const auto stats = margin.stats();
  EXPECT_EQ(stats.reserveLines, 0);
  EXPECT_EQ(stats.latestSafeLine, 0);
  EXPECT_EQ(stats.healthyAcks, 0u);
  EXPECT_EQ(stats.reductions, 0u);
  EXPECT_EQ(stats.resets, 0u);
  EXPECT_FALSE(stats.eligible);
  EXPECT_FALSE(margin.latestSafeLine().has_value());
}

TEST_F(AdaptiveMargin, ObservationsAndQueriesFromSeveralThreadsStayConsistent) {
  // The transport observes from the rendering thread while the GUI polls stats,
  // so every entry point takes the same lock.
  lockNtsc();
  std::atomic<bool> stop{false};
  std::thread reader([&] {
    while (!stop) {
      const auto stats = margin.stats();
      ASSERT_GE(stats.reserveLines, kNtscFloor);
      ASSERT_LE(stats.reserveLines, kNtscConservative);
      margin.latestSafeLine();
    }
  });
  for (unsigned step = 0; step < 6; ++step) {
    healthy(AdaptiveHealthyAcksPerStep);
    missing();
  }
  unhealthy();
  stop = true;
  reader.join();
  EXPECT_EQ(margin.stats().reserveLines, kNtscConservative);
}

}  // namespace
