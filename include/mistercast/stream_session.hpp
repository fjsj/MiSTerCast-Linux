#pragma once
#include "mistercast/groovy_transport.hpp"
#include "mistercast/interfaces.hpp"
#include "mistercast/audio_ring.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
namespace mistercast {
struct SessionStats {
 uint64_t capturedFrames{}, sentFrames{}, droppedFrames{}, audioDroppedSamples{}, audioUnderrunSamples{};
 size_t audioBufferedSamples{};
 double captureFps{}, streamFps{}, audioPeak{};
 bool misterAudioEnabled{};
};
class StreamSession {
public:
 using StateCallback=std::function<void(SessionState,const std::optional<SessionError>&)>;
 explicit StreamSession(std::unique_ptr<IVideoCapture> video=makeX11Capture(),std::unique_ptr<IAudioCapture> audio=makePulseAudioCapture());~StreamSession();
 bool start(const AppConfig&,StateCallback callback={},std::string* error=nullptr);void stop()noexcept;
 void setPreviewCallback(std::function<void(const Frame&)> callback){previewCallback_=std::move(callback);}
 SessionState state()const noexcept{return state_.load();}uint64_t droppedFrames()const noexcept{return dropped_.load();}
 SessionStats stats()const;
private:
 void setState(SessionState, std::optional<SessionError> = {});void fail(SessionError);void captureLoop();void renderLoop();void audioLoop();
 std::unique_ptr<IVideoCapture>video_;std::unique_ptr<IAudioCapture>audio_;GroovyTransport transport_;AudioRing audioRing_{192000};AppConfig config_;StateCallback callback_;std::function<void(const Frame&)>previewCallback_;std::atomic<SessionState>state_{SessionState::Idle};std::atomic<bool>stop_{false};std::atomic<uint64_t>dropped_{0},captured_{0},sent_{0},audioDropped_{0},audioUnderrun_{0};std::atomic<uint32_t>audioPeak_{0};std::chrono::steady_clock::time_point startedAt_{};std::mutex mutex_;std::condition_variable cv_;std::deque<Frame>frames_;std::thread captureThread_,renderThread_,audioThread_;
};
}
