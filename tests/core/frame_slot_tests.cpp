#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "mistercast/frame_slot.hpp"

using namespace mistercast;

namespace {

// These rules used to live inside the PipeWire capture, where the only way to
// reach them was a container running a compositor and a desktop portal. Each one
// cost something to get right: the re-delivery rule was measured on real
// hardware as capture running at 24 fps against a 60 Hz stream.

constexpr auto kNoWait = std::chrono::milliseconds(0);
constexpr auto kShortWait = std::chrono::milliseconds(50);

// Fills the producer's buffer with a recognisable frame, then publishes it.
void produce(FrameSlot& slot, uint32_t width, uint32_t height,
             uint8_t fill = 0x40) {
  auto& frame = slot.staging();
  frame.width = width;
  frame.height = height;
  frame.stride = width * 4;
  frame.bgra.assign(size_t(width) * height * 4, fill);
  slot.publish();
}

TEST(FrameSlot, HasNothingToDeliverUntilSomethingIsPublished) {
  FrameSlot slot;
  Frame frame;
  EXPECT_EQ(slot.next(frame, kNoWait), FrameDelivery::Empty);
}

TEST(FrameSlot, DeliversWhatWasPublished) {
  FrameSlot slot;
  produce(slot, 64, 32, 0x7f);
  Frame frame;
  ASSERT_EQ(slot.next(frame, kNoWait), FrameDelivery::Delivered);
  EXPECT_EQ(frame.width, 64u);
  EXPECT_EQ(frame.height, 32u);
  EXPECT_EQ(frame.stride, 64u * 4);
  EXPECT_EQ(frame.bgra.size(), size_t(64) * 32 * 4);
  EXPECT_EQ(frame.bgra.front(), 0x7f);
  EXPECT_EQ(frame.sequence, 1u);
}

// A compositor emits nothing at all while the screen is still, and the MiSTer
// needs a frame every refresh regardless, so the held frame goes out again.
TEST(FrameSlot, RedeliversTheHeldFrameWhenNothingNewArrives) {
  FrameSlot slot;
  produce(slot, 16, 16);
  Frame frame;
  uint64_t previous = 0;
  for (int delivery = 0; delivery < 5; ++delivery) {
    ASSERT_EQ(slot.next(frame, kNoWait), FrameDelivery::Delivered) << delivery;
    EXPECT_EQ(frame.width, 16u);
    EXPECT_GT(frame.sequence, previous) << "each delivery is its own frame";
    previous = frame.sequence;
  }
}

// The rule that cost 24 fps on hardware: with a frame in hand, next() must not
// spend the caller's timeout hoping for a newer one.
TEST(FrameSlot, ReturnsTheHeldFrameWithoutWaiting) {
  FrameSlot slot;
  produce(slot, 8, 8);
  Frame frame;
  ASSERT_EQ(slot.next(frame, kNoWait), FrameDelivery::Delivered);
  const auto start = std::chrono::steady_clock::now();
  ASSERT_EQ(slot.next(frame, std::chrono::seconds(10)),
            FrameDelivery::Delivered);
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(1))
      << "a held frame must be returned at once, not after the timeout";
}

// The first frame is the one worth waiting for, because there is nothing to
// stand in for it.
TEST(FrameSlot, WaitsForTheFirstFrameAndIsWokenByIt) {
  FrameSlot slot;
  std::thread producer([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    produce(slot, 32, 8);
  });
  Frame frame;
  EXPECT_EQ(slot.next(frame, std::chrono::seconds(5)),
            FrameDelivery::Delivered);
  EXPECT_EQ(frame.width, 32u);
  producer.join();
}

// A frame cropped for the old geometry is worse than no frame: the caller changed
// what it expects, so handing back the old one sends a mismatched payload.
TEST(FrameSlot, DiscardDropsTheHeldFrame) {
  FrameSlot slot;
  produce(slot, 64, 64);
  Frame frame;
  ASSERT_EQ(slot.next(frame, kNoWait), FrameDelivery::Delivered);
  slot.discard();
  EXPECT_EQ(slot.next(frame, kNoWait), FrameDelivery::Empty);
  // And a frame published after the discard is delivered normally.
  produce(slot, 24, 24);
  ASSERT_EQ(slot.next(frame, kNoWait), FrameDelivery::Delivered);
  EXPECT_EQ(frame.width, 24u);
}

TEST(FrameSlot, ReportsAFailureOnceAndKeepsTheFirstOne) {
  FrameSlot slot;
  slot.fail({"video", "first failure", "first hint"});
  slot.fail({"video", "second failure", "second hint"});
  Frame frame;
  EXPECT_EQ(slot.next(frame, kNoWait), FrameDelivery::Failed);
  const auto failure = slot.takeFailure();
  ASSERT_TRUE(failure);
  EXPECT_EQ(failure->message, "first failure")
      << "later failures are usually consequences of the first";
  EXPECT_EQ(slot.next(frame, kNoWait), FrameDelivery::Empty)
      << "reading the failure clears it";
}

