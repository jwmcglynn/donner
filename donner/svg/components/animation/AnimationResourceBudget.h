#pragma once
/// @file

#include <algorithm>
#include <cstddef>
#include <memory>
#include <utility>

namespace donner::svg::components {

/** Shared contextual preparation allowance; used only by serialized document preparation. */
class AnimationContextualWorkBudget {
public:
  /// Total preparation work admitted between resets.
  static constexpr std::size_t kMaximumWork = 4 * 1024 * 1024;

  /// Remaining preparation work across this render operation's document occurrences.
  std::size_t remaining() const { return kMaximumWork - work_; }
  /// Reset only at the outer owner-frame boundary.
  void reset() { work_ = 0; }
  /// Admit work before copying or normalization. @param work Conservative work cost.
  bool reserve(std::size_t work) {
    if (work > remaining()) {
      work_ = kMaximumWork;
      return false;
    }
    work_ += work;
    return true;
  }

private:
  std::size_t work_ = 0;
};

/// Scoped preparation context; never carried by filter graphs, snapshots, or raster replay.
struct AnimationContextualWorkContext {
  std::shared_ptr<AnimationContextualWorkBudget>
      budget;  ///< Owned allowance, without registry links.
};

/** Per-document animation admission with optional owner-frame contextual preparation admission. */
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
  static constexpr std::size_t kMaximumContextualWork = AnimationContextualWorkBudget::kMaximumWork;

  /// Maximum ID hashing, comparison and document traversal work per sample or event.
  static constexpr std::size_t kMaximumReferenceWork = 4 * 1024 * 1024;

  /// Remaining shared reference lookup work.
  std::size_t remainingReferenceWork() const { return kMaximumReferenceWork - referenceWork_; }

  /// Admit reference work before copying IDs, comparing bytes, or advancing the tree.
  /// @param work Conservative byte or node-operation count.
  bool reserveReferenceWork(std::size_t work) {
    if (rejected_ || work > remainingReferenceWork()) {
      rejected_ = true;
      return false;
    }
    referenceWork_ += work;
    return true;
  }

  /// Remaining contextual work; shared by scalar/font stacks and every contextual length consumer.
  std::size_t remainingContextualWork() const {
    const std::size_t local = kMaximumContextualWork - contextualWork_;
    return sharedContextualWork_ ? std::min(local, sharedContextualWork_->remaining()) : local;
  }

  /// Begin preparation for another occurrence without replenishing its shared owner allowance.
  void resetContextualWork() { contextualWork_ = 0; }
  /// Bind an owner allowance, or detach it. @param budget Shared preparation counter, or null.
  void setContextualWorkBudget(std::shared_ptr<AnimationContextualWorkBudget> budget) {
    sharedContextualWork_ = std::move(budget);
  }
  /// Return the owned binding so a nested scope can restore only its own changes.
  std::shared_ptr<AnimationContextualWorkBudget> contextualWorkBudget() const {
    return sharedContextualWork_;
  }

  /// Admit consumer work before evaluating a recipe. Refused consumers retain their base value.
  /// @param work Conservative parsing/interpolation or measured path-work cost.
  bool reserveContextualWork(std::size_t work) {
    if (work > kMaximumContextualWork - contextualWork_ ||
        (sharedContextualWork_ && !sharedContextualWork_->reserve(work))) {
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
  std::shared_ptr<AnimationContextualWorkBudget> sharedContextualWork_;
  std::size_t contextualWork_ = 0;
  std::size_t animations_ = 0;
  std::size_t sourceBytes_ = 0;
  std::size_t outputBytes_ = 0;
  bool rejected_ = false;
  std::size_t geometryWork_ = 0;
  std::size_t referenceWork_ = 0;
};

}  // namespace donner::svg::components
