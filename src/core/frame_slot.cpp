#include "mistercast/frame_slot.hpp"

#include <cstring>

namespace mistercast {
void FrameSlot::publish() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopped_) return;
  std::swap(staging_, held_);
  hasFrame_ = true;
  ready_.notify_all();
}

void FrameSlot::discard() {
  std::lock_guard<std::mutex> lock(mutex_);
  hasFrame_ = false;
}

void FrameSlot::fail(SessionError error) {
  std::lock_guard<std::mutex> lock(mutex_);
  // The first failure is the one worth reporting; later ones are usually its
  // consequences.
  if (!failure_) failure_ = std::move(error);
  ready_.notify_all();
}

void FrameSlot::reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  hasFrame_ = false;
  stopped_ = false;
  sequence_ = 0;
  failure_.reset();
}

void FrameSlot::stop() {
  std::lock_guard<std::mutex> lock(mutex_);
  stopped_ = true;
  ready_.notify_all();
}

FrameDelivery FrameSlot::next(Frame& out, std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (!hasFrame_ && !failure_ && !stopped_)
    ready_.wait_for(lock, timeout,
                    [this] { return hasFrame_ || failure_ || stopped_; });
  if (failure_) return FrameDelivery::Failed;
  if (!hasFrame_ || stopped_) return FrameDelivery::Empty;
  // The copy stays under the lock. That is what makes publish()'s swap a safe
  // handoff rather than a race: releasing the lock first to memcpy outside it
  // would let a swap land mid-copy. The same shortcut in the caller's crop is
  // what produced a stale-frame defect.
  out.width = held_.width;
  out.height = held_.height;
  out.stride = held_.stride;
  out.bgra.resize(held_.bgra.size());
  std::memcpy(out.bgra.data(), held_.bgra.data(), held_.bgra.size());
  out.sequence = ++sequence_;
  return FrameDelivery::Delivered;
}

std::optional<SessionError> FrameSlot::takeFailure() {
  std::lock_guard<std::mutex> lock(mutex_);
  auto failure = std::move(failure_);
  failure_.reset();
  return failure;
}
}  // namespace mistercast
