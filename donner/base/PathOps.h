#pragma once
/// @file

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "donner/base/FillRule.h"
#include "donner/base/Path.h"
#include "donner/base/Transform.h"

namespace donner {

/// Boolean operation to apply to filled path inputs.
enum class PathBooleanOp : std::uint8_t {
  Union,       ///< Filled region covered by any input.
  Intersect,   ///< Filled region covered by every input.
  Difference,  ///< Filled region covered by the first input but no later input.
  Xor,         ///< Filled region covered by an odd number of inputs.
};

/// One filled path participating in a boolean operation.
struct PathBooleanInput {
  Path path;                                   ///< Source path geometry.
  FillRule fillRule = FillRule::NonZero;       ///< Fill rule for \ref path.
  Transform2d outputFromPath = Transform2d();  ///< Transform into output coordinates.
};

/// Limits and tolerances for bounded boolean operations.
struct PathBooleanOptions {
  double geometricTolerance = 1e-6;        ///< Geometric comparison tolerance.
  std::size_t maxCurveCount = 100000;      ///< Maximum input segment count.
  std::size_t maxIntersections = 100000;   ///< Maximum segment intersections.
  std::size_t maxOutputCommands = 200000;  ///< Maximum emitted output commands.
};

/// Status for a path boolean result.
enum class PathBooleanStatus : std::uint8_t {
  Ok,            ///< Operation succeeded and produced at least one output path.
  EmptyResult,   ///< Operation succeeded but the requested region is empty.
  InvalidInput,  ///< Input paths or options are invalid.
  TooComplex,    ///< Operation exceeded configured complexity caps.
};

/// Result of \ref ApplyPathBoolean.
struct PathBooleanResult {
  PathBooleanStatus status = PathBooleanStatus::Ok;  ///< Operation status.
  std::vector<Path> paths;                           ///< Output paths on success.
  std::vector<std::string> diagnostics;              ///< Compact diagnostic messages.
};

/**
 * Apply a filled path boolean operation.
 *
 * The implementation is in-tree and preserves line, quadratic, and cubic segments for retained
 * boundary spans. It is intentionally bounded: invalid inputs or operations exceeding configured
 * caps return a non-\ref PathBooleanStatus::Ok status instead of mutating caller state.
 *
 * @param op Boolean operation to apply.
 * @param inputs Filled path inputs in operation order.
 * @param options Tolerances and complexity caps.
 */
PathBooleanResult ApplyPathBoolean(PathBooleanOp op, std::span<const PathBooleanInput> inputs,
                                   const PathBooleanOptions& options = {});

}  // namespace donner
