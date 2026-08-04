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

}  // namespace mistercast::test
