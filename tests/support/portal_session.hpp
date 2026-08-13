#pragma once

#include <cstdio>
#include <cstdlib>

#include "mistercast/interfaces.hpp"

namespace mistercast::test {

// Why a portal suite cannot run here, or nullptr when it can.
//
// Shared by every suite that reaches a real ScreenCast portal, because they all
// need the same four answers and had already drifted apart when each kept its
// own copy: one of them checked an environment variable that nothing sets.
inline const char* portalSessionUnavailable() {
  // Opt-in, never inferred. A granted portal session captures the screen of
  // whoever runs the suite, and on a Wayland desktop with a grant already stored
  // it succeeds silently -- so `ctest` on a developer's own machine would record
  // their screen, or stop to ask them for it. The rig sets this; nothing else
  // should.
  if (!std::getenv("MISTERCAST_PORTAL_TESTS"))
    return "not opted in (set MISTERCAST_PORTAL_TESTS=1; these tests capture "
           "the screen of whoever runs them)";
  if (!portalCaptureAvailable())
    return "built without libpipewire/libsystemd";
  // Selection is what decides whether a portal is worth trying at all, so this
  // skips on exactly the condition that would make the backend unusable in
  // production rather than on a probe of its own.
  if (resolveCaptureBackend(CaptureBackend::Auto,
                            SessionEnvironment::current()) !=
      CaptureBackend::Portal)
    return "not running in a Wayland session";
  if (!std::getenv("DBUS_SESSION_BUS_ADDRESS"))
    return "no session bus to reach a portal on";
  // Set by the test rig when its own portal backend cannot stream in the
  // environment it is running in; see packaging/docker/wayland-test-session.sh.
  // Nothing a desktop portal does is being excused here -- the rig is saying it
  // cannot host the test at all.
  if (std::getenv("MISTERCAST_RIG_NO_SCREENCOPY"))
    return "this session's portal backend cannot capture headlessly";
  return nullptr;
}

// Exit code 77 is a ctest skip; see SKIP_RETURN_CODE in tests/CMakeLists.txt.
inline constexpr int kSkipExitCode = 77;

// Prints why and returns 77 when a portal suite cannot run, else 0.
inline int portalSessionSkipCode(const char* suite) {
  if (const char* reason = portalSessionUnavailable()) {
    std::fprintf(stderr, "%s skipped: %s\n", suite, reason);
    return kSkipExitCode;
  }
  return 0;
}

}  // namespace mistercast::test
