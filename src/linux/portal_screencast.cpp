#include "portal_screencast.hpp"

#include <fcntl.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

namespace mistercast {
namespace {
constexpr const char* kService = "org.freedesktop.portal.Desktop";
constexpr const char* kPath = "/org/freedesktop/portal/desktop";
constexpr const char* kScreenCast = "org.freedesktop.portal.ScreenCast";
constexpr const char* kRequest = "org.freedesktop.portal.Request";
constexpr const char* kSession = "org.freedesktop.portal.Session";
// persist_mode and restore_token arrived in ScreenCast version 4. Asking an
// older portal for them is a protocol error, not a silently ignored option.
constexpr uint32_t kRestoreTokenVersion = 4;
constexpr uint32_t kSourceTypeMonitor = 1, kSourceTypeWindow = 2;
// The cursor is always hidden, matching X11 capture, which never includes one.
constexpr uint32_t kCursorHidden = 1;
constexpr uint32_t kPersistPersistent = 2;
constexpr uint32_t kResponseSuccess = 0, kResponseCancelled = 1;
// Only Start can show a dialog, so it is the only step allowed to wait for a
// person. The rest must answer promptly or something is wrong.
constexpr auto kPickerTimeout = std::chrono::minutes(2);
constexpr auto kCallTimeout = std::chrono::seconds(15);

// The bus name with the leading ':' dropped and every '.' turned into '_', which
// is how the portal spells the caller inside a Request object path.
std::string senderToken(const char* uniqueName) {
  std::string token(uniqueName ? uniqueName : "");
  if (!token.empty() && token.front() == ':') token.erase(0, 1);
  std::replace(token.begin(), token.end(), '.', '_');
  return token;
}

SessionError busError(const char* step, int result, const sd_bus_error* error) {
  const std::string name = error && error->name ? error->name : "";
  const std::string detail =
      error && error->message
          ? error->message
          : std::strerror(result < 0 ? -result : result);
  if (name == "org.freedesktop.DBus.Error.ServiceUnknown" ||
      name == "org.freedesktop.DBus.Error.NameHasNoOwner")
    return {"video", "no desktop screen-sharing portal is running",
            "Install xdg-desktop-portal and the backend for your desktop "
            "(xdg-desktop-portal-gnome, -kde, or -wlr)."};
  if (name == "org.freedesktop.DBus.Error.UnknownMethod" ||
      name == "org.freedesktop.DBus.Error.UnknownInterface")
    return {"video",
            std::string("the desktop portal does not implement ") + step,
            "Update xdg-desktop-portal and its desktop backend."};
  return {"video", std::string("portal ") + step + " failed: " + detail,
          "Check that xdg-desktop-portal is running in this session."};
}

// Writes an a{sv} options dictionary. Every portal call sends one, and each
// append can fail, so the first error is kept here rather than checked at a dozen
// call sites.
class OptionDict {
 public:
  explicit OptionDict(sd_bus_message* message) : message_(message) {
    result_ = sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "{sv}");
  }
  // One overload per value type the portal takes, so a caller cannot pair a
  // signature with the wrong argument: the variadic forward to
  // sd_bus_message_append accepts add(key, "s", std::string) and
  // add(key, "b", false) and both are undefined behaviour.
  OptionDict& add(const char* key, const std::string& value) {
    return append(key, "s", value.c_str());
  }
  OptionDict& add(const char* key, uint32_t value) {
    return append(key, "u", value);
  }
  OptionDict& add(const char* key, bool value) {
    return append(key, "b", int(value));
  }
  int close() {
    if (result_ < 0) return result_;
    return sd_bus_message_close_container(message_);
  }

 private:
  template <class... Value>
  OptionDict& append(const char* key, const char* type, Value... value) {
    if (result_ < 0) return *this;
    if ((result_ = sd_bus_message_open_container(
             message_, SD_BUS_TYPE_DICT_ENTRY, "sv")) < 0)
      return *this;
    if ((result_ = sd_bus_message_append(message_, "s", key)) < 0) return *this;
    if ((result_ = sd_bus_message_append(message_, "v", type, value...)) < 0)
      return *this;
    result_ = sd_bus_message_close_container(message_);
    return *this;
  }

