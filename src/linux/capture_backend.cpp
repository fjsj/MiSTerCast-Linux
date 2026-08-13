#include <cstdlib>

#include "mistercast/config.hpp"
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

PortalCaptureOptions portalOptionsFromTokenFile(
    const std::string& tokenPath,
    std::function<void(const std::string&)> onWarning) {
  PortalCaptureOptions options;
  options.restoreToken = loadPortalRestoreToken(tokenPath);
  options.onRestoreToken = [tokenPath, warn = std::move(onWarning)](
                               const std::string& token) {
    std::string error;
    if (savePortalRestoreToken(token, tokenPath, error) || !warn) return;
    warn("Cannot remember the screen-sharing permission: " + error);
  };
  return options;
}

std::unique_ptr<IVideoCapture> makeVideoCapture(CaptureBackend requested,
                                                PortalCaptureOptions portal) {
  if (resolveCaptureBackend(requested, SessionEnvironment::current()) ==
      CaptureBackend::Portal)
    return makePortalCapture(std::move(portal));
  return makeX11Capture();
}
}  // namespace mistercast
