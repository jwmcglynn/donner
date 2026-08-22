#pragma once
/// @file

#include <cstddef>

namespace donner::svg::components {

/** Per-frame work and allocation envelope for untrusted SMIL animation values. */
class AnimationResourceBudget {
public:
  static constexpr std::size_t kMaximumAnimations = 4096;
  static constexpr std::size_t kMaximumSourceBytes = 1024 * 1024;
  static constexpr std::size_t kMaximumOutputBytes = 4 * 1024 * 1024;
  static constexpr std::size_t kMaximumNumbersPerValue = 64;
  static constexpr std::size_t kMaximumPathValueBytes = 64 * 1024;

  bool reserveAnimation(std::size_t sourceBytes) {
    if (animations_ >= kMaximumAnimations || sourceBytes > kMaximumSourceBytes - sourceBytes_) {
      rejected_ = true;
      return false;
    }
    ++animations_;
    sourceBytes_ += sourceBytes;
    return true;
  }

  bool reserveOutput(std::size_t bytes) {
    if (bytes > kMaximumOutputBytes - outputBytes_) {
      rejected_ = true;
      return false;
    }
    outputBytes_ += bytes;
    return true;
  }

  std::size_t animations() const { return animations_; }
  std::size_t sourceBytes() const { return sourceBytes_; }
  std::size_t outputBytes() const { return outputBytes_; }
  bool rejected() const { return rejected_; }

private:
  std::size_t animations_ = 0;
  std::size_t sourceBytes_ = 0;
  std::size_t outputBytes_ = 0;
  bool rejected_ = false;
};

}  // namespace donner::svg::components
