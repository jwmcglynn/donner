#pragma once
/// @file

#include <cstdlib>
#include <string>

namespace donner {

/**
 * Sets or unsets an environment variable for one test and restores the previous state afterwards.
 *
 * Tests whose behavior depends on an ambient marker need to force both answers, and Bazel scrubs
 * the test environment, so neither answer can be relied on to be the ambient one.
 */
class ScopedEnvironmentVariable {
public:
  /**
   * Sets \p name to \p value, or unsets it when \p value is null.
   *
   * @param name Environment variable name.
   * @param value Value to set, or nullptr to unset.
   */
  ScopedEnvironmentVariable(const char* name, const char* value) : name_(name) {
    if (const char* previous = std::getenv(name); previous != nullptr) {
      previous_ = previous;
      hadPrevious_ = true;
    }
    if (value != nullptr) {
      setenv(name, value, 1);
    } else {
      unsetenv(name);
    }
  }

  ~ScopedEnvironmentVariable() {
    if (hadPrevious_) {
      setenv(name_.c_str(), previous_.c_str(), 1);
    } else {
      unsetenv(name_.c_str());
    }
  }

  ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
  ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

private:
  std::string name_;
  std::string previous_;
  bool hadPrevious_ = false;
};

}  // namespace donner
