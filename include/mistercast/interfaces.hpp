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

// The desktop session as backend selection sees it. Passed in rather than read
// from the environment inside resolveCaptureBackend, so the rules are testable
// without mutating the process environment.
struct SessionEnvironment {
  std::string sessionType;     // XDG_SESSION_TYPE
  std::string waylandDisplay;  // WAYLAND_DISPLAY
  std::string display;         // DISPLAY
  static SessionEnvironment current();
};
// Resolves Auto against the session and returns the requested backend
// unchanged otherwise. Never returns Auto. A Wayland session wins over a set
// DISPLAY, because that DISPLAY is XWayland's: it accepts a connection and then
// hands out a black or empty root window instead of the desktop.
CaptureBackend resolveCaptureBackend(CaptureBackend requested,
                                     const SessionEnvironment&);
// Whether this build has the ScreenCast portal and PipeWire backend compiled
// in. False means makePortalCapture() still returns a capture object, but
// start() fails with a message naming the missing build dependencies.
bool portalCaptureAvailable() noexcept;
struct PortalCaptureOptions {
  CapturePreference preference{CapturePreference::Monitor};
  // A token from a previous grant. When the portal accepts it, capture starts
  // without a picker dialog; when it rejects it, the picker is shown instead.
  std::string restoreToken;
  // Called with the token the portal issued for this grant, from start(), so
  // the caller can persist it. Empty when the portal granted none.
  std::function<void(const std::string&)> onRestoreToken;
  bool embedCursor{false};
};
std::unique_ptr<IVideoCapture> makePortalCapture(PortalCaptureOptions = {});
// Portal options wired to a token file: the stored grant is offered to the
// portal, and a newly issued one is written back the moment it arrives. Lives
// here rather than in each frontend because the CLI and the GUI want exactly the
// same behaviour and only differ in where a warning goes. onWarning is called
// with a ready-made message when the token cannot be stored, which costs a
// dialog on the next run and nothing else.
PortalCaptureOptions portalOptionsFromTokenFile(
    const std::string& tokenPath,
    std::function<void(const std::string&)> onWarning);
// The backend-aware factory. Resolves the backend against the current session
// and builds the matching capture; the returned object has not touched the
// display server, portal, or PipeWire yet, so constructing it is free of
// side effects.
std::unique_ptr<IVideoCapture> makeVideoCapture(
    CaptureBackend requested = CaptureBackend::Auto,
    PortalCaptureOptions portal = {});
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