  sd_bus_message* message_;
  int result_{};
};

// Reads one uint32 portal property, reporting absence rather than failing: both
// callers have a defined behaviour for a portal that does not answer.
bool portalProperty(sd_bus* bus, const char* name, uint32_t& out) {
  sd_bus_error error = SD_BUS_ERROR_NULL;
  const int result = sd_bus_get_property_trivial(bus, kService, kPath,
                                                 kScreenCast, name, &error,
                                                 'u', &out);
  sd_bus_error_free(&error);
  return result >= 0;
}

// Variants are entered explicitly rather than through sd_bus_message_read's "v"
// shorthand, because a portal is free to send a key whose value is not the type
// expected. enter_container refuses a mismatch without consuming anything,
// which leaves the entry in a state where skipping it is still correct.
bool readStringVariant(sd_bus_message* message, std::string& out) {
  if (sd_bus_message_enter_container(message, SD_BUS_TYPE_VARIANT, "s") <= 0)
    return false;
  const char* value = nullptr;
  const bool read = sd_bus_message_read(message, "s", &value) >= 0 && value;
  if (read) out = value;
  sd_bus_message_exit_container(message);
  return read;
}

bool readPointVariant(sd_bus_message* message, int32_t& x, int32_t& y) {
  if (sd_bus_message_enter_container(message, SD_BUS_TYPE_VARIANT, "(ii)") <= 0)
    return false;
  const bool read = sd_bus_message_read(message, "(ii)", &x, &y) >= 0;
  sd_bus_message_exit_container(message);
  return read;
}

// Reads the key of an a{sv} entry the iteration has already entered.
std::string entryKey(sd_bus_message* message) {
  const char* key = nullptr;
  if (sd_bus_message_read(message, "s", &key) < 0 || !key) return {};
  return key;
}

// Walks an a{sv} dictionary, offering each key to the reader. A reader that does
// not consume the value returns false and the value is skipped: leaving one
// half-read desynchronizes every field after it.
template <class Reader>
void forEachOption(sd_bus_message* message, Reader reader) {
  if (sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "{sv}") < 0)
    return;
  while (sd_bus_message_enter_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv") >
         0) {
    if (!reader(entryKey(message))) sd_bus_message_skip(message, "v");
    sd_bus_message_exit_container(message);
  }
  sd_bus_message_exit_container(message);
}

struct GrantedStream {
  bool present{};
  uint32_t nodeId{};
  uint32_t width{}, height{};
};

// Parses the "streams" value of a Start response: a(ua{sv}), each entry a
// PipeWire node id and its properties. multiple:false was requested, so only the
// first entry is kept -- but the rest are still walked, because leaving a
// container half-read desynchronizes every field after it.
bool readStreamVariant(sd_bus_message* message, GrantedStream& out) {
  if (sd_bus_message_enter_container(message, SD_BUS_TYPE_VARIANT,
                                     "a(ua{sv})") <= 0)
    return false;
  if (sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "(ua{sv})") >
      0) {
    while (sd_bus_message_enter_container(message, SD_BUS_TYPE_STRUCT,
                                          "ua{sv}") > 0) {
      const bool first = !out.present;
      uint32_t node = 0;
      if (sd_bus_message_read(message, "u", &node) >= 0 && first) {
        out.nodeId = node;
        out.present = true;
      }
      forEachOption(message, [&](const std::string& property) {
        int32_t x = 0, y = 0;
        if (property != "size" || !readPointVariant(message, x, y)) return false;
        if (first) {
          out.width = uint32_t(std::max(x, 0));
          out.height = uint32_t(std::max(y, 0));
        }
        return true;
      });
      sd_bus_message_exit_container(message);
    }
    sd_bus_message_exit_container(message);
  }
  sd_bus_message_exit_container(message);
  return true;
}
}  // namespace

PortalScreenCast::~PortalScreenCast() { close(); }

