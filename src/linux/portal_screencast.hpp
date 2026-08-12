#pragma once

// Only compiled into builds that found libsystemd and libpipewire; CMakeLists
// adds this translation unit with the feature rather than guarding it here.
#include <systemd/sd-bus.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

#include "mistercast/types.hpp"

namespace mistercast {

// One granted ScreenCast session: the PipeWire remote to connect to, and the
// node inside it carrying the selected source.
struct PortalStream {
  // Owned by the caller once open() succeeds.
  int pipewireFd{-1};
  uint32_t nodeId{};
  // The portal's logical stream size. Advisory only: with output scaling it is
  // not the pixel size, so the real dimensions come from PipeWire's negotiated
  // format instead.
  uint32_t width{}, height{};
  std::string restoreToken;
};

// The org.freedesktop.portal.ScreenCast half of Wayland capture: everything
// that happens over D-Bus, up to the point where there is a PipeWire remote to
// read pixels from. Kept apart from the PipeWire consumer because it is the
// half that needs a session bus and a desktop portal rather than a compositor,
// and so it can be tested against a fake portal on a private bus.
class PortalScreenCast {
 public:
  PortalScreenCast() = default;
  ~PortalScreenCast();
  PortalScreenCast(const PortalScreenCast&) = delete;
  PortalScreenCast& operator=(const PortalScreenCast&) = delete;

  struct Request {
    CapturePreference preference{CapturePreference::Monitor};
    // When the portal accepts this token the grant is restored silently; when it
    // rejects it, the picker is shown as if there had been no token.
    std::string restoreToken;
    bool embedCursor{false};
    // Only Start can show a dialog, so it is the only step allowed to wait for
    // a person. The rest must answer promptly or something is wrong.
    std::chrono::milliseconds pickerTimeout{std::chrono::minutes(2)};
    std::chrono::milliseconds callTimeout{std::chrono::seconds(15)};
  };

  bool open(const Request&, PortalStream&, SessionError&);
  // Pumps the bus so that the portal closing the session — the user revoking
  // the share from the desktop's screen-sharing indicator — is noticed while
  // streaming instead of silently freezing the last frame. The callback runs on
  // the watch thread.
  void watch(std::function<void()> onClosed);
  void close() noexcept;

 private:
  bool ensureBus(SessionError&);
  static int onResponseSignal(sd_bus_message*, void* userdata, sd_bus_error*);
  static int onClosedSignal(sd_bus_message*, void* userdata, sd_bus_error*);
  // Registers the Response match before issuing the call, because the portal is
  // free to answer before the method reply arrives.
  bool call(const char* method, std::chrono::milliseconds timeout,
            const std::function<int(sd_bus_message*, const std::string&)>&
                arguments,
            const std::function<void(sd_bus_message*)>& results,
            SessionError&);
  bool pump(std::chrono::milliseconds timeout, const char* step, SessionError&);

  sd_bus* bus_{};
  sd_bus_slot* sessionSlot_{};
  sd_bus_slot* responseSlot_{};
  std::string sessionPath_;
  std::string senderToken_;
  unsigned tokenCounter_{};
  // Written by the Response and Closed handlers, read by the wait loop.
  bool responded_{}, sessionClosed_{};
  uint32_t responseCode_{};
  const std::function<void(sd_bus_message*)>* resultParser_{};
  std::function<void()> onClosed_;
  std::thread watchThread_;
  std::atomic<bool> watchStop_{false};
};
}  // namespace mistercast