// A failure outranks a held frame: continuing to stream a frozen picture past a
// revoked screen share is worse than stopping.
TEST(FrameSlot, PrefersReportingAFailureOverTheHeldFrame) {
  FrameSlot slot;
  produce(slot, 16, 16);
  slot.fail({"video", "sharing stopped", "start again"});
  Frame frame;
  EXPECT_EQ(slot.next(frame, kNoWait), FrameDelivery::Failed);
}

TEST(FrameSlot, AFailureWakesAWaiterInsteadOfHoldingItForTheTimeout) {
  FrameSlot slot;
  std::thread producer([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    slot.fail({"video", "no buffers", "check PipeWire"});
  });
  Frame frame;
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(slot.next(frame, std::chrono::seconds(5)), FrameDelivery::Failed);
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(2));
  producer.join();
}

// Teardown must not wait out a consumer's timeout: stop() is called from the
// thread that then joins the producer.
TEST(FrameSlot, StopWakesAWaiterAndRefusesFurtherDelivery) {
  FrameSlot slot;
  std::thread stopper([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    slot.stop();
  });
  Frame frame;
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(slot.next(frame, std::chrono::seconds(5)), FrameDelivery::Empty);
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(2));
  stopper.join();
  // A frame published after stopping is not delivered either.
  produce(slot, 16, 16);
  EXPECT_EQ(slot.next(frame, kNoWait), FrameDelivery::Empty);
}

TEST(FrameSlot, ResetClearsTheFrameTheFailureAndTheStop) {
  FrameSlot slot;
  produce(slot, 16, 16);
  slot.fail({"video", "gone", "again"});
  slot.stop();
  slot.reset();
  Frame frame;
  EXPECT_EQ(slot.next(frame, kNoWait), FrameDelivery::Empty)
      << "the frame from before the reset belongs to the previous stream";
  produce(slot, 48, 12);
  ASSERT_EQ(slot.next(frame, kNoWait), FrameDelivery::Delivered);
  EXPECT_EQ(frame.width, 48u);
  EXPECT_EQ(frame.sequence, 1u) << "sequence numbering restarts with the stream";
}

// publish() swaps, so the two buffers cycle between producer and consumer. Once
// both have been grown to the frame size, neither should allocate again.
TEST(FrameSlot, PublishingCyclesTwoBuffersWithoutReallocating) {
  FrameSlot slot;
  produce(slot, 64, 64);
  produce(slot, 64, 64);
  const auto* cycled = slot.staging().bgra.data();
  ASSERT_NE(cycled, nullptr);
  produce(slot, 64, 64);
  produce(slot, 64, 64);
  EXPECT_EQ(slot.staging().bgra.data(), cycled)
      << "a same-sized frame must reuse the producer's buffer";
}

// discard() and publish() race in production: discard comes from the capture
// thread on a crop change, publish from the PipeWire thread. A publish of a frame
// prepared before the discard must not resurrect it -- that is the defect discard
// exists to prevent, and the caller is responsible for not preparing a frame
// outside the lock that discard is taken under. This pins the slot's half of the
// contract: publish after discard replaces the frame, so the caller must not
// publish a frame that predates the discard.
TEST(FrameSlot, PublishAfterDiscardReplacesRatherThanRestores) {
  FrameSlot slot;
  produce(slot, 64, 64);
  slot.discard();
  Frame frame;
  ASSERT_EQ(slot.next(frame, kNoWait), FrameDelivery::Empty);
  // A publish now delivers whatever the producer most recently prepared, which is
  // why PortalCapture::consume crops and publishes under the same lock that
  // setRegion takes to discard.
  produce(slot, 24, 12);
  ASSERT_EQ(slot.next(frame, kNoWait), FrameDelivery::Delivered);
  EXPECT_EQ(frame.width, 24u) << "the new geometry, not the discarded one";
  EXPECT_EQ(frame.height, 12u);
}

// The producer and consumer are different threads in production, so the handoff
// has to survive them running flat out against each other -- and a third thread
// discarding, which is what `setRegion` does from the capture thread. Every
// delivered frame must still be a whole frame of one geometry: the failure this
// guards is a torn or half-written buffer reaching a caller.
TEST(FrameSlot, SurvivesAProducerAndConsumerRunningConcurrently) {
  FrameSlot slot;
  std::atomic<bool> stop{false};
  std::atomic<int> delivered{0};
  std::thread discarder([&] {
    while (!stop) {
      slot.discard();
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  });
  std::thread producer([&] {
    while (!stop) produce(slot, 32, 32);
  });
  std::thread consumer([&] {
    Frame frame;
    while (!stop)
      if (slot.next(frame, std::chrono::milliseconds(1)) ==
          FrameDelivery::Delivered) {
        // Every delivered frame must be whole, never a half-written buffer.
        if (frame.bgra.size() == size_t(32) * 32 * 4) ++delivered;
      }
  });
  std::this_thread::sleep_for(kShortWait);
  stop = true;
  producer.join();
  consumer.join();
  discarder.join();
  EXPECT_GT(delivered.load(), 0)
      << "a discarder running flat out must not starve delivery entirely";
}

}  // namespace
