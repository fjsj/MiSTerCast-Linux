#pragma once

#include <chrono>
#include <thread>

namespace mistercast::test {

// Sanitizer and coverage builds run the pacing loop far slower than release, so
// tests wait on a condition with a generous deadline rather than sleeping for a
// fixed time or assuming a frame count was reached.
template <class Predicate>
bool waitFor(Predicate ready, std::chrono::milliseconds timeout =
                                  std::chrono::milliseconds(15000)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (ready()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return ready();
}

}  // namespace mistercast::test
