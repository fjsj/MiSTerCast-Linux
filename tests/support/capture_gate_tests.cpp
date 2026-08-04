#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "support/fake_capture.hpp"

using namespace mistercast::test;

namespace {

// disarm() has to publish `armed_` while holding the gate's mutex. If it writes
// it outside, a waiter can evaluate take()'s predicate under the lock and be
// pre-empted before it blocks, so the notify that follows arrives before the
// waiter is registered and is lost. The waiter then never leaves next(), which
// hangs StreamSession::stop() on captureThread_.join() until ctest's suite
// timeout — so the session suite reports an opaque stall instead of whatever it
// was actually asserting. ThreadSanitizer cannot see this class of defect,
// because nothing here is a data race, so this is the only guard.
//
// Racing disarm() against gate entry across a sweep of offsets reproduces it in
// single-digit trials when the write is unlocked (observed at trials 3, 4 and 5
// on three separate runs), so 2000 trials leaves a wide margin.
TEST(CaptureGate, DisarmWakesAWaiterThatIsStillEnteringTheGate) {
  constexpr int kTrials = 2000;
  constexpr int kOffsets = 400;
  for (int trial = 0; trial < kTrials; ++trial) {
    CaptureGate gate;
    gate.arm();
    std::atomic<bool> entered{false}, returned{false};
    std::thread waiter([&] {
      entered = true;
      gate.take();
      returned = true;
    });
    while (!entered) std::this_thread::yield();
    // Land disarm() at a different point around the predicate check every
    // trial, including the window between checking it and blocking on it.
    for (volatile int spin = 0; spin < trial % kOffsets; ++spin) {
    }
    gate.disarm();

    // A regression must fail here rather than hang the suite, so give the
    // waiter a deadline and then prod it loose. `armed_` is already false, so
    // any notify releases it, and a second disarm() is idempotent.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!returned && std::chrono::steady_clock::now() < deadline)
      std::this_thread::yield();
    const bool woke = returned;
    if (!woke) gate.disarm();
    waiter.join();
    ASSERT_TRUE(woke) << "disarm() was lost on trial " << trial
                      << ": a waiter entering the gate slept through it";
  }
}

// take() returns early on an unarmed gate before it touches `waiting_` or falls
// into the credit check. Both halves of that placement are load-bearing: after
// the wait instead, an unarmed gate would satisfy the predicate through
// !armed_, drop into `if (!credits_) return false` with no credits, and turn
// next() into a permanent failure for every ungated test in the session suite;
// after `waiting_ = true`, parked() would report a waiter that is not in the
// gate at all.
TEST(CaptureGate, AnUnarmedGateLetsEveryCaptureThroughAndParksNobody) {
  CaptureGate gate;
  for (int i = 0; i < 3; ++i) EXPECT_TRUE(gate.take());
  EXPECT_FALSE(gate.parked());
}

// disarm() is one-shot, which is what FakeVideo::stop() relies on to release
// the capture thread. A test that restarts a session against the same fake has
// to arm it again, and this pins that so the comment saying so cannot drift.
TEST(CaptureGate, StaysOpenAfterDisarmUntilItIsArmedAgain) {
  CaptureGate gate;
  gate.arm();
  gate.disarm();
  EXPECT_TRUE(gate.take()) << "a disarmed gate must not block";

  gate.arm();
  gate.release(1);
  EXPECT_TRUE(gate.take()) << "arming again must re-establish the gate";
  EXPECT_FALSE(gate.parked()) << "the credit was spent by the take above";
}

}  // namespace
