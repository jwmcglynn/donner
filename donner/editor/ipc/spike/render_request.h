#pragma once
/// @file
/// Spike-only struct used by the Teleport IPC reflection-codec feasibility
/// test (M0.1). NOT part of the production editor API.
///
/// The fields are intentionally a small mix of:
///   * fixed-width integers,
///   * a heap-allocated string,
///   * a nested POD,
/// so the codec exercises both trivially-copyable and non-trivial paths.

#include <cstdint>
#include <string>

namespace donner::teleport {

/// Nested POD intentionally distinct from donner::Vector2 — we want the spike
/// to be self-contained and avoid pulling in the base library.
struct Vector2d {
  double x;
  double y;
};

struct RenderRequest {
  std::int32_t width;
  std::int32_t height;
  std::string svg_source;
  Vector2d cursor;
};

}  // namespace donner::teleport
