#pragma once
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "mistercast/types.hpp"

namespace mistercast {
using ErrorCallback = std::function<void(SessionError)>;
class IVideoCapture {
 public:
  virtual ~IVideoCapture() = default;
  virtual std::vector<Monitor> monitors(std::string& error) = 0;
  virtual bool start(const std::string& monitor, ErrorCallback) = 0;
  virtual bool next(Frame&, std::chrono::milliseconds timeout) = 0;
  virtual void stop() noexcept = 0;
};
class IAudioCapture {
 public:
  virtual ~IAudioCapture() = default;
  virtual bool start(const std::string& sink, ErrorCallback) = 0;
  virtual bool next(PcmBlock&, std::chrono::milliseconds timeout) = 0;
  virtual void stop() noexcept = 0;
  virtual uint32_t sampleRate() const noexcept = 0;
};
std::unique_ptr<IVideoCapture> makeX11Capture();
std::unique_ptr<IAudioCapture> makePulseAudioCapture();
std::vector<AudioSink> pulseAudioSinks(std::string& error);
inline constexpr const char* SilentAudioSink = "@mistercast-silent";
}  // namespace mistercast