bool PortalScreenCast::ensureBus(SessionError& error) {
  if (bus_) return true;
  const int result = sd_bus_open_user(&bus_);
  if (result < 0) {
    bus_ = nullptr;
    error = {"video", "cannot reach the desktop session bus",
             "Start MiSTerCast from inside your desktop session so "
             "DBUS_SESSION_BUS_ADDRESS points at it."};
    return false;
  }
  // sd_bus_open_user() can return before the connection has authenticated, and
  // until it has there is no unique name. One round trip forces it; without it
  // the name is empty, every Request path becomes ".../request//token", and that
  // invalid path fails the first sd_bus_match_signal.
  sd_bus_error pingFailure = SD_BUS_ERROR_NULL;
  sd_bus_call_method(bus_, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                     "org.freedesktop.DBus.Peer", "Ping", &pingFailure, nullptr,
                     "");
  sd_bus_error_free(&pingFailure);
  const char* unique = nullptr;
  const int named = sd_bus_get_unique_name(bus_, &unique);
  senderToken_ = named >= 0 ? senderToken(unique) : std::string{};
  if (senderToken_.empty()) {
    error = {"video",
             std::string("the desktop session bus gave this process no name: ") +
                 std::strerror(named < 0 ? -named : EINVAL),
             "Check that DBUS_SESSION_BUS_ADDRESS points at your own session "
             "bus and that the socket is readable."};
    return false;
  }
  return true;
}

bool PortalScreenCast::pump(std::chrono::milliseconds timeout, const char* step,
                            SessionError& error) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!responded_) {
    if (sessionClosed_) {
      error = {"video", "the desktop portal closed the screen-sharing session",
               "Start the stream again and allow sharing."};
      return false;
    }
    const int processed = sd_bus_process(bus_, nullptr);
    if (processed < 0) {
      error = {"video", std::string("portal ") + step + " lost the session bus",
               "Check that the desktop session bus is still running."};
      return false;
    }
    if (processed > 0) continue;
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      error = {"video",
               std::string("the desktop portal did not answer ") + step,
               "Check that xdg-desktop-portal and its desktop backend are "
               "running, then try again."};
      return false;
    }
    // Bounded so a hung portal cannot pin this thread past the deadline, and so
    // a session that closes mid-wait is noticed promptly.
    const auto remaining =
        std::chrono::duration_cast<std::chrono::microseconds>(deadline - now)
            .count();
    if (sd_bus_wait(bus_, uint64_t(std::min<int64_t>(remaining, 200000))) < 0) {
      error = {"video", std::string("portal ") + step + " lost the session bus",
               "Check that the desktop session bus is still running."};
      return false;
    }
  }
  return true;
}

bool PortalScreenCast::call(
    const char* method, std::chrono::milliseconds timeout,
    const std::function<int(sd_bus_message*, const std::string&)>& arguments,
    const std::function<void(sd_bus_message*)>& results, SessionError& error) {
  const auto token = "mistercast" + std::to_string(::getpid()) + "_" +
                     std::to_string(++tokenCounter_);
  // The portal is free to emit Response before the method reply arrives, so the
  // path is computed and subscribed before the call rather than after it.
  const auto expected =
      std::string(kPath) + "/request/" + senderToken_ + "/" + token;
  responded_ = false;
  responseCode_ = 0;
  resultParser_ = &results;
  sd_bus_slot_unref(responseSlot_);
  responseSlot_ = nullptr;
  const int watched =
      sd_bus_match_signal(bus_, &responseSlot_, kService, expected.c_str(),
                          kRequest, "Response",
                          PortalScreenCast::onResponseSignal, this);
  if (watched < 0) {
    error = {"video",
             std::string("cannot watch the portal ") + method + " request: " +
                 std::strerror(watched < 0 ? -watched : EINVAL),
             "Check that the desktop session bus is still running."};
    return false;
  }
  sd_bus_message* message = nullptr;
  int result = sd_bus_message_new_method_call(bus_, &message, kService, kPath,
                                              kScreenCast, method);
  if (result >= 0) result = arguments(message, token);
  sd_bus_error busFailure = SD_BUS_ERROR_NULL;
  sd_bus_message* reply = nullptr;
  if (result >= 0)
    result = sd_bus_call(bus_, message, 0, &busFailure, &reply);
  sd_bus_message_unref(message);
  if (result < 0) {
    error = busError(method, result, &busFailure);
    sd_bus_error_free(&busFailure);
    return false;
  }
  const char* handle = nullptr;
  if (sd_bus_message_read(reply, "o", &handle) >= 0 && handle &&
      expected != handle) {
    // Portals older than 0.9 minted their own handle. Subscribing to the one
    // they returned is the documented fallback.
    sd_bus_slot_unref(responseSlot_);
    responseSlot_ = nullptr;
    sd_bus_match_signal(bus_, &responseSlot_, kService, handle, kRequest,
                        "Response", PortalScreenCast::onResponseSignal, this);
  }
  sd_bus_message_unref(reply);
  sd_bus_error_free(&busFailure);
  const bool answered = pump(timeout, method, error);
  // The parser is a reference to one of open()'s locals; leaving it installed
  // would let a late Response reach it after open() has returned.
  resultParser_ = nullptr;
  if (!answered) return false;
  if (responseCode_ != kResponseSuccess) {
    if (responseCode_ == kResponseCancelled)
      error = {"video", "the screen-sharing request was cancelled",
               "Choose a screen or window in the desktop dialog and allow "
               "sharing."};
    else
      error = {"video",
               std::string("the desktop portal refused ") + method,
               "Check the desktop's screen-sharing permissions and try again."};
    return false;
  }
  return true;
}

