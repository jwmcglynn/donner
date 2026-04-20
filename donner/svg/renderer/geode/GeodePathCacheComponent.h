#pragma once
/// @file
/// Per-entity cache of Geode's CPU encode pipeline output.
///
/// Design doc: docs/design_docs/0030-geode_performance.md §Milestone 2.
///
/// `GeodePathEncoder::encode` (cubicToQuadratic → toMonotonic → band
/// decomposition) is the Tier-3 hot path identified in 0030. Without a
/// cache it runs every frame for every draw — 132 times per frame for
/// `lion.svg`. This component holds the encode result across frames so
/// re-rendering an unchanged document skips the CPU work entirely.
///
/// Invalidation is owned by `RendererGeode`, which listens on entt's
/// `on_update<ComputedPathComponent>` + `on_destroy<ComputedPathComponent>`
/// signals and removes this component whenever the underlying geometry
/// changes. `ShapeSystem`'s content-equality gate (see
/// `emplaceComputedPathIfChanged`) ensures those signals only fire when
/// the path actually changed, so idle re-renders leave the cache intact.

#include <optional>

#include "donner/base/Path.h"
#include "donner/svg/renderer/geode/GeodePathEncoder.h"

namespace donner::geode {

/// Per-entity cache of Geode's encoded-path output (and strokeToFill
/// result). Installed lazily by `RendererGeode` at the encode call
/// sites via `get_or_emplace`; removed automatically when the source
/// `ComputedPathComponent` updates or is destroyed.
///
/// Lives in `donner::geode` (not `donner::svg::geode`) to match the
/// other Geode types (`EncodedPath`, `LinearGradientParams`, …) that
/// `RendererGeode.cc` references unqualified via `geode::` inside the
/// `donner::svg` namespace.
struct GeodePathCacheComponent {
  /// Fill-slot encode. Populated on first encode; reused on hit.
  /// Reset by the entt signal listener when geometry changes.
  std::optional<EncodedPath> fillEncode;

  /// Stroke-slot cache. Holds both the `Path::strokeToFill` output
  /// path and its encoded form, keyed by the source `StrokeStyle`.
  /// Invalidated whenever the fill slot is (geometry change, via the
  /// entt signal), or on stroke-key mismatch (stroke width/dash/cap/
  /// join change via CSS — the old key no longer matches the new one,
  /// so the next access regenerates).
  struct StrokeSlot {
    /// Equality key. Compared against the caller's `StrokeStyle` to
    /// detect stroke-parameter changes.
    StrokeStyle strokeKey;

    /// Cached `Path::strokeToFill` output. Reused across draws of
    /// the same entity + stroke-key combination.
    Path strokedPath;

    /// Cached encode of `strokedPath`. Produced by
    /// `GeodePathEncoder::encode(strokedPath, strokeFillRule)`.
    EncodedPath strokedEncode;

    /// Fill rule the stroke was encoded with. `strokeToFill` picks
    /// NonZero vs EvenOdd based on subpath topology, so this is
    /// derived and cached alongside the encode.
    FillRule strokeFillRule = FillRule::NonZero;
  };
  std::optional<StrokeSlot> strokeSlot;
};

}  // namespace donner::geode
