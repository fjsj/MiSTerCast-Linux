#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include <unistd.h>

namespace mistercast::test {

// A unique directory removed on destruction, so config tests never share state
// and never touch the developer's real ~/.config/mistercast.
class TemporaryDirectory {
 public:
  explicit TemporaryDirectory(const std::string& label) {
    path_ = std::filesystem::temp_directory_path() /
            ("mistercast-" + label + "-" + std::to_string(::getpid()) + "-" +
             std::to_string(counter()));
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  const std::filesystem::path& path() const noexcept { return path_; }
  std::filesystem::path file(const std::string& name) const {
    return path_ / name;
  }

  std::filesystem::path write(const std::string& name,
                              const std::string& contents) const {
    const auto target = file(name);
    std::ofstream stream(target, std::ios::trunc);
    stream << contents;
    return target;
  }

  static std::string read(const std::filesystem::path& target) {
    std::ifstream stream(target);
    return std::string((std::istreambuf_iterator<char>(stream)),
                       std::istreambuf_iterator<char>());
  }

 private:
  static unsigned counter() {
    static unsigned value = 0;
    return ++value;
  }

  std::filesystem::path path_;
};

// Sets an environment variable for the lifetime of the object and restores the
// previous value, so a test that redirects XDG_CONFIG_HOME or DISPLAY cannot
// leak that into the tests that run after it.
class ScopedEnvironment {
 public:
  ScopedEnvironment(const char* name, const char* value) : name_(name) {
    if (const char* previous = ::getenv(name)) {
      had_ = true;
      previous_ = previous;
    }
    if (value)
      ::setenv(name, value, 1);
    else
      ::unsetenv(name);
  }

  ~ScopedEnvironment() {
    if (had_)
      ::setenv(name_.c_str(), previous_.c_str(), 1);
    else
      ::unsetenv(name_.c_str());
  }

  ScopedEnvironment(const ScopedEnvironment&) = delete;
  ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

 private:
  std::string name_, previous_;
  bool had_{false};
};

}  // namespace mistercast::test
