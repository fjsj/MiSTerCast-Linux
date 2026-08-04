#pragma once

#include <xcb/xcb.h>

#include <string>

namespace mistercast {
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
