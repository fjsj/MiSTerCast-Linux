#pragma once

#include <chrono>
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

}  // namespace mistercast::test
