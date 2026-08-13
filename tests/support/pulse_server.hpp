#pragma once

#include <cstdlib>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "support/subprocess.hpp"
#include "support/wait_for.hpp"

namespace mistercast::test {

// A private PulseAudio server with a single null sink, reachable only through a
// socket in a temporary directory.
//
// The capture code under test enumerates sinks, reads a sink monitor, and — for
// the "silent output" option — loads a null sink and moves the server's default
// output and live playback streams onto it. Doing that against the developer's
// own sound server would reroute whatever they are listening to, so the suite
// brings up a server of its own instead. Nothing outside this directory is
// touched, and the same setup works headlessly in CI.
class PrivatePulseServer {
 public:
  // withNullSink=false brings up a server with no output at all, which is what a
  // machine with no sound card looks like to the capture code.
  explicit PrivatePulseServer(const std::filesystem::path& directory,
                              bool withNullSink = true)
      : directory_(directory) {
    socket_ = directory_ / "native";
    std::filesystem::create_directories(directory_);
    // PulseAudio refuses a runtime directory other processes could read.
    std::filesystem::permissions(directory_,
                                 std::filesystem::perms::owner_all);

    std::vector<std::string> arguments{
        "pulseaudio",
        "-n",
        "--daemonize=no",
        "--fail=true",
        "--exit-idle-time=-1",
        "--disallow-exit=true",
        "--disable-shm=true",
        "--log-target=newfile:" + (directory_ / "pulse.log").string()};
    if (withNullSink)
      arguments.push_back(
          "--load=module-null-sink sink_name=mistercast_test_output "
          "rate=48000 channels=2 "
          "sink_properties=device.description=MiSTerCast_Test_Output");
    arguments.push_back("--load=module-native-protocol-unix socket=" +
                        socket_.string());
    // The daemon claims org.PulseAudio1 on whichever session bus it can find,
    // and --fail=true turns "name already taken" into a failed start-up. The
    // desktop's own server holds that name, and so would a second private server
    // sharing an autolaunched bus, so point both bus addresses at nothing:
    // failing to reach a bus is only a warning.
    const auto absentBus = "unix:path=" + (directory_ / "no-dbus").string();
    server_ = std::make_unique<BackgroundProcess>(arguments, [&] {
      ::setenv("XDG_RUNTIME_DIR", directory_.c_str(), 1);
      ::setenv("HOME", directory_.c_str(), 1);
      ::unsetenv("PULSE_SERVER");
      ::setenv("DBUS_SESSION_BUS_ADDRESS", absentBus.c_str(), 1);
      ::setenv("DBUS_SYSTEM_BUS_ADDRESS", absentBus.c_str(), 1);
    });
    ready_ = server_->started() && waitForSocket();
  }

  PrivatePulseServer(const PrivatePulseServer&) = delete;
  PrivatePulseServer& operator=(const PrivatePulseServer&) = delete;

  bool ready() const noexcept { return ready_; }
  std::string address() const { return "unix:" + socket_.string(); }

 private:
  bool waitForSocket() {
    bool listening = false;
    waitFor([&] {
      if (!server_->running()) return true;  // died during start-up
      listening = std::filesystem::exists(socket_);
      return listening;
    });
    // The socket appears slightly before the server accepts on it.
    if (listening) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return listening;
  }

  std::filesystem::path directory_, socket_;
  std::unique_ptr<BackgroundProcess> server_;
  bool ready_{false};
};

// The same private sound server, provided by PipeWire's PulseAudio replacement
// instead of PulseAudio itself.
//
// This is what Ubuntu 24.04 and 26.04 actually run, and the parts of the capture
// code most likely to differ are the ones the pipewire-pulse implementation has
// to emulate rather than merely serve: loading module-null-sink, changing the
// default sink, and moving live playback streams. Running the same suite against
// both servers is the only way to know they behave the same.
//
// wireplumber is not optional here even though nothing in the suite talks to it:
// pipewire-pulse keeps the default-sink choice in metadata that the session
// manager owns, and without one, set-default-sink answers "Not supported" and
// every silent-output test fails for a reason that has nothing to do with
// MiSTerCast.
class PrivatePipeWirePulseServer {
 public:
  explicit PrivatePipeWirePulseServer(const std::filesystem::path& directory,
                                      bool withNullSink = true)
      : directory_(directory) {
    // pipewire-pulse puts its native socket under the runtime directory.
    socket_ = directory_ / "pulse" / "native";
    std::filesystem::create_directories(directory_);
    std::filesystem::permissions(directory_,
                                 std::filesystem::perms::owner_all);
    const auto environment = [this] {
      ::setenv("XDG_RUNTIME_DIR", directory_.c_str(), 1);
      ::setenv("HOME", directory_.c_str(), 1);
      ::unsetenv("PULSE_SERVER");
      // Left to find the session bus if there is one: PipeWire only logs a
      // warning without it, unlike PulseAudio's name claim.
    };
    daemon_ = std::make_unique<BackgroundProcess>(
        std::vector<std::string>{"pipewire"}, environment);
    if (!daemon_->started()) return;
    if (!waitFor([this] { return std::filesystem::exists(
                              directory_ / "pipewire-0"); }))
      return;
    sessionManager_ = std::make_unique<BackgroundProcess>(
        std::vector<std::string>{"wireplumber"}, environment);
    pulse_ = std::make_unique<BackgroundProcess>(
        std::vector<std::string>{"pipewire-pulse"}, environment);
    if (!sessionManager_->started() || !pulse_->started()) return;
    if (!waitFor([this] { return std::filesystem::exists(socket_); })) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ready_ = !withNullSink || loadNullSink();
  }

  PrivatePipeWirePulseServer(const PrivatePipeWirePulseServer&) = delete;
  PrivatePipeWirePulseServer& operator=(const PrivatePipeWirePulseServer&) =
      delete;

  bool ready() const noexcept { return ready_; }
  std::string address() const { return "unix:" + socket_.string(); }

 private:
  // PulseAudio takes its startup modules on the command line; pipewire-pulse has
  // no equivalent, so the sink is loaded over the protocol once the server is up.
  // pactl rather than libpulse so this header stays usable by suites that do not
  // link it.
  bool loadNullSink() {
    const auto command =
        "PULSE_SERVER=" + address() +
        " pactl load-module module-null-sink sink_name=mistercast_test_output"
        " rate=48000 channels=2"
        " sink_properties=device.description=MiSTerCast_Test_Output"
        " >/dev/null 2>&1";
    if (std::system(command.c_str()) != 0) return false;
    const auto setDefault = "PULSE_SERVER=" + address() +
                            " pactl set-default-sink mistercast_test_output"
                            " >/dev/null 2>&1";
    return std::system(setDefault.c_str()) == 0;
  }

  std::filesystem::path directory_, socket_;
  std::unique_ptr<BackgroundProcess> daemon_, sessionManager_, pulse_;
  bool ready_{false};
};

}  // namespace mistercast::test
