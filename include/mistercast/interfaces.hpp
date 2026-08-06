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
  virtual bool start(const CaptureSource& source, ErrorCallback) = 0;
  // Geometry of the monitor or window chosen by start(), for computing crop.
  virtual SourceGeometry selectedGeometry() const = 0;
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
struct X11CaptureOptions {
  bool useShm{true};
  bool useComposite{true};
};
std::unique_ptr<IVideoCapture> makeX11Capture(X11CaptureOptions);
// Window discovery has a separate lifetime and concern from frame capture.
std::vector<Monitor> x11Monitors(std::string& error);
std::vector<CaptureWindow> x11CaptureWindows(std::string& error);
std::unique_ptr<IAudioCapture> makePulseAudioCapture();
std::vector<AudioSink> pulseAudioSinks(std::string& error);
inline constexpr const char* SilentAudioSink = "@mistercast-silent";
// Every silent-output null sink is named SilentSinkPrefix + pid of its owner,
// so leftovers from a crashed instance can be recognized and removed.
inline constexpr const char* SilentSinkPrefix = "mistercast_silent_";
// Unloads silent-output sinks whose owning process no longer exists. A killed
// or crashed instance leaves its module loaded in the sound server, where it
// would otherwise pollute output listings forever. Returns how many were
// removed; sinks of live processes (including this one) are left alone.
unsigned cleanupStaleSilentSinks(std::string& error);
}  // namespace mistercast
