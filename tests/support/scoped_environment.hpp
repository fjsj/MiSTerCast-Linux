#pragma once

#include <cstdlib>
#include <string>

namespace mistercast::test {

// Sets an environment variable for the lifetime of the object and restores the
// previous value, so a test that redirects XDG_CONFIG_HOME or DISPLAY cannot
// leak that into the tests that run after it. A null value unsets the variable.
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
