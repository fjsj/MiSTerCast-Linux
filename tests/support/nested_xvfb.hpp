#pragma once

#include <unistd.h>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "support/scoped_environment.hpp"
#include "support/subprocess.hpp"

namespace mistercast::test {

// A private X server for tests that change server-wide state (RandR monitors,
// screen layout) or that need to kill the server outright. DISPLAY points at it
// for the lifetime of the object.
class NestedXvfb {
 public:
  explicit NestedXvfb(std::vector<std::string> extraArguments = {"-screen", "0",
                                                                 "64x64x24"}) {
    // Xvfb writes the display number it settled on to -displayfd, which is the
    // only race-free way to learn it. The pipe is created before the fork so the
    // server inherits the write end.
    int pipes[2];
    if (pipe(pipes) != 0) return;
    std::vector<std::string> arguments{"Xvfb", "-displayfd",
                                       std::to_string(pipes[1])};
    arguments.insert(arguments.end(), extraArguments.begin(),
                     extraArguments.end());
    arguments.push_back("-nolisten");
    arguments.push_back("tcp");
    server_ = std::make_unique<BackgroundProcess>(arguments);
    close(pipes[1]);
    if (!server_->started()) {
      close(pipes[0]);
      return;
    }
    char number[16]{};
    const auto length = read(pipes[0], number, sizeof(number) - 1);
    close(pipes[0]);
    if (length <= 0) return;
    display_ = ":" + std::string(number, size_t(length));
    while (!display_.empty() &&
           (display_.back() == '\n' || display_.back() == '\r'))
      display_.pop_back();
    environment_ =
        std::make_unique<ScopedEnvironment>("DISPLAY", display_.c_str());
    ready_ = true;
  }

  NestedXvfb(const NestedXvfb&) = delete;
  NestedXvfb& operator=(const NestedXvfb&) = delete;

  bool ready() const noexcept { return ready_; }
  const std::string& display() const noexcept { return display_; }

  void terminate() {
    if (server_) server_->terminate();
  }

  // Releases DISPLAY without stopping the server, for the replacement-server case.
  void forgetEnvironment() { environment_.reset(); }

 private:
  std::unique_ptr<BackgroundProcess> server_;
  std::string display_;
  std::unique_ptr<ScopedEnvironment> environment_;
  bool ready_{false};
};
}  // namespace mistercast::test
