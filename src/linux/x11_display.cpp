#include "x11_display.hpp"

#include <cstdlib>
#include <utility>

namespace mistercast {
bool x11ExtensionPresent(xcb_connection_t* connection,
                         xcb_extension_t& extension) noexcept {
  if (!connection) return false;
  const auto* data = xcb_get_extension_data(connection, &extension);
  return data && data->present;
}

X11DisplayConnection::~X11DisplayConnection() { reset(); }

X11DisplayConnection::X11DisplayConnection(
    X11DisplayConnection&& other) noexcept {
  *this = std::move(other);
}

X11DisplayConnection& X11DisplayConnection::operator=(
    X11DisplayConnection&& other) noexcept {
  if (this == &other) return *this;
  reset();
  connection_ = std::exchange(other.connection_, nullptr);
  screen_ = std::exchange(other.screen_, nullptr);
  return *this;
}

bool X11DisplayConnection::connect(std::string& error) {
  if (connection_) return true;
  const char* display = std::getenv("DISPLAY");
  if (!display || !*display) {
    error = "X11/Xorg required: DISPLAY is not set";
    return false;
  }
  int screenIndex = 0;
  connection_ = xcb_connect(nullptr, &screenIndex);
  if (!connection_ || xcb_connection_has_error(connection_)) {
    reset();
    error =
        "cannot connect to X11 display; on a Wayland session use the portal "
        "capture backend instead";
    return false;
  }
  auto screens = xcb_setup_roots_iterator(xcb_get_setup(connection_));
  while (screenIndex-- && screens.rem) xcb_screen_next(&screens);
  screen_ = screens.data;
  if (!screen_) {
    reset();
    error = "X11 display has no screen";
    return false;
  }
  return true;
}

void X11DisplayConnection::reset() noexcept {
  if (connection_) xcb_disconnect(connection_);
  connection_ = nullptr;
  screen_ = nullptr;
}

bool X11DisplayConnection::lost() const noexcept {
  return connection_ && xcb_connection_has_error(connection_);
}
}  // namespace mistercast
