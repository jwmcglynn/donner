#pragma once
/// @file
/// Per-entity caches for the conversions \ref donner::svg::RendererTinySkia runs on the way
/// into tiny-skia.
///
/// Both conversions below are pure functions of document state that does not change between
/// frames, yet the backend re-ran them on every draw of every frame:
///
/// - `donner::Path` -> `tiny_skia::Path`, which allocates a verb buffer and a point buffer per
///   shape per frame. A document with a few hundred shapes pays that several hundred times a
///   frame for geometry that never moved.
/// - An image's straight-alpha pixels -> premultiplied pixels, a full-buffer pass plus a
///   full-buffer allocation for every image draw.
///
/// Invalidation is owned by \ref donner::svg::RendererTinySkia, which connects entt
/// `on_update` + `on_destroy` listeners for the source component and removes the matching cache
/// component whenever that source changes. A cached conversion that outlived its source would be
/// a correctness bug, not a stale optimization, so the listeners are wired before the first
/// cache entry is installed and cover both mutation and teardown. `ShapeSystem`'s
/// content-equality gate (`emplaceComputedPathIfChanged`) keeps the path signals from firing on
/// an unchanged re-render, so an idle frame keeps its caches intact.

#include <cstdint>
#include <optional>
#include <vector>

#include "tiny_skia/Path.h"

namespace donner::svg {

/// Per-entity cache of a shape's `donner::Path` converted to `tiny_skia::Path`.
///
/// Installed lazily at the draw call sites via `get_or_emplace`, keyed by
/// `PathShape::sourceEntity`; removed by the `ComputedPathComponent` listener when the geometry
/// changes or the entity goes away. A null source entity (overlay drawing, test harnesses) is
/// not cached and converts inline, exactly as before the cache existed.
struct TinySkiaPathCacheComponent {
  /// Conversion that preserves `ClosePath` verbs. Used by fills and by every ordinary stroke.
  std::optional<tiny_skia::Path> closedPath;

  /// Conversion that replaces each `ClosePath` with an explicit closing line. Used only by the
  /// dash-stroke path that has to emit caps at a closed contour's seam, so it is filled in on
  /// demand rather than alongside \ref closedPath.
  std::optional<tiny_skia::Path> openedPath;
};

/// Per-entity cache of a `LoadedImageComponent`'s pixels converted from the straight-alpha
/// `ImageResource` contract to the premultiplied form tiny-skia samples.
///
/// Installed lazily by `drawImage` via `get_or_emplace`, keyed by `ImageParams::sourceEntity`;
/// removed by the `LoadedImageComponent` listener when the element loads different pixels or
/// goes away. The dimensions are stored alongside the payload and rechecked on every hit, so a
/// draw whose source no longer matches the cached buffer reconverts instead of sampling the
/// wrong extent.
struct TinySkiaImageCacheComponent {
  /// Premultiplied RGBA8, tightly packed.
  std::vector<std::uint8_t> premultiplied;
  /// Width the payload was converted at, in pixels.
  int width = 0;
  /// Height the payload was converted at, in pixels.
  int height = 0;
};

}  // namespace donner::svg
