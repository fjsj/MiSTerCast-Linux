#include "mistercast/stream_session.hpp"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>

#include "mistercast/audio_pacer.hpp"
#include "mistercast/transform.hpp"
namespace mistercast {
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
  auto network = transport_.stats();
  result.acknowledgedFrame = network.acknowledgedFrame;
  result.fpgaFrame = network.fpgaFrame;
  result.syncLine = network.requestedSyncLine;
  result.fpgaVCount = network.fpgaVCount;
  result.acknowledgedFrames = network.acknowledgedFrames;
  result.missedAcks = network.missedAcks;
  result.streamTimeUs = network.streamTimeUs;
  result.ackAgeMs = network.ackAgeMs;
  result.rasterCorrectionUs = network.rasterCorrectionUs;
  result.vramSynced = network.vramSynced;
  result.vgaFrameskip = network.vgaFrameskip;
  result.vgaVblank = network.vgaVblank;
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
  stop_ = false;
  dropped_ = captured_ = sent_ = audioDropped_ = audioUnderrun_ = audioPeak_ =
      0;
  audioRing_.reset();
  {
    std::lock_guard<std::mutex> l(mutex_);
    readyFrame_ = {};
    frameReady_ = false;
    captureRequested_ = true;
  }
  setState(SessionState::Starting);
  auto onError = [this](SessionError e) { fail(std::move(e)); };
  if (!video_->start(c.source.monitor, onError)) {
    if (err) *err = "video capture initialization failed";
    setState(SessionState::Error);
    return false;
  }
  // Restrict capture to the crop up front so only the pixels that will be sent
  // are ever transferred out of the X server.
  const auto monitor = video_->selected();
  CropRect crop;
  std::string cropError;
  if (!calculateCrop(monitor.width, monitor.height, c.source, c.modeline, crop,
                     cropError)) {
    video_->stop();
    if (err) *err = cropError;
    setState(SessionState::Error,
             SessionError{"video", cropError,
                          "Check the crop size, offsets, and monitor."});
    return false;
  }
  video_->setRegion(crop);
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
  startedAt_ = std::chrono::steady_clock::now();
  if (c.source.audio)
    audioThread_ = std::thread(&StreamSession::audioLoop, this);
  captureThread_ = std::thread(&StreamSession::captureLoop, this);
  renderThread_ = std::thread(&StreamSession::renderLoop, this);
  setState(SessionState::Streaming);
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
  auto nextPreview = std::chrono::steady_clock::now();
  Frame working;
  while (!stop_) {
    {
      std::unique_lock<std::mutex> l(mutex_);
      cv_.wait(l, [&] { return stop_ || captureRequested_; });
      if (stop_) break;
      captureRequested_ = false;
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
  uint32_t number = 0;
  uint8_t field = 0;
  std::vector<uint8_t> rgb;
  std::vector<int16_t> audioSamples, audioSourceSamples;
  bool firstAudio = true;
  AudioPacer audioPacer(audio_->sampleRate());
  if (config_.source.audio) {
    audioSamples.reserve(32000);
    const auto prebufferSamples = audio_->sampleRate() / 10;
    const auto timeout =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    while (!stop_ && audioRing_.size() < prebufferSamples &&
           std::chrono::steady_clock::now() < timeout)
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  auto audioClock = std::chrono::steady_clock::now();
  Frame f;
  bool haveFrame = false;
  while (!stop_) {
    {
      std::unique_lock<std::mutex> l(mutex_);
      // Only the very first frame is worth blocking for. From then on the
      // newest finished capture is taken if one is ready, and otherwise the
      // previous frame is sent again; waiting here would put the capture back
      // on the critical path in front of every blit.
      if (!haveFrame &&
          !cv_.wait_for(l, std::chrono::seconds(5),
                        [&] { return stop_ || frameReady_; })) {
        l.unlock();
        fail({"video", "no frame was captured within 5 seconds",
              "Check the monitor selection and X11 session."});
        break;
      }
      if (stop_) break;
      if (frameReady_) {
        std::swap(f, readyFrame_);
        frameReady_ = false;
        haveFrame = true;
      }
    }
    ++number;
    transport_.alignFrame(number, field);
    std::string e;
    // The captured frame is already the crop region, so sample all of it.
    const CropRect wholeFrame{0, 0, f.width, f.height};
    if (!transformRgb24(f, wholeFrame, config_.source, config_.modeline, field,
                        rgb, e)) {
      fail({"stream", e, "Check capture geometry and network connectivity."});
      break;
    }
    if (config_.source.audio) {
      auto now = std::chrono::steady_clock::now();
      size_t count;
      if (firstAudio) {
        const auto lineNs =
            uint64_t(std::llround(double(config_.modeline.hTotal) * 1000.0 /
                                  config_.modeline.pixelClockMHz));
        const auto cycleNs = lineNs * config_.modeline.vTotal /
                             (config_.modeline.interlaced ? 2 : 1);
        count = audioPacer.valuesDue(cycleNs, 32000);
        firstAudio = false;
        audioSamples.resize(count);
        auto real = audioRing_.pop(audioSamples.data(), count);
        audioUnderrun_ += count - real;
      } else {
        auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           now - audioClock)
                           .count();
        count = audioPacer.valuesDue(uint64_t(std::max<int64_t>(elapsed, 0)),
                                     32000);
        const auto buffered = audioRing_.size();
        const auto rate = audio_->sampleRate();
        const auto targetValues = (size_t(rate) * 2 / 25) & ~size_t(1);
        const auto hysteresisValues = size_t(rate / 50) & ~size_t(1);
        const auto sourceCount = audioPacer.sourceValuesFor(
            count, buffered, targetValues, hysteresisValues);
        audioSourceSamples.resize(sourceCount);
        auto real = audioRing_.pop(audioSourceSamples.data(), sourceCount);
        audioUnderrun_ += sourceCount - real;
        AudioPacer::conformStereo(audioSourceSamples.data(), sourceCount,
                                  audioSamples, count);
      }
      audioClock = now;
      if (count &&
          !transport_.sendAudio(audioSamples.data(), audioSamples.size(), e)) {
        fail({"audio", e, "Check the network and restart streaming."});
        break;
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
