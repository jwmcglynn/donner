#pragma once
/// @file

#include <cstddef>

namespace donner::svg::components {

/** Per-frame work and allocation envelope for untrusted SMIL animation values. */
class AnimationResourceBudget {
public:
  /// Maximum number of animation declarations accepted in one frame.
  static constexpr std::size_t kMaximumAnimations = 4096;
  /// Maximum aggregate source bytes retained for animation values.
  static constexpr std::size_t kMaximumSourceBytes = 1024 * 1024;
  /// Maximum aggregate output bytes produced by animation evaluation.
  static constexpr std::size_t kMaximumOutputBytes = 4 * 1024 * 1024;
  /// Maximum numeric values accepted in one animated value.
  static constexpr std::size_t kMaximumNumbersPerValue = 64;
  /// Maximum source bytes accepted for one animated path value.
  static constexpr std::size_t kMaximumPathValueBytes = 64 * 1024;

  /// Charge one animation and its source payload to the frame envelope.
  /// @param sourceBytes Source bytes retained for this animation.
  /// @return False when the animation-count or source-byte limit is exceeded.
  bool reserveAnimation(std::size_t sourceBytes) {
    if (animations_ >= kMaximumAnimations || sourceBytes > kMaximumSourceBytes - sourceBytes_) {
      rejected_ = true;
      return false;
    }
    ++animations_;
    sourceBytes_ += sourceBytes;
    return true;
  }

  /// Charge produced animation data to the frame output envelope.
  /// @param bytes Additional output bytes to reserve.
  /// @return False when the output-byte limit is exceeded.
  bool reserveOutput(std::size_t bytes) {
    if (bytes > kMaximumOutputBytes - outputBytes_) {
      rejected_ = true;
      return false;
    }
    outputBytes_ += bytes;
    return true;
  }

  /// Number of animations admitted so far.
  std::size_t animations() const { return animations_; }
  /// Aggregate animation source bytes admitted so far.
  std::size_t sourceBytes() const { return sourceBytes_; }
  /// Aggregate animation output bytes admitted so far.
  std::size_t outputBytes() const { return outputBytes_; }
  /// Whether any admission exceeded its corresponding limit.
  bool rejected() const { return rejected_; }

private:
  std::size_t animations_ = 0;
  std::size_t sourceBytes_ = 0;
  std::size_t outputBytes_ = 0;
  bool rejected_ = false;
};

}  // namespace donner::svg::components
