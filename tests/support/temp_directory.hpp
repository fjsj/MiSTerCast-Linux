#pragma once

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

}  // namespace mistercast::test
