#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <numeric>
#include <thread>
#include <vector>

#include "mistercast/audio_pacer.hpp"
#include "mistercast/audio_ring.hpp"

using namespace mistercast;
using testing::ElementsAre;

namespace {

std::vector<int16_t> popped(AudioRing& ring, size_t count) {
  std::vector<int16_t> out(count, -1);
  ring.pop(out.data(), count);
  return out;
}

TEST(AudioRing, StartsEmpty) {
  AudioRing ring(8);
  EXPECT_EQ(ring.size(), 0u);
  EXPECT_THAT(popped(ring, 2), ElementsAre(0, 0));
}

TEST(AudioRing, ACapacityOfZeroStillHoldsOneSample) {
  AudioRing ring(0);
  const int16_t values[] = {5, 6};
  EXPECT_EQ(ring.push(values, 2), 1u);
  EXPECT_EQ(ring.size(), 1u);
  EXPECT_THAT(popped(ring, 1), ElementsAre(6));
}

TEST(AudioRing, PopReturnsRealSampleCountAndZeroFillsTheRest) {
  AudioRing ring(4);
  const int16_t values[] = {1, 2, 3};
  ASSERT_EQ(ring.push(values, 3), 0u);
  int16_t out[4]{};
  EXPECT_EQ(ring.pop(out, 4), 3u);
  EXPECT_THAT(out, ElementsAre(1, 2, 3, 0));
  EXPECT_EQ(ring.size(), 0u);
}

TEST(AudioRing, OverrunDropsTheOldestSamples) {
  AudioRing ring(4);
  const int16_t values[] = {1, 2, 3, 4, 5};
  EXPECT_EQ(ring.push(values, 5), 1u);
  EXPECT_EQ(ring.size(), 4u);
  EXPECT_THAT(popped(ring, 4), ElementsAre(2, 3, 4, 5));
}

TEST(AudioRing, DiscardDropsTheOldestAndIsClampedToTheContents) {
  AudioRing ring(4);
  const int16_t values[] = {7, 8, 9, 10};
  ASSERT_EQ(ring.push(values, 4), 0u);
  EXPECT_EQ(ring.discard(2), 2u);
  EXPECT_EQ(ring.size(), 2u);
  EXPECT_THAT(popped(ring, 2), ElementsAre(9, 10));

  EXPECT_EQ(ring.discard(5), 0u) << "discarding an empty ring drops nothing";
  ASSERT_EQ(ring.push(values, 4), 0u);
  EXPECT_EQ(ring.discard(9), 4u) << "discard is clamped to what is buffered";
  EXPECT_EQ(ring.size(), 0u);
}

TEST(AudioRing, WrapsAroundWithoutLosingOrder) {
  AudioRing ring(4);
  const int16_t first[] = {1, 2, 3};
  ASSERT_EQ(ring.push(first, 3), 0u);
  int16_t drained[2]{};
  ASSERT_EQ(ring.pop(drained, 2), 2u);
  const int16_t second[] = {4, 5, 6};
  ASSERT_EQ(ring.push(second, 3), 0u);
  EXPECT_EQ(ring.size(), 4u);
  EXPECT_THAT(popped(ring, 4), ElementsAre(3, 4, 5, 6));
}

TEST(AudioRing, ResetDropsEverythingBuffered) {
  AudioRing ring(4);
  const int16_t values[] = {1, 2, 3};
  ASSERT_EQ(ring.push(values, 3), 0u);
  ring.reset();
  EXPECT_EQ(ring.size(), 0u);
  const int16_t more[] = {9};
  ASSERT_EQ(ring.push(more, 1), 0u);
  EXPECT_THAT(popped(ring, 1), ElementsAre(9));
}

TEST(AudioRing, ConcurrentProducerAndConsumerNeitherLoseNorDuplicate) {
  // The ring is the hand-off between the audio thread and the rendering thread,
  // so the locking has to hold under real contention rather than only in
  // single-threaded use.
  AudioRing ring(4096);
  constexpr size_t kBlocks = 500, kBlockValues = 64;
  std::atomic<uint64_t> consumed{0}, dropped{0};
  std::thread producer([&] {
    std::vector<int16_t> block(kBlockValues, 1);
    for (size_t i = 0; i < kBlocks; ++i)
      dropped += ring.push(block.data(), block.size());
  });
  std::thread consumer([&] {
    std::vector<int16_t> block(kBlockValues);
    while (consumed + dropped < kBlocks * kBlockValues)
      consumed += ring.pop(block.data(), block.size());
  });
  producer.join();
  consumer.join();
  EXPECT_EQ(consumed + dropped + ring.size(), kBlocks * kBlockValues);
}

TEST(AudioPacer, DeliversWholeStereoFramesProportionalToElapsedTime) {
  AudioPacer pacer(48000);
  uint64_t pacedValues = 0, totalNs = 0;
  for (int i = 0; i < 1000; ++i) {
    const uint64_t elapsed = 16682885 + uint64_t(i % 3);
    totalNs += elapsed;
    const auto due = pacer.valuesDue(elapsed, 32000);
    ASSERT_EQ(due % 2, 0u) << "a partial stereo frame must never be produced";
    pacedValues += due;
  }
  // Fractional nanoseconds are carried across calls, so 1000 irregular frame
  // periods still add up to exactly the sample count the elapsed time implies.
  EXPECT_EQ(pacedValues, (totalNs * 48000 / 1000000000) * 2);
}

TEST(AudioPacer, WholeSecondsAndRemaindersAreAccountedSeparately) {
  AudioPacer pacer(48000);
  EXPECT_EQ(pacer.valuesDue(1000000000, 200000), 96000u);
  pacer.reset(48000);
  EXPECT_EQ(pacer.valuesDue(2500000000ull, 400000), 240000u);
}

TEST(AudioPacer, ClampsToTheCallersLimitAndOwesTheRemainder) {
  AudioPacer pacer(48000);
  EXPECT_EQ(pacer.valuesDue(1000000000, 32000), 32000u);
  // The samples that did not fit are still owed and come out of the next call
  // even though no further time elapsed.
  EXPECT_EQ(pacer.valuesDue(0, 100000), 64000u);
  EXPECT_EQ(pacer.valuesDue(0, 100000), 0u);
}

TEST(AudioPacer, ResetForgetsOwedSamplesAndAdoptsTheNewRate) {
  AudioPacer pacer(48000);
  ASSERT_EQ(pacer.valuesDue(1000000000, 32000), 32000u);
  pacer.reset(22050);
  EXPECT_EQ(pacer.valuesDue(0, 100000), 0u) << "owed samples are forgotten";
  EXPECT_EQ(pacer.valuesDue(1000000000, 100000), 44100u);
}

TEST(AudioPacer, AnOddLimitStillYieldsOnlyCompleteFrames) {
  AudioPacer pacer(48000);
  EXPECT_EQ(pacer.valuesDue(1000000000, 7), 6u);
}

TEST(AudioPacer, ServoAsksForMoreSourceWhenTheBufferIsAboveTarget) {
  const AudioPacer pacer(48000);
  EXPECT_EQ(pacer.sourceValuesFor(8, 2500, 1600, 800), 10u);
}

TEST(AudioPacer, ServoAsksForLessSourceWhenTheBufferIsBelowTarget) {
  const AudioPacer pacer(48000);
  EXPECT_EQ(pacer.sourceValuesFor(8, 700, 1600, 800), 6u);
}

TEST(AudioPacer, ServoLeavesTheDeadBandAlone) {
  const AudioPacer pacer(48000);
  EXPECT_EQ(pacer.sourceValuesFor(8, 1600, 1600, 800), 8u);
  EXPECT_EQ(pacer.sourceValuesFor(8, 2400, 1600, 800), 8u)
      << "exactly target + hysteresis is still inside the dead band";
  EXPECT_EQ(pacer.sourceValuesFor(8, 800, 1600, 800), 8u)
      << "exactly target - hysteresis is still inside the dead band";
}

TEST(AudioPacer, ServoNeverUnderflowsATinyOutputBlock) {
  const AudioPacer pacer(48000);
  EXPECT_EQ(pacer.sourceValuesFor(0, 0, 1600, 800), 0u);
  EXPECT_EQ(pacer.sourceValuesFor(2, 0, 1600, 800), 2u)
      << "a single frame cannot be trimmed to zero";
}

TEST(ConformStereo, CopiesAnExactMatchUnchanged) {
  const int16_t source[] = {1, 2, 3, 4};
  std::vector<int16_t> output;
  AudioPacer::conformStereo(source, 4, output, 4);
  EXPECT_THAT(output, ElementsAre(1, 2, 3, 4));
}

TEST(ConformStereo, AnEmptyRequestProducesNothing) {
  const int16_t source[] = {1, 2};
  std::vector<int16_t> output{9, 9};
  AudioPacer::conformStereo(source, 2, output, 0);
  EXPECT_TRUE(output.empty());
}

TEST(ConformStereo, MissingSourceBecomesSilence) {
  std::vector<int16_t> output;
  AudioPacer::conformStereo(nullptr, 0, output, 4);
  EXPECT_THAT(output, ElementsAre(0, 0, 0, 0));
}

TEST(ConformStereo, DropsOneFrameNearTheMiddleWhenTheSourceIsOneFrameLong) {
  const int16_t source[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  std::vector<int16_t> output;
  AudioPacer::conformStereo(source, 10, output, 8);
  // Frame 2 (values 4,5) is the one dropped, keeping both channels aligned.
  EXPECT_THAT(output, ElementsAre(0, 1, 2, 3, 6, 7, 8, 9));
}

TEST(ConformStereo, RepeatsOneFrameNearTheMiddleWhenTheSourceIsOneFrameShort) {
  const int16_t source[] = {0, 1, 2, 3, 4, 5};
  std::vector<int16_t> output;
  AudioPacer::conformStereo(source, 6, output, 8);
  // The pivot frame (values 4,5) is the one repeated, keeping both channels
  // aligned and the seam away from the block boundary.
  EXPECT_THAT(output, ElementsAre(0, 1, 2, 3, 4, 5, 4, 5));
}

TEST(ConformStereo, ATinySourceStillPivotsInsideItsOwnBounds) {
  const int16_t source[] = {11, 12};
  std::vector<int16_t> output;
  AudioPacer::conformStereo(source, 2, output, 4);
  EXPECT_THAT(output, ElementsAre(11, 12, 11, 12));
}

TEST(ConformStereo, LargerMismatchesFallBackToCopyThenSilence) {
  const int16_t source[] = {1, 2, 3, 4};
  std::vector<int16_t> output;
  AudioPacer::conformStereo(source, 4, output, 10);
  EXPECT_THAT(output, ElementsAre(1, 2, 3, 4, 0, 0, 0, 0, 0, 0));

  // Two frames fewer: too far apart for the single-frame servo, so the block is
  // truncated rather than resampled.
  const int16_t longer[] = {1, 2, 3, 4, 5, 6, 7, 8};
  AudioPacer::conformStereo(longer, 8, output, 2);
  EXPECT_THAT(output, ElementsAre(1, 2));
}

TEST(ConformStereo, KeepsBothChannelsOfAToneIdentical) {
  std::vector<int16_t> source(64);
  for (size_t i = 0; i < source.size(); i += 2)
    source[i] = source[i + 1] = int16_t(i * 100);
  std::vector<int16_t> output;
  for (size_t requested : {62u, 64u, 66u}) {
    AudioPacer::conformStereo(source.data(), source.size(), output, requested);
    ASSERT_EQ(output.size(), requested);
    for (size_t i = 0; i < output.size(); i += 2)
      ASSERT_EQ(output[i], output[i + 1]) << "at value " << i;
  }
}

}  // namespace
