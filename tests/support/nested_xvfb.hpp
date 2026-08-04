#pragma once

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <memory>
#include <string>
#include <vector>

#include "support/temp_directory.hpp"

namespace mistercast::test {

// A private X server for tests that change server-wide state (RandR monitors,
// screen layout) or that need to kill the server outright. DISPLAY points at it
// for the lifetime of the object.
class NestedXvfb {
 public:
  explicit NestedXvfb(std::vector<std::string> extraArguments = {"-screen", "0",
                                                                 "64x64x24"}) {
    int pipes[2];
    if (pipe(pipes) != 0) return;
    server_ = fork();
    if (server_ < 0) {
      close(pipes[0]);
      close(pipes[1]);
      return;
    }
    if (server_ == 0) {
      close(pipes[0]);
      // Detach the server from this process's stdio and process group. A forked
      // X server that keeps the test binary's stdout open makes ctest wait for
      // EOF long after the test itself has finished.
      const int devNull = open("/dev/null", O_RDWR);
      if (devNull >= 0) {
        dup2(devNull, STDIN_FILENO);
        dup2(devNull, STDOUT_FILENO);
        dup2(devNull, STDERR_FILENO);
        if (devNull > STDERR_FILENO) close(devNull);
      }
      setsid();
      std::vector<std::string> arguments{"Xvfb", "-displayfd",
                                         std::to_string(pipes[1])};
      arguments.insert(arguments.end(), extraArguments.begin(),
                       extraArguments.end());
      arguments.push_back("-nolisten");
      arguments.push_back("tcp");
      std::vector<char*> argv;
      for (auto& argument : arguments)
        argv.push_back(const_cast<char*>(argument.c_str()));
      argv.push_back(nullptr);
      execvp(argv[0], argv.data());
      _exit(127);
    }
    close(pipes[1]);
    char number[16]{};
    const auto length = read(pipes[0], number, sizeof(number) - 1);
    close(pipes[0]);
    if (length <= 0) return;
    number[length] = '\0';
    display_ = ":" + std::string(number);
    while (!display_.empty() &&
           (display_.back() == '\n' || display_.back() == '\r'))
      display_.pop_back();
    environment_ =
        std::make_unique<ScopedEnvironment>("DISPLAY", display_.c_str());
    ready_ = true;
  }

  ~NestedXvfb() { terminate(); }

  NestedXvfb(const NestedXvfb&) = delete;
  NestedXvfb& operator=(const NestedXvfb&) = delete;

  bool ready() const noexcept { return ready_; }
  const std::string& display() const noexcept { return display_; }

  void terminate() {
    if (server_ > 0) {
      kill(server_, SIGTERM);
      waitpid(server_, nullptr, 0);
      server_ = -1;
    }
  }

  // Releases DISPLAY without stopping the server, for the replacement-server case.
  void forgetEnvironment() { environment_.reset(); }

 private:
  pid_t server_{-1};
  std::string display_;
  std::unique_ptr<ScopedEnvironment> environment_;
  bool ready_{false};
};
}  // namespace mistercast::test
