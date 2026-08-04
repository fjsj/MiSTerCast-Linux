#pragma once

#include <xcb/xcb.h>

#include <string>

namespace mistercast {

// True when the server advertises the extension. Every optional extension must
// be checked with this before any of its requests are sent: libxcb shuts the
// whole connection down when a request belonging to an absent extension is
// issued, so an unguarded probe does not degrade, it destroys the connection it
// was probing — and capture is handed its own connection here, on start and on
// every recovery. Composite, MIT-SHM, and RandR are all guarded this way.
bool x11ExtensionPresent(xcb_connection_t* connection,
                         xcb_extension_t& extension) noexcept;

class X11DisplayConnection {
 public:
  X11DisplayConnection() = default;
  ~X11DisplayConnection();
  X11DisplayConnection(const X11DisplayConnection&) = delete;
  X11DisplayConnection& operator=(const X11DisplayConnection&) = delete;
  X11DisplayConnection(X11DisplayConnection&&) noexcept;
  X11DisplayConnection& operator=(X11DisplayConnection&&) noexcept;

  bool connect(std::string& error);
  void reset() noexcept;
  bool lost() const noexcept;
  xcb_connection_t* connection() const noexcept { return connection_; }
  xcb_screen_t* screen() const noexcept { return screen_; }

 private:
  xcb_connection_t* connection_{};
  xcb_screen_t* screen_{};
};
}  // namespace mistercast
