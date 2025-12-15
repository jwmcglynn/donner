#pragma once
/// @file

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <utility>
#include <vector>

namespace donner::backends::tiny_skia_cpp {

/** Draws at the beginning and end of an open path contour. */
enum class LineCap : uint8_t {
  kButt,
  kRound,
  kSquare,
};

inline std::ostream& operator<<(std::ostream& os, LineCap value) {
  switch (value) {
    case LineCap::kButt: return os << "LineCap::kButt";
    case LineCap::kRound: return os << "LineCap::kRound";
    case LineCap::kSquare: return os << "LineCap::kSquare";
  }
  return os;
}

/** Specifies how corners are drawn when a shape is stroked. */
enum class LineJoin : uint8_t {
  kMiter,
  kMiterClip,
  kRound,
  kBevel,
};

inline std::ostream& operator<<(std::ostream& os, LineJoin value) {
  switch (value) {
    case LineJoin::kMiter: return os << "LineJoin::kMiter";
    case LineJoin::kMiterClip: return os << "LineJoin::kMiterClip";
    case LineJoin::kRound: return os << "LineJoin::kRound";
    case LineJoin::kBevel: return os << "LineJoin::kBevel";
  }
  return os;
}

/**
 * Stroke dashing properties.
 *
 * Guarantees:
 * - Dash array contains an even number of values and at least two entries.
 * - All dash entries are finite and non-negative.
 * - Dash array sum is finite and positive.
 * - Dash offset is finite and normalized to [0, intervalLength).
 */
class StrokeDash {
public:
  /** Validates and constructs a StrokeDash instance. */
  static std::optional<StrokeDash> Create(std::vector<float> dashArray, float dashOffset);

  /** Total length of one dash interval (on + off). */
  float intervalLength() const { return intervalLength_; }

  /** Normalized offset in [0, intervalLength). */
  float offset() const { return offset_; }

  /** Remaining length in the first dash/gap interval after applying the offset. */
  float firstLength() const { return firstLength_; }

  /** Index of the starting dash element after applying the offset. */
  size_t firstIndex() const { return firstIndex_; }

  /** Underlying dash array. */
  const std::vector<float>& array() const { return dashArray_; }

private:
  StrokeDash(std::vector<float> dashArray, float offset, float intervalLength, float firstLength,
             size_t firstIndex)
      : dashArray_(std::move(dashArray)),
        offset_(offset),
        intervalLength_(intervalLength),
        firstLength_(firstLength),
        firstIndex_(firstIndex) {}

  std::vector<float> dashArray_;
  float offset_ = 0.0f;
  float intervalLength_ = 0.0f;
  float firstLength_ = 0.0f;
  size_t firstIndex_ = 0;
};

/** Stroke properties for path stroking. */
struct Stroke {
  /// A stroke thickness. When set to 0, a hairline stroke is used.
  float width = 1.0f;
  /// The limit at which a sharp corner is drawn beveled.
  float miterLimit = 4.0f;
  /// A stroke line cap.
  LineCap lineCap = LineCap::kButt;
  /// A stroke line join.
  LineJoin lineJoin = LineJoin::kMiter;
  /// Optional stroke dashing properties.
  std::optional<StrokeDash> dash;
};

}  // namespace donner::backends::tiny_skia_cpp