int PortalScreenCast::onResponseSignal(sd_bus_message* message, void* userdata,
                                      sd_bus_error*) {
  auto& self = *static_cast<PortalScreenCast*>(userdata);
  uint32_t code = 0;
  if (sd_bus_message_read(message, "u", &code) < 0) return 0;
  self.responseCode_ = code;
  self.responded_ = true;
  if (self.resultParser_ && *self.resultParser_) (*self.resultParser_)(message);
  return 1;
}

int PortalScreenCast::onClosedSignal(sd_bus_message*, void* userdata,
                                     sd_bus_error*) {
  auto& self = *static_cast<PortalScreenCast*>(userdata);
  self.sessionClosed_ = true;
  if (self.onClosed_) self.onClosed_();
  return 1;
}

bool PortalScreenCast::open(const Request& request, PortalStream& out,
                            SessionError& error) {
  if (!ensureBus(error)) return false;
  // Cleared before the first call rather than after CreateSession: a previous
  // session's closure would otherwise make this handshake's very first wait
  // return as though the new session had already ended.
  sessionClosed_ = false;
  uint32_t version = 1;
  const bool haveVersion = portalProperty(bus_, "version", version);
  uint32_t cursorModes = 0;
  const bool haveCursorModes =
      portalProperty(bus_, "AvailableCursorModes", cursorModes);

  std::string sessionPath;
  auto readSession = [&sessionPath](sd_bus_message* message) {
    forEachOption(message, [&](const std::string& key) {
      return key == "session_handle" && readStringVariant(message, sessionPath);
    });
  };
  const auto sessionToken =
      "mistercast" + std::to_string(::getpid()) + "_session";
  if (!call(
          "CreateSession", kCallTimeout,
          [&](sd_bus_message* message, const std::string& token) {
            return OptionDict(message)
                .add("handle_token", token)
                .add("session_handle_token", sessionToken)
                .close();
          },
          readSession, error))
    return false;
  if (sessionPath.empty()) {
    error = {"video", "the desktop portal returned no screen-sharing session",
             "Update xdg-desktop-portal and its desktop backend."};
    return false;
  }
  sessionPath_ = sessionPath;
  sd_bus_match_signal(bus_, &sessionSlot_, kService, sessionPath_.c_str(),
                      kSession, "Closed", PortalScreenCast::onClosedSignal, this);

  const bool restorable = haveVersion && version >= kRestoreTokenVersion;
  if (!call(
          "SelectSources", kCallTimeout,
          [&](sd_bus_message* message, const std::string& token) {
            int r = sd_bus_message_append(message, "o", sessionPath_.c_str());
            if (r < 0) return r;
            OptionDict options(message);
            options.add("handle_token", token)
                .add("types", request.preference == CapturePreference::Window
                                  ? kSourceTypeWindow
                                  : kSourceTypeMonitor)
                .add("multiple", false);
            // cursor_mode and persist_mode are only accepted by portals that
            // advertise them; an older one treats an unknown option as an error.
            if (haveCursorModes && (cursorModes & kCursorHidden))
              options.add("cursor_mode", kCursorHidden);
            if (restorable) {
              options.add("persist_mode", kPersistPersistent);
              if (!request.restoreToken.empty())
                options.add("restore_token", request.restoreToken);
            }
            return options.close();
          },
          {}, error))
    return false;

  GrantedStream granted;
  std::string restoreToken;
  auto readStreams = [&](sd_bus_message* message) {
    forEachOption(message, [&](const std::string& key) {
      if (key == "restore_token") return readStringVariant(message, restoreToken);
      if (key == "streams") return readStreamVariant(message, granted);
      return false;
    });
  };
  if (!call(
          "Start", kPickerTimeout,
          [&](sd_bus_message* message, const std::string& token) {
            int r = sd_bus_message_append(message, "o", sessionPath_.c_str());
            if (r < 0) return r;
            // No parent window: MiSTerCast has no Wayland surface handle to
            // give, so the portal dialog is not modal to it.
            if ((r = sd_bus_message_append(message, "s", "")) < 0) return r;
            return OptionDict(message).add("handle_token", token).close();
          },
          readStreams, error))
    return false;
  if (!granted.present) {
    error = {"video", "the desktop portal granted no capture stream",
             "Choose a screen or window in the desktop dialog and try again."};
    return false;
  }

  sd_bus_error busFailure = SD_BUS_ERROR_NULL;
  sd_bus_message* reply = nullptr;
  int result = sd_bus_call_method(bus_, kService, kPath, kScreenCast,
                                  "OpenPipeWireRemote", &busFailure, &reply,
                                  "oa{sv}", sessionPath_.c_str(), 0);
  if (result < 0) {
    error = busError("OpenPipeWireRemote", result, &busFailure);
    sd_bus_error_free(&busFailure);
    return false;
  }
  int borrowed = -1;
  result = sd_bus_message_read(reply, "h", &borrowed);
  // The descriptor belongs to the reply message, so it has to be duplicated
  // before the message is dropped.
  const int owned =
      result >= 0 ? ::fcntl(borrowed, F_DUPFD_CLOEXEC, 0) : -1;
  sd_bus_message_unref(reply);
  sd_bus_error_free(&busFailure);
  if (owned < 0) {
    error = {"video", "the desktop portal returned no PipeWire connection",
             "Check that PipeWire is running in this session."};
    return false;
  }
  out.pipewireFd = owned;
  out.nodeId = granted.nodeId;
  out.width = granted.width;
  out.height = granted.height;
  out.restoreToken = restoreToken;
  return true;
}

