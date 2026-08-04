#include "support/subprocess.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cstdlib>

namespace mistercast::test {
namespace {

void readAvailable(int descriptor, std::string& into) {
  std::array<char, 4096> buffer{};
  for (;;) {
    const auto size = ::read(descriptor, buffer.data(), buffer.size());
    if (size > 0) {
      into.append(buffer.data(), size_t(size));
      continue;
    }
    return;
  }
}

}  // namespace

bool ProcessResult::mentions(const std::string& text) const {
  return out.find(text) != std::string::npos ||
         err.find(text) != std::string::npos;
}

ProcessResult runProcess(const std::vector<std::string>& arguments,
                         ProcessOptions options) {
  ProcessResult result;
  if (arguments.empty()) return result;

  int outPipe[2], errPipe[2];
  if (::pipe(outPipe) != 0) return result;
  if (::pipe(errPipe) != 0) {
    ::close(outPipe[0]);
    ::close(outPipe[1]);
    return result;
  }

  const auto child = ::fork();
  if (child < 0) {
    ::close(outPipe[0]);
    ::close(outPipe[1]);
    ::close(errPipe[0]);
    ::close(errPipe[1]);
    return result;
  }
  if (child == 0) {
    ::dup2(outPipe[1], STDOUT_FILENO);
    ::dup2(errPipe[1], STDERR_FILENO);
    ::close(outPipe[0]);
    ::close(outPipe[1]);
    ::close(errPipe[0]);
    ::close(errPipe[1]);
    for (const auto& [name, value] : options.environment) {
      if (value.empty())
        ::unsetenv(name.c_str());
      else
        ::setenv(name.c_str(), value.c_str(), 1);
    }
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto& argument : arguments)
      argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    ::execv(argv[0], argv.data());
    ::_exit(127);
  }

  ::close(outPipe[1]);
  ::close(errPipe[1]);
  // Non-blocking reads let one loop drain both streams and still notice the
  // child exiting; a blocking read would stall until the child wrote again.
  ::fcntl(outPipe[0], F_SETFL, O_NONBLOCK);
  ::fcntl(errPipe[0], F_SETFL, O_NONBLOCK);

  const auto started = std::chrono::steady_clock::now();
  const auto deadline = started + options.timeout;
  bool interrupted = false;
  int status = 0;
  bool reaped = false;
  std::array<pollfd, 2> descriptors{pollfd{outPipe[0], POLLIN, 0},
                                    pollfd{errPipe[0], POLLIN, 0}};

  while (!reaped) {
    const auto now = std::chrono::steady_clock::now();
    if (options.interruptAfter && !interrupted &&
        now - started >= *options.interruptAfter) {
      // WNOHANG first, so a child that already exited is not counted as
      // signalled and the caller can tell the two cases apart.
      if (::waitpid(child, &status, WNOHANG) == child) {
        reaped = true;
        interrupted = true;
      } else {
        ::kill(child, SIGINT);
        interrupted = true;
        result.interruptDelivered = true;
      }
    }
    if (now >= deadline) {
      result.timedOut = true;
      ::kill(child, SIGKILL);
      ::waitpid(child, &status, 0);
      break;
    }
    ::poll(descriptors.data(), descriptors.size(), 20);
    if (descriptors[0].revents) readAvailable(outPipe[0], result.out);
    if (descriptors[1].revents) readAvailable(errPipe[0], result.err);
    if (::waitpid(child, &status, WNOHANG) == child) reaped = true;
  }
  // Drain whatever the child wrote between the last poll and its exit.
  readAvailable(outPipe[0], result.out);
  readAvailable(errPipe[0], result.err);
  ::close(outPipe[0]);
  ::close(errPipe[0]);

  if (WIFEXITED(status))
    result.exitCode = WEXITSTATUS(status);
  else if (WIFSIGNALED(status))
    result.terminatingSignal = WTERMSIG(status);
  return result;
}

}  // namespace mistercast::test
