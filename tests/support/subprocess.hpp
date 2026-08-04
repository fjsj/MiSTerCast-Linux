#pragma once

#include <sys/types.h>

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mistercast::test {

struct ProcessResult {
  int exitCode{-1};
  int terminatingSignal{0};
  bool timedOut{false};
  // True when the child was still running at `interruptAfter` and was signalled.
  // A child that had already exited is never signalled, which distinguishes
  // "shut down cleanly on SIGINT" from "exited before the signal arrived".
  bool interruptDelivered{false};
  std::string out, err;

  bool exitedWith(int code) const noexcept {
    return !timedOut && !terminatingSignal && exitCode == code;
  }
  // Convenience for assertions that only care that some diagnostic mentioned
  // the text, without depending on which stream carried it.
  bool mentions(const std::string& text) const;
};

struct ProcessOptions {
  std::vector<std::pair<std::string, std::string>> environment;
  std::chrono::milliseconds timeout{std::chrono::seconds(20)};
  // Sends SIGINT once the child has been running this long, for the CLI paths
  // whose only clean exit is an interrupt.
  std::optional<std::chrono::milliseconds> interruptAfter;
};

// Runs a program to completion, capturing both output streams. Uses fork/exec
// rather than popen so the child's environment and signals can be controlled and
// so coverage counters are flushed by the child's own normal exit.
ProcessResult runProcess(const std::vector<std::string>& arguments,
                         ProcessOptions options = {});

// A long-lived helper daemon (Xvfb, pulseaudio) that the suite starts and stops,
// as opposed to runProcess's run-to-completion child.
//
// Its stdio goes to /dev/null and it gets its own session, which is not optional:
// a forked daemon still holding the test binary's stdout keeps ctest waiting for
// EOF long after the tests have finished.
class BackgroundProcess {
 public:
  // childSetup, when given, runs in the forked child after stdio has been
  // redirected and setsid() has been called, immediately before exec — the one
  // place a daemon's environment can be arranged without disturbing the test
  // process. File descriptors the parent opened before construction are still
  // inherited, so a readiness pipe can be passed through argv.
  explicit BackgroundProcess(const std::vector<std::string>& arguments,
                             const std::function<void()>& childSetup = {});
  ~BackgroundProcess();

  BackgroundProcess(const BackgroundProcess&) = delete;
  BackgroundProcess& operator=(const BackgroundProcess&) = delete;

  bool started() const noexcept { return child_ > 0; }
  // False once the daemon has exited, which distinguishes "still starting up"
  // from "died during start-up". Not const: the check reaps an exited child, and
  // the pid must be forgotten with it, or a later terminate() could signal a
  // process the kernel has since given that pid to.
  bool running() noexcept;
  // SIGTERM and reap. Idempotent, and called by the destructor.
  void terminate() noexcept;

 private:
  pid_t child_{-1};
};

}  // namespace mistercast::test