void PortalScreenCast::watch(std::function<void()> onClosed) {
  if (!bus_ || watchThread_.joinable()) return;
  onClosed_ = std::move(onClosed);
  watchStop_ = false;
  watchThread_ = std::thread([this] {
    // The bus is not thread safe; from here until close() joins this thread,
    // this thread is its only user.
    while (!watchStop_) {
      const int processed = sd_bus_process(bus_, nullptr);
      if (processed < 0) return;
      if (processed > 0) continue;
      if (sd_bus_wait(bus_, 200000) < 0) return;
    }
  });
}

void PortalScreenCast::close() noexcept {
  watchStop_ = true;
  if (watchThread_.joinable()) watchThread_.join();
  onClosed_ = {};
  if (bus_ && !sessionPath_.empty() && !sessionClosed_) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    // Releasing the grant when the stream stops is the point: an idle
    // MiSTerCast should not hold a live capture permission.
    sd_bus_call_method(bus_, kService, sessionPath_.c_str(), kSession, "Close",
                       &error, nullptr, "");
    sd_bus_error_free(&error);
  }
  sd_bus_slot_unref(responseSlot_);
  sd_bus_slot_unref(sessionSlot_);
  responseSlot_ = nullptr;
  sessionSlot_ = nullptr;
  sessionPath_.clear();
  resultParser_ = nullptr;
  if (bus_) {
    sd_bus_flush(bus_);
    sd_bus_close(bus_);
    sd_bus_unref(bus_);
    bus_ = nullptr;
  }
}
}  // namespace mistercast
