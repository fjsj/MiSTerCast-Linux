#pragma once
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>

#include "mistercast/types.hpp"

namespace mistercast {
// Hands one frame from a producer that pushes to a consumer that pulls.
//
// A ScreenCast portal delivers frames when the compositor produces them, while
// the MiSTer raster asks for one per refresh. Reconciling those two rates needs
// a handful of rules that are easy to get wrong and were expensive to get right,
// so they live here rather than inside the PipeWire code: nothing in this file
// needs a compositor, so all of it is testable.
enum class FrameDelivery : uint8_t { Delivered, Empty, Failed };

class FrameSlot {
 public:
  // The producer's own buffer. Nothing reads it until publish(), so filling it
  // takes no lock and cannot stall a consumer mid-copy.
  Frame& staging() noexcept { return staging_; }
  // Makes staging() the held frame. Swaps rather than copies, so a producer that
  // keeps filling the same buffer never allocates again.
  void publish();
  // Drops the held frame, for when it no longer describes what the consumer
  // expects: the crop changed, or the source renegotiated its format. The
  // alternative is handing back a frame whose geometry disagrees with the
  // caller's, which is worse than handing back nothing.
  void discard();
  // Recorded once and reported by the next next(). Producers run on their own
  // threads, and reporting from there would put session teardown on a callback
  // thread that is about to be joined.
  void fail(SessionError);
  // Back to empty and accepting frames again.
  void reset();
  // Wakes every waiter and refuses further delivery, so a consumer blocked on
  // the first frame does not hold up teardown for its whole timeout.
  void stop();

  // Waits only when nothing is held yet. Once a frame is in hand it is returned
  // again immediately: a compositor emits nothing at all while the screen is
  // still, and the MiSTer needs a frame every refresh regardless. Waiting for a
  // newer one instead spends the caller's whole timeout to deliver the frame it
  // could have had at once, which measured as capture running at 24 fps against
  // a 60 Hz stream.
  FrameDelivery next(Frame& out, std::chrono::milliseconds timeout);
  // The recorded failure, cleared by reading it.
  std::optional<SessionError> takeFailure();

 private:
  mutable std::mutex mutex_;
  std::condition_variable ready_;
  Frame staging_, held_;
  bool hasFrame_{false}, stopped_{false};
  uint64_t sequence_{};
  std::optional<SessionError> failure_;
};
}  // namespace mistercast
