#include <csignal>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {
bool waitForExit(pid_t child, int& status,
                 std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (waitpid(child, &status, WNOHANG) == child) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: gui_signal_tests MISTERCAST_EXECUTABLE\n";
    return 2;
  }

  const pid_t child = fork();
  if (child == -1) {
    perror("fork");
    return 1;
  }
  if (child == 0) {
    setenv("QT_QPA_PLATFORM", "offscreen", 1);
    setenv("DISPLAY", "", 1);
    execl(argv[1], argv[1], nullptr);
    perror("execl");
    _exit(127);
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  int status = 0;
  if (waitpid(child, &status, WNOHANG) == child) {
    std::cerr << "GUI exited before SIGINT (status " << status << ")\n";
    return 1;
  }
  if (kill(child, SIGINT) == -1) {
    perror("kill");
    kill(child, SIGKILL);
    waitpid(child, nullptr, 0);
    return 1;
  }
  if (!waitForExit(child, status, std::chrono::seconds(5))) {
    std::cerr << "GUI did not exit within 5 seconds of SIGINT\n";
    kill(child, SIGKILL);
    waitpid(child, nullptr, 0);
    return 1;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    std::cerr << "GUI did not exit cleanly after SIGINT (status " << status
              << ")\n";
    return 1;
  }
  return 0;
}
