#include <cstdlib>

#include "mistercast/interfaces.hpp"

namespace mistercast {
namespace {
std::string environmentValue(const char* name) {
  const char* value = std::getenv(name);
  return value ? value : std::string{};
}
}  // namespace

SessionEnvironment SessionEnvironment::current() {
  return {environmentValue("XDG_SESSION_TYPE"),
          environmentValue("WAYLAND_DISPLAY"), environmentValue("DISPLAY")};
}

CaptureBackend resolveCaptureBackend(CaptureBackend requested,
                                     const SessionEnvironment& session) {
  if (requested != CaptureBackend::Auto) return requested;
  if (!session.waylandDisplay.empty() || session.sessionType == "wayland")
    return CaptureBackend::Portal;
  // Neither variable set means nothing will capture. X11 is still the better
  // answer than the portal: its "DISPLAY is not set" failure names the one
  // thing the user can act on, where a portal failure would blame D-Bus.
  return CaptureBackend::X11;
}

std::unique_ptr<IVideoCapture> makeVideoCapture(CaptureBackend requested,
                                                PortalCaptureOptions portal) {
  if (resolveCaptureBackend(requested, SessionEnvironment::current()) ==
      CaptureBackend::Portal)
    return makePortalCapture(std::move(portal));
  return makeX11Capture();
}
}  // namespace mistercast
