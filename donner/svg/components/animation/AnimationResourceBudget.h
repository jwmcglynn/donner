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

  /// Maximum path parsing and arc-length work across one animation sample.
  static constexpr std::size_t kMaximumGeometryWork = 262144;

  /// Aggregate contextual-length evaluation and measurement work during render preparation.
  static constexpr std::size_t kMaximumContextualWork = 4 * 1024 * 1024;

  /// Remaining contextual work; shared by every text span and marker reference in this sample.
  std::size_t remainingContextualWork() const { return kMaximumContextualWork - contextualWork_; }

  /// Admit consumer work before evaluating a recipe. Refused consumers retain their base value.
  /// @param work Conservative parsing/interpolation or measured path-work cost.
  bool reserveContextualWork(std::size_t work) {
    if (work > remainingContextualWork()) {
      contextualWork_ = kMaximumContextualWork;
      return false;
    }
    contextualWork_ += work;
    return true;
  }

  /// Remaining path work for this frame.
  std::size_t remainingGeometryWork() const { return kMaximumGeometryWork - geometryWork_; }

  /// Charge bounded path parsing or measurement work; reject on exhaustion.
  bool reserveGeometryWork(std::size_t work) {
    if (work > remainingGeometryWork()) {
      rejected_ = true;
      return false;
    }
    geometryWork_ += work;
    return true;
  }

  /// Charge one animation and its source payload to the frame envelope.
  /// @param sourceBytes Source bytes retained for this animation.
  /// @return False when the animation-count or source-byte limit is exceeded.
  bool reserveAnimation(std::size_t sourceBytes) {
    if (animations_ >= kMaximumAnimations) {
      rejected_ = true;
      return false;
    }
    ++animations_;
    return reserveSourceBytes(sourceBytes);
  }

  /// Admit additional source bytes before copying an underlying or referenced value.
  /// @param bytes Source bytes to retain or parse.
  /// @return False when the aggregate source envelope is exceeded.
  bool reserveSourceBytes(std::size_t bytes) {
    if (bytes > kMaximumSourceBytes - sourceBytes_) {
      rejected_ = true;
      return false;
    }
    sourceBytes_ += bytes;
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
  /// Whether declaration, source, output or path-work admission rejected the sample.
  bool rejected() const { return rejected_; }

private:
  std::size_t contextualWork_ = 0;
  std::size_t animations_ = 0;
  std::size_t sourceBytes_ = 0;
  std::size_t outputBytes_ = 0;
  bool rejected_ = false;
  std::size_t geometryWork_ = 0;
};

}  // namespace donner::svg::components
