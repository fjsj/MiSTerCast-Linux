#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#include "mistercast/audio_ring.hpp"
#include "mistercast/groovy_transport.hpp"
#include "mistercast/interfaces.hpp"
namespace mistercast {
struct SessionStats {
  uint64_t capturedFrames{}, sentFrames{}, droppedFrames{},
      audioDroppedSamples{}, audioUnderrunSamples{};
  size_t audioBufferedSamples{};
  uint32_t audioSampleRate{};
  double captureFps{}, streamFps{}, audioPeak{};
  uint32_t acknowledgedFrame{}, fpgaFrame{};
  uint16_t syncLine{}, fpgaVCount{};
  uint64_t acknowledgedFrames{}, missedAcks{}, streamTimeUs{}, ackAgeMs{},
      sendErrors{}, networkRttUs{};
  int64_t rasterCorrectionUs{};
  bool misterAudioEnabled{}, vramSynced{}, vgaFrameskip{}, vgaVblank{};
};
class StreamSession {
 public:
  using StateCallback =
      std::function<void(SessionState, const std::optional<SessionError>&)>;
  explicit StreamSession(
      std::unique_ptr<IVideoCapture> video = makeX11Capture(),
      std::unique_ptr<IAudioCapture> audio = makePulseAudioCapture());
  ~StreamSession();
  bool start(const AppConfig&, StateCallback callback = {},
             std::string* error = nullptr);
  // Changes timings on a live stream. The switch is sent immediately before the
  // next frame, so its payload already matches the new active area.
  bool updateModeline(const Modeline&, bool progressiveInterlaceBuffer,
                      std::string* error = nullptr);
  void stop() noexcept;
  void setPreviewCallback(std::function<void(const Frame&)> callback) {
    previewCallback_ = std::move(callback);
  }
  SessionState state() const noexcept { return state_.load(); }
  uint64_t droppedFrames() const noexcept { return dropped_.load(); }
  SessionStats stats() const;

 private:
  void setState(SessionState, std::optional<SessionError> = {});
  void fail(SessionError);
  void captureLoop();
  void renderLoop();
  void audioLoop();
  std::unique_ptr<IVideoCapture> video_;
  std::unique_ptr<IAudioCapture> audio_;
  GroovyTransport transport_;
  AudioRing audioRing_{192000};
  AppConfig config_;
  StateCallback callback_;
  std::function<void(const Frame&)> previewCallback_;
  std::atomic<SessionState> state_{SessionState::Idle};
  std::atomic<bool> stop_{false};
  std::atomic<uint64_t> dropped_{0}, captured_{0}, sent_{0}, audioDropped_{0},
      audioUnderrun_{0};
  std::atomic<uint32_t> audioPeak_{0};
  std::chrono::steady_clock::time_point startedAt_{};
  // Monitor geometry the active crop was computed for. Set before the threads
  // start, then owned by the capture thread.
  Monitor cropMonitor_{};
  // Live modeline switching. config_ stays immutable once streaming, so the
  // timings both worker threads use live here instead. The rendering thread is
  // the only writer; the capture thread notices via the generation counter and
  // re-reads, since 1x-5x crops are relative to the active area.
  std::mutex configMutex_;
  Modeline activeModeline_, pendingModeline_;
  bool activeProgressiveBuffer_{}, pendingProgressiveBuffer_{};
  std::atomic<bool> modelineChangePending_{false};
  std::atomic<uint32_t> modelineGeneration_{0};
  std::mutex mutex_;
  std::condition_variable cv_;
  // Newest completed capture, waiting to be picked up. The rendering thread
  // swaps its own frame in, so the two buffers cycle between the threads and
  // neither allocates once they have grown to the capture size.
  Frame readyFrame_;
  bool frameReady_{false};
  bool captureRequested_{true};
  std::thread captureThread_, renderThread_, audioThread_;
};
}  // namespace mistercast
