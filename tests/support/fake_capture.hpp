#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "mistercast/interfaces.hpp"

namespace mistercast::test {

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

  // Holds next() until the test hands out captures, so a finished frame can be
  // placed at a chosen point in the sender's cycle. The wait ignores the
  // caller's timeout on purpose: a timeout would send the capture loop around
  // its own retry path and re-request a frame, which is exactly the state a
  // gated test is trying to hold still.
  std::atomic<bool> gated{false};

  void release(uint32_t frames) {
    {
      std::lock_guard<std::mutex> lock(gateMutex_);
      credits_ += frames;
    }
    gate_.notify_all();
  }

  // True once every released capture has been taken and the capture thread is
  // parked in the gate again.
  bool parked() const {
    std::lock_guard<std::mutex> lock(gateMutex_);
    return waiting_ && !credits_;
  }

  bool next(Frame& out, std::chrono::milliseconds) override {
    if (gated) {
      std::unique_lock<std::mutex> lock(gateMutex_);
      waiting_ = true;
      gate_.wait(lock, [this] { return credits_ || !gated; });
      waiting_ = false;
      if (!credits_) return false;  // released by stop()
      --credits_;
    }
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
    gated = false;
    gate_.notify_all();
  }

  std::vector<CropRect> capturedRegions() const {
    std::lock_guard<std::mutex> lock(mutex);
    return regions;
  }

 private:
  std::atomic<uint32_t> attempts_{0};
  mutable std::mutex gateMutex_;
  std::condition_variable gate_;
  uint32_t credits_{0};
  bool waiting_{false};
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
