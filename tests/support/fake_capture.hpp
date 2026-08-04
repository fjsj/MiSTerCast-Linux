#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "mistercast/interfaces.hpp"

namespace mistercast::test {

// Holds a capture inside next() until a test hands one out, so a finished frame
// can be placed at a chosen point in the sender's cycle.
//
// Credits count next() calls rather than successful captures, matching the unit
// the session itself uses: captureLoop clears captureRequested_ once per
// capture attempt, not once per frame that arrives. A released call therefore
// still runs the fake's other knobs and may fail for their reasons, which also
// keeps the credit arithmetic independent of how those knobs are configured.
class CaptureGate {
 public:
  // Arm before starting the threads that will wait on the gate. disarm() is a
  // one-shot, so a session stopped and restarted against the same fake runs
  // ungated the second time unless the test arms it again.
  void arm() {
    std::lock_guard<std::mutex> lock(mutex_);
    armed_ = true;
  }

  // Hands out `frames` captures to the waiter.
  void release(uint32_t frames) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      credits_ += frames;
    }
    handout_.notify_all();
  }

  // True once every released capture has been taken and the waiter is parked in
  // the gate again. False for the whole of an in-flight capture, so a test that
  // waits on this knows every released frame has already been published.
  bool parked() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return waiting_ && !credits_;
  }

  // Blocks until a capture is handed out. False means disarm() released the
  // waiter for shutdown rather than a capture arriving.
  //
  // This ignores the timeout its caller was given on purpose: timing out would
  // send the capture loop around its own retry path and re-request a frame,
  // undoing the state a gated test is holding still. Blocking past that
  // deadline is more than a real X11Capture would do, but it is only a lever —
  // the interleaving it reproduces needs a stalled capture followed by a fast
  // one, which a 30 ms hiccup and a 1 ms frame deliver well inside the timeout.
  bool take() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!armed_) return true;
    waiting_ = true;
    handout_.wait(lock, [this] { return credits_ || !armed_; });
    waiting_ = false;
    if (!credits_) return false;
    --credits_;
    return true;
  }

  // Disarms the gate and releases a parked waiter, so the session can join its
  // capture thread. Every later take() passes straight through.
  //
  // `armed_` is part of take()'s wait predicate, so it must be written while
  // holding the mutex. Writing it outside admits a lost wakeup: the waiter can
  // evaluate the predicate under the lock and be pre-empted before it blocks,
  // and then never sees this notify. That wedges the waiter inside next()
  // forever and hangs the session's captureThread_.join() until ctest's
  // suite timeout, which buries whatever the test was actually reporting.
  void disarm() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      armed_ = false;
    }
    handout_.notify_all();
  }

 private:
  // Every field lives under this one mutex, `armed_` included, so no caller
  // can reintroduce the lost wakeup above by touching a predicate field
  // unlocked. An unarmed take() therefore costs a mutex acquisition rather
  // than an atomic load: measured at 4.2 ns, which against the ~60 next()
  // calls a second the capture loop makes is 0.25 us per second of streaming.
  mutable std::mutex mutex_;
  std::condition_variable handout_;
  uint32_t credits_{0};
  bool armed_{false}, waiting_{false};
};

// Synthetic capture sources, so session behaviour that depends on the monitor
// changing, on capture stalling, or on the core's audio bit can be driven
// deterministically. These stand in for the X11 and PulseAudio devices at the
// same interface the production session consumes; the real implementations are
// exercised separately by the x11 and pulse suites.
class FakeVideo final : public IVideoCapture {
 public:
  std::atomic<uint16_t> width{1920}, height{1080};
  std::atomic<uint32_t> captured{0}, starts{0}, stops{0};
  std::atomic<bool> produce{true}, startSucceeds{true};
  // Holds next() so a test can park a capture mid-cycle. Unarmed by default.
  CaptureGate gate;
  // Reports a fatal capture error through the session's error callback the way
  // X11Capture does after a sustained failure.
  std::atomic<bool> reportError{false};
  // When non-zero, every Nth call fails, standing in for the transient X11
  // faults (resize, replug, stale shared segment) that cost frames but not the
  // session.
  std::atomic<uint32_t> failEvery{0};
  mutable std::mutex mutex;
  std::vector<CropRect> regions;
  CropRect region{};
  CaptureSource lastSource;
  ErrorCallback errorCallback;

  bool start(const CaptureSource& source, ErrorCallback callback) override {
    ++starts;
    std::lock_guard<std::mutex> lock(mutex);
    lastSource = source;
    errorCallback = std::move(callback);
    return startSucceeds;
  }

  SourceGeometry selectedGeometry() const override {
    return {width.load(), height.load()};
  }

  void setRegion(const CropRect& value) override {
    std::lock_guard<std::mutex> lock(mutex);
    region = value;
    regions.push_back(value);
  }

  bool next(Frame& out, std::chrono::milliseconds) override {
    if (!gate.take()) return false;  // released by stop()
    if (reportError.exchange(false)) {
      ErrorCallback callback;
      {
        std::lock_guard<std::mutex> lock(mutex);
        callback = errorCallback;
      }
      if (callback)
        callback({"video", "X11 capture kept failing",
                  "Check the monitor selection and X11 session."});
      return false;
    }
    if (!produce) return false;
    const auto attempt = ++attempts_;
    if (failEvery && attempt % failEvery == 0) return false;
    CropRect rectangle;
    {
      std::lock_guard<std::mutex> lock(mutex);
      rectangle = region;
    }
    if (!rectangle.width || !rectangle.height) return false;
    out.width = rectangle.width;
    out.height = rectangle.height;
    out.stride = rectangle.width * 4;
    out.bgra.assign(size_t(out.stride) * out.height, 128);
    ++captured;
    return true;
  }

  void stop() noexcept override {
    ++stops;
    gate.disarm();
  }

  std::vector<CropRect> capturedRegions() const {
    std::lock_guard<std::mutex> lock(mutex);
    return regions;
  }

 private:
  std::atomic<uint32_t> attempts_{0};
};

class FakeAudio final : public IAudioCapture {
 public:
  std::atomic<bool> produce{false}, startSucceeds{true};
  std::atomic<uint32_t> starts{0}, stops{0};
  std::atomic<uint32_t> rate{48000};
  // Values per 10 ms block, and the constant written into every sample.
  std::atomic<size_t> valuesPerBlock{960};
  std::atomic<int16_t> sampleValue{1000};
  std::atomic<std::chrono::milliseconds::rep> blockDelayMs{10};
  std::string lastSink;

  bool start(const std::string& sink, ErrorCallback) override {
    ++starts;
    lastSink = sink;
    return startSucceeds;
  }

  bool next(PcmBlock& block, std::chrono::milliseconds) override {
    if (!produce) return false;
    block.sampleRate = rate;
    block.samples.assign(valuesPerBlock, sampleValue);
    std::this_thread::sleep_for(std::chrono::milliseconds(blockDelayMs));
    return true;
  }

  void stop() noexcept override { ++stops; }
  uint32_t sampleRate() const noexcept override { return rate; }
};

}  // namespace mistercast::test
