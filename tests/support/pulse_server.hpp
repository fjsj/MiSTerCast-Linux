#pragma once

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <iterator>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

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

    child_ = ::fork();
    if (child_ < 0) return;
    if (child_ == 0) {
      // The daemon must not keep the test binary's stdout open, or ctest waits
      // for EOF on it long after the tests have finished.
      const int devNull = ::open("/dev/null", O_RDWR);
      if (devNull >= 0) {
        ::dup2(devNull, STDIN_FILENO);
        ::dup2(devNull, STDOUT_FILENO);
        ::dup2(devNull, STDERR_FILENO);
        if (devNull > STDERR_FILENO) ::close(devNull);
      }
      ::setsid();
      ::setenv("XDG_RUNTIME_DIR", directory_.c_str(), 1);
      ::setenv("HOME", directory_.c_str(), 1);
      ::unsetenv("PULSE_SERVER");
      // The daemon claims org.PulseAudio1 on whichever session bus it can find,
      // and --fail=true turns "name already taken" into a failed start-up. The
      // desktop's own server holds that name, and so would a second private
      // server sharing an autolaunched bus, so point both bus addresses at
      // nothing: failing to reach a bus is only a warning.
      const auto absentBus = "unix:path=" + (directory_ / "no-dbus").string();
      ::setenv("DBUS_SESSION_BUS_ADDRESS", absentBus.c_str(), 1);
      ::setenv("DBUS_SYSTEM_BUS_ADDRESS", absentBus.c_str(), 1);
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
      std::vector<char*> argv;
      for (auto& argument : arguments)
        argv.push_back(const_cast<char*>(argument.c_str()));
      argv.push_back(nullptr);
      ::execvp(argv[0], argv.data());
      ::_exit(127);
    }
    ready_ = waitForSocket();
  }

  ~PrivatePulseServer() {
    if (child_ > 0) {
      ::kill(child_, SIGTERM);
      ::waitpid(child_, nullptr, 0);
    }
  }

  PrivatePulseServer(const PrivatePulseServer&) = delete;
  PrivatePulseServer& operator=(const PrivatePulseServer&) = delete;

  bool ready() const noexcept { return ready_; }
  std::string address() const { return "unix:" + socket_.string(); }

 private:
  bool waitForSocket() const {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
      int status = 0;
      if (::waitpid(child_, &status, WNOHANG) == child_) return false;
      if (std::filesystem::exists(socket_)) {
        // The socket appears slightly before the server accepts on it.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
  }

  std::filesystem::path directory_, socket_;
  pid_t child_{-1};
  bool ready_{false};
};

}  // namespace mistercast::test
