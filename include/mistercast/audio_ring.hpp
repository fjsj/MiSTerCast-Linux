#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>
namespace mistercast {
class AudioRing {
 public:
  explicit AudioRing(size_t capacitySamples);
  size_t push(const int16_t*, size_t);  // returns samples dropped
  size_t pop(int16_t*, size_t);  // zero-fills underrun, returns real samples
  size_t discard(size_t);        // drops oldest, returns samples dropped
  void reset();
  size_t size() const;

 private:
  mutable std::mutex mutex_;
  std::vector<int16_t> data_;
  size_t read_{}, write_{}, size_{};
};
}  // namespace mistercast
