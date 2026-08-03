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
  virtual std::vector<CaptureWindow> windows(std::string&) { return {}; }
  virtual bool start(const SourceOptions& source, ErrorCallback) = 0;
  // Geometry of the monitor or window chosen by start(), for computing crop.
  virtual Monitor selected() const = 0;
  // Restricts subsequent next() calls to this source-relative sub-rectangle,
  // so only the pixels that will actually be sent are transferred. An empty
  // rectangle captures the whole monitor.
  virtual void setRegion(const CropRect&) = 0;
  // Fills the frame with the current region. The frame buffer is reused, so
  // passing the same Frame back keeps the capture path allocation-free.
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
