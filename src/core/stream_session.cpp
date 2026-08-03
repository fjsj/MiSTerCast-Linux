#include "mistercast/stream_session.hpp"

#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>

#include "mistercast/audio_pacer.hpp"
#include "mistercast/transform.hpp"
namespace mistercast {
namespace {
// The capture, pacing, and audio loops all have to wake on time; on a loaded
// desktop the default scheduler will not guarantee that. Windows put its cast
// thread at THREAD_PRIORITY_HIGHEST, and round-robin at the lowest realtime
// priority is the closest equivalent that cannot monopolise a core. Both steps
// need privileges an ordinary desktop process usually lacks, so failure is
// silent and streaming simply continues on the normal scheduler.
void raiseThreadPriority() {
  sched_param parameters{};
  parameters.sched_priority = sched_get_priority_min(SCHED_RR) + 1;
  if (pthread_setschedparam(pthread_self(), SCHED_RR, &parameters) == 0) return;
  setpriority(PRIO_PROCESS, 0, -10);
}
}  // namespace
StreamSession::StreamSession(std::unique_ptr<IVideoCapture> v,
                             std::unique_ptr<IAudioCapture> a)
    : video_(std::move(v)), audio_(std::move(a)) {}
StreamSession::~StreamSession() { stop(); }
SessionStats StreamSession::stats() const {
  SessionStats result;
  result.capturedFrames = captured_;
  result.sentFrames = sent_;
  result.droppedFrames = dropped_;
  result.audioDroppedSamples = audioDropped_;
  result.audioUnderrunSamples = audioUnderrun_;
  result.audioBufferedSamples = audioRing_.size();
  result.audioSampleRate = config_.source.audio ? audio_->sampleRate() : 0;
  result.audioPeak = audioPeak_ / 32768.0;
  result.misterAudioEnabled = transport_.misterAudioEnabled();
  result.transformTimeUs = transformTimeUs_;
  result.transformMaxUs = transformMaxUs_;
  result.transport = transport_.stats();
  auto seconds = std::chrono::duration<double>(
                     std::chrono::steady_clock::now() - startedAt_)
                     .count();
  if (seconds > 0) {
    result.captureFps = result.capturedFrames / seconds;
    result.streamFps = result.sentFrames / seconds;
  }
  return result;
}
void StreamSession::setState(SessionState s, std::optional<SessionError> e) {
  state_ = s;
  if (callback_) callback_(s, e);
}
void StreamSession::fail(SessionError e) {
  stop_ = true;
  setState(SessionState::Error, e);
  cv_.notify_all();
}
bool StreamSession::start(const AppConfig& c, StateCallback cb,
                          std::string* err) {
  if (state_ != SessionState::Idle && state_ != SessionState::Error) {
    if (err) *err = "stream is already active";
    return false;
  }
  stop();
  callback_ = std::move(cb);
  config_ = c;
  if (auto e = c.validate()) {
    if (err) *err = *e;
    setState(SessionState::Error,
             SessionError{"config", *e, "Correct the settings and try again."});
    return false;
  }
  if (c.target.empty()) {
    if (err) *err = "target address is required";
    return false;
  }
  if (c.source.captureMode == CaptureMode::Window &&
      (!c.source.window || !c.source.window->id)) {
    if (err) *err = "a window must be selected for window capture";
    return false;
  }
  stop_ = false;
  dropped_ = captured_ = sent_ = audioDropped_ = audioUnderrun_ = audioPeak_ =
      0;
  transformTimeUs_ = transformMaxUs_ = 0;
  audioRing_.reset();
  {
    std::lock_guard<std::mutex> l(mutex_);
    readyFrame_ = {};
    frameReady_ = false;
    captureRequested_ = true;
  }
  setState(SessionState::Starting);
  auto onError = [this](SessionError e) { fail(std::move(e)); };
  if (!video_->start(c.source, onError)) {
    if (err) *err = "video capture initialization failed";
    setState(SessionState::Error);
    return false;
  }
  // Restrict capture to the crop up front so only the pixels that will be sent
  // are ever transferred out of the X server.
  const auto monitor = video_->selectedGeometry();
  CropRect crop;
  std::string cropError;
  if (!calculateCrop(monitor.width, monitor.height, c.source, c.modeline, crop,
                     cropError)) {
    video_->stop();
    if (err) *err = cropError;
    setState(SessionState::Error,
             SessionError{"video", cropError,
                          "Check the crop size, offsets, and capture source."});
    return false;
  }
  video_->setRegion(crop);
  cropGeometry_ = monitor;
  uint32_t rate = 48000;
  if (c.source.audio && !audio_->start(c.source.audioSink, onError)) {
    video_->stop();
    if (err) *err = "audio capture initialization failed";
    setState(SessionState::Error);
    return false;
  }
  if (c.source.audio) rate = audio_->sampleRate();
  std::string e;
  if (!transport_.open(c.target, rate, e) ||
      !transport_.switchMode(c.modeline, c.source.progressiveInterlaceBuffer,
                             e)) {
    audio_->stop();
    video_->stop();
    transport_.close();
    if (err) *err = e;
    setState(
        SessionState::Error,
        SessionError{"network", e,
                     "Verify the address, MiSTer core, and UDP port 32100."});
    return false;
  }
  transport_.setSyncOptions(c.source.syncRefresh, c.source.frameDelay);
  {
    std::lock_guard<std::mutex> l(configMutex_);
    activeModeline_ = c.modeline;
    activeProgressiveBuffer_ = c.source.progressiveInterlaceBuffer;
    modelineChangePending_ = false;
  }
  modelineGeneration_ = 0;
  startedAt_ = std::chrono::steady_clock::now();
  if (c.source.audio)
    audioThread_ = std::thread(&StreamSession::audioLoop, this);
  captureThread_ = std::thread(&StreamSession::captureLoop, this);
  renderThread_ = std::thread(&StreamSession::renderLoop, this);
  setState(SessionState::Streaming);
  return true;
}
bool StreamSession::updateModeline(const Modeline& m,
                                   bool progressiveInterlaceBuffer,
                                   std::string* err) {
  if (auto e = m.validate()) {
    if (err) *err = *e;
    return false;
  }
  if (state_ != SessionState::Streaming) {
    if (err) *err = "stream is not active";
    return false;
  }
  std::lock_guard<std::mutex> l(configMutex_);
  pendingModeline_ = m;
  pendingProgressiveBuffer_ = progressiveInterlaceBuffer;
  modelineChangePending_ = true;
  return true;
}
void StreamSession::stop() noexcept {
  auto s = state_.load();
  if (s == SessionState::Idle && !captureThread_.joinable() &&
      !renderThread_.joinable() && !audioThread_.joinable())
    return;
  if (s != SessionState::Error) setState(SessionState::Stopping);
  stop_ = true;
  cv_.notify_all();
  video_->stop();
  if (captureThread_.joinable() &&
      captureThread_.get_id() != std::this_thread::get_id())
    captureThread_.join();
  if (renderThread_.joinable() &&
      renderThread_.get_id() != std::this_thread::get_id())
    renderThread_.join();
  if (audioThread_.joinable() &&
      audioThread_.get_id() != std::this_thread::get_id())
    audioThread_.join();
  audio_->stop();
  transport_.close();
  {
    std::lock_guard<std::mutex> l(mutex_);
    readyFrame_ = {};
    frameReady_ = false;
  }
  setState(SessionState::Idle);
}
void StreamSession::captureLoop() {
  raiseThreadPriority();
  auto nextPreview = std::chrono::steady_clock::now();
  Frame working;
  Modeline cropModeline = config_.modeline;
  uint32_t cropGeneration = modelineGeneration_.load();
  while (!stop_) {
    {
      std::unique_lock<std::mutex> l(mutex_);
      cv_.wait(l, [&] { return stop_ || captureRequested_; });
      if (stop_) break;
      captureRequested_ = false;
    }
    // The monitor can be resized or replugged mid-stream and the modeline can
    // be switched live, either of which changes what the crop should be.
    // Recompute it rather than streaming a stale rectangle.
    const auto monitor = video_->selectedGeometry();
    const auto generation = modelineGeneration_.load();
    if (monitor.width != cropGeometry_.width ||
        monitor.height != cropGeometry_.height || generation != cropGeneration) {
      {
        std::lock_guard<std::mutex> l(configMutex_);
        cropModeline = activeModeline_;
      }
      CropRect crop;
      std::string cropError;
      if (calculateCrop(monitor.width, monitor.height, config_.source,
                        cropModeline, crop, cropError)) {
        video_->setRegion(crop);
        cropGeometry_ = monitor;
        cropGeneration = generation;
      }
    }
    if (!video_->next(working, std::chrono::milliseconds(100))) {
      if (!stop_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        std::lock_guard<std::mutex> l(mutex_);
        captureRequested_ = true;
        cv_.notify_all();
      }
      continue;
    }
    ++captured_;
    auto now = std::chrono::steady_clock::now();
    if (config_.source.preview && previewCallback_ && now >= nextPreview) {
      previewCallback_(working);
      nextPreview = now + std::chrono::milliseconds(100);
    }
    {
      std::lock_guard<std::mutex> l(mutex_);
      // Publish and take the previous buffer back in one swap.
      if (frameReady_) ++dropped_;
      std::swap(readyFrame_, working);
      frameReady_ = true;
    }
    cv_.notify_all();
  }
}
void StreamSession::renderLoop() {
  raiseThreadPriority();
  uint32_t number = 0;
  uint8_t field = 0;
  std::vector<uint8_t> rgb;
  std::vector<int16_t> audioSamples, audioSourceSamples;
  const auto audioRate = audio_->sampleRate();
  // Latency the consumer aims to keep buffered, the fine servo's dead band, and
  // the backlog above which the servo gives up on nudging and resynchronises.
  const size_t audioTargetValues = (size_t(audioRate) * 2 / 25) & ~size_t(1);
  const size_t audioHysteresisValues = size_t(audioRate / 50) & ~size_t(1);
  const size_t audioCeilingValues = audioTargetValues * 3;
  bool audioStarted = false;
  AudioPacer audioPacer(audioRate);
  if (config_.source.audio) audioSamples.reserve(32000);
  auto audioClock = std::chrono::steady_clock::now();
  Frame f;
  bool haveFrame = false;
  // This thread owns the timings in use, so it keeps its own copy and only
  // takes the lock when applying a change.
  auto source = config_.source;
  auto modeline = config_.modeline;
  while (!stop_) {
    {
      std::unique_lock<std::mutex> l(mutex_);
      // Only the very first frame is worth blocking for. From then on the
      // newest finished capture is taken if one is ready, and otherwise the
      // previous frame is sent again; waiting here would put the capture back
      // on the critical path in front of every blit.
      if (!haveFrame && !cv_.wait_for(l, std::chrono::seconds(5),
                                      [&] { return stop_ || frameReady_; })) {
        l.unlock();
        fail({"video", "no frame was captured within 5 seconds",
              "Check the selected source and X11 session."});
        break;
      }
      if (stop_) break;
      if (frameReady_) {
        std::swap(f, readyFrame_);
        frameReady_ = false;
        haveFrame = true;
      }
    }
    // Apply a live modeline change before building the frame, so the payload
    // already matches the active area the MiSTer now expects. Windows switched
    // after building the framebuffer, which sent one frame at the old size.
    if (modelineChangePending_.load()) {
      Modeline pending;
      bool progressive = false, apply = false;
      {
        std::lock_guard<std::mutex> l(configMutex_);
        if (modelineChangePending_) {
          pending = pendingModeline_;
          progressive = pendingProgressiveBuffer_;
          modelineChangePending_ = false;
          apply = true;
        }
      }
      if (apply) {
        std::string switchError;
        if (!transport_.switchMode(pending, progressive, switchError)) {
          fail({"stream", switchError,
                "Check the modeline and network connectivity."});
          break;
        }
        {
          std::lock_guard<std::mutex> l(configMutex_);
          activeModeline_ = pending;
          activeProgressiveBuffer_ = progressive;
        }
        modeline = pending;
        source.progressiveInterlaceBuffer = progressive;
        // Windows kept its frame counter running across a switch and only reset
        // the field; alignFrame realigns from the FPGA echo either way.
        field = 0;
        ++modelineGeneration_;
      }
    }
    ++number;
    transport_.alignFrame(number, field);
    std::string e;
    // The captured frame is already the crop region, so sample all of it.
    const CropRect wholeFrame{0, 0, f.width, f.height};
    const auto transformStarted = std::chrono::steady_clock::now();
    if (!transformRgb24(f, wholeFrame, source, modeline, field, rgb, e)) {
      fail({"stream", e, "Check capture geometry and network connectivity."});
      break;
    }
    const auto transformUs = uint64_t(std::chrono::duration_cast<
        std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                   transformStarted).count());
    const auto previousEwma = transformTimeUs_.load();
    transformTimeUs_ = previousEwma ? (previousEwma * 7 + transformUs) / 8
                                    : transformUs;
    auto previousMax = transformMaxUs_.load();
    while (previousMax < transformUs &&
           !transformMaxUs_.compare_exchange_weak(previousMax, transformUs)) {}
    if (config_.source.audio) {
      const auto now = std::chrono::steady_clock::now();
      if (!transport_.misterAudioEnabled()) {
        // The core has audio off, so sending would only waste bytes in the same
        // UDP stream that carries video. Keep the ring drained and the clock
        // current so that enabling audio later starts bounded rather than
        // seconds behind.
        audioRing_.discard(audioRing_.size());
        audioPacer.reset(audioRate);
        audioStarted = false;
        audioClock = now;
      } else {
        // A sink monitor can deliver nothing for the first 1-3 seconds after
        // connecting. Until real PCM has built up, send no audio at all:
        // silence would be inserted for seconds and, worse, the pacer would
        // count it as consumed and owe those samples, so the catch-up burst
        // became permanent backlog. Waiting here instead would stall video for
        // just as long. Prebuffering to exactly the steady-state target means
        // the servo has its dead band available immediately and nothing has to
        // be trimmed.
        if (!audioStarted && audioRing_.size() >= audioTargetValues) {
          audioStarted = true;
          audioPacer.reset(audioRate);
          audioClock = now;
        }
        if (!audioStarted) {
          audioClock = now;
        } else {
          const auto elapsed =
              std::chrono::duration_cast<std::chrono::nanoseconds>(now -
                                                                   audioClock)
                  .count();
          const auto count = audioPacer.valuesDue(
              uint64_t(std::max<int64_t>(elapsed, 0)), 32000);
          auto buffered = audioRing_.size();
          // The +-2 values per frame servo below is only strong enough for
          // clock drift; against a real backlog it would need minutes. Once the
          // buffer is far past target, drop the excess in one step so audio
          // latency stays bounded instead of growing until the ring overruns.
          if (buffered > audioCeilingValues) {
            audioDropped_ += audioRing_.discard(buffered - audioTargetValues);
            buffered = audioRing_.size();
          }
          const auto sourceCount = audioPacer.sourceValuesFor(
              count, buffered, audioTargetValues, audioHysteresisValues);
          audioSourceSamples.resize(sourceCount);
          auto real = audioRing_.pop(audioSourceSamples.data(), sourceCount);
          audioUnderrun_ += sourceCount - real;
          AudioPacer::conformStereo(audioSourceSamples.data(), sourceCount,
                                    audioSamples, count);
          audioClock = now;
          if (count && !transport_.sendAudio(audioSamples.data(),
                                             audioSamples.size(), e)) {
            fail({"audio", e, "Check the network and restart streaming."});
            break;
          }
        }
      }
    }
    if (!transport_.sendFrame(number, field, rgb, e)) {
      fail({"stream", e, "Check network connectivity."});
      break;
    }
    ++sent_;
    // Start the next capture before pacing, so it runs during the wait for the
    // MiSTer raster rather than in front of the next blit. alignFrame sets the
    // field from the FPGA state at the top of every iteration, so there is
    // nothing to advance here.
    {
      std::lock_guard<std::mutex> l(mutex_);
      captureRequested_ = true;
    }
    cv_.notify_all();
    transport_.waitSync();
  }
}
void StreamSession::audioLoop() {
  raiseThreadPriority();
  while (!stop_) {
    PcmBlock b;
    if (!audio_->next(b, std::chrono::milliseconds(100))) {
      if (!stop_) std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }
    uint32_t peak = 0;
    for (auto sample : b.samples) {
      auto magnitude =
          sample == INT16_MIN ? 32768u : uint32_t(std::abs(int(sample)));
      peak = std::max(peak, magnitude);
    }
    audioPeak_ = peak;
    audioDropped_ += audioRing_.push(b.samples.data(), b.samples.size());
  }
}
}  // namespace mistercast
