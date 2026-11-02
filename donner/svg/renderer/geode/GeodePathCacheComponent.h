#pragma once
/// @file
/// Per-entity cache of Geode's CPU encode pipeline output.
///
/// `GeodePathEncoder::encode` (cubicToQuadratic → toMonotonic → band
/// decomposition) is a measured per-frame hot path. Without a
/// cache it runs every frame for every draw - 132 times per frame for
/// `lion.svg`. This component holds the encode result across frames so
/// re-rendering an unchanged document skips the CPU work entirely.
///
/// Invalidation is owned by `RendererGeode`, which listens on entt's
/// `on_update<ComputedPathComponent>` + `on_destroy<ComputedPathComponent>`
/// signals and removes this component whenever the underlying geometry
/// changes. `ShapeSystem`'s content-equality gate (see
/// `emplaceComputedPathIfChanged`) ensures those signals only fire when
/// the path actually changed, so idle re-renders leave the cache intact.

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

#include "donner/base/Path.h"
#include "donner/base/Transform.h"
#include "donner/svg/renderer/geode/GeodePathEncoder.h"
#include "donner/svg/renderer/geode/GeodeResourceBudget.h"

namespace donner::geode {

/// Number of fill encodes an entity keeps, one per device-derived tolerance bucket. See
/// `GeodePathCacheComponent::FillVariant`; `GeodeResidentPathSlots` keeps one resident fill slot
/// per variant.
inline constexpr std::size_t kGeodeFillVariantCount = 2;

/// Per-entity cache of Geode's encoded-path output (and strokeToFill
/// result). Installed lazily by `RendererGeode` at the encode call
/// sites via `get_or_emplace`; removed automatically when the source
/// `ComputedPathComponent` updates or is destroyed.
///
/// Lives in `donner::geode` (not `donner::svg::geode`) to match the
/// other Geode types (`EncodedPath`, `LinearGradientParams`, ...) that
/// `RendererGeode.cc` references unqualified via `geode::` inside the
/// `donner::svg` namespace.
struct GeodePathCacheComponent {
  /// Limit initial storage for this large component while retaining paged pointer stability:
  /// every document registry with a drawn path allocates at least one page.
  static constexpr std::size_t page_size = 64;

  /// One fill encode, built for one device-derived cubic tolerance bucket.
  struct FillVariant {
    /// Cached encode. Reset by the entt signal listener when geometry changes.
    std::optional<EncodedPath> encode;
    /// Approximation tolerance of `encode`; zero for paths without cubic segments, whose
    /// encode is the same at every scale.
    double tolerance = 0.0;
    /// Live retained-byte charge for `encode`.
    GeodeGeometryCacheReservation reservation;
    /// Value of `fillUseClock` when this variant last served a draw; picks the variant to replace.
    std::uint64_t lastUse = 0;

    /// True when this variant holds the encode a draw at \p drawTolerance would build.
    ///
    /// Only an exact bucket match serves the draw. Serving a finer encode at a coarser scale
    /// would make the pixels of a draw depend on which scale encoded the entity first: an
    /// element drawn at two scales would render differently in its first frame than in later
    /// ones, a difference that a blur makes visible.
    bool serves(double drawTolerance) const {
      return encode.has_value() && (tolerance == 0.0 || tolerance == drawTolerance);
    }
  };

  /// Number of tolerance buckets cached at once. Two covers an element drawn at its rest scale
  /// and an animated scale in the same frame; a third bucket replaces the least recently used.
  static constexpr std::size_t kFillVariantCount = kGeodeFillVariantCount;

  /// Fill encodes, one per recently drawn tolerance bucket. Populated on first encode for a
  /// bucket; reused while the bucket stays cached. Index `i` pairs with the resident slots of the
  /// same index in `GeodeResidentPathSlots`.
  std::array<FillVariant, kFillVariantCount> fillVariants;
  /// Monotonic use counter for `FillVariant::lastUse`.
  std::uint64_t fillUseClock = 0;

  /// Index of the variant serving \p drawTolerance, or nullopt when no cached encode matches.
  std::optional<std::size_t> fillVariantFor(double drawTolerance) const {
    for (std::size_t i = 0; i < fillVariants.size(); ++i) {
      if (fillVariants[i].serves(drawTolerance)) {
        return i;
      }
    }
    return std::nullopt;
  }

  /// Stroke-slot cache. Holds both the `Path::strokeToFill` output
  /// path and its encoded form, keyed by the source `StrokeStyle` plus
  /// the device-derived curve-flattening tolerance.
  /// Invalidated whenever the fill slot is (geometry change, via the
  /// entt signal), on stroke-key mismatch (stroke width/dash/cap/
  /// join change via CSS - the old key no longer matches the new one,
  /// so the next access regenerates), or on tolerance mismatch (the
  /// view scale crossed a flattening bucket).
  struct StrokeSlot {
    /// Equality key. Compared against the caller's `StrokeStyle` to
    /// detect stroke-parameter changes.
    StrokeStyle strokeKey;

    /// Second half of the equality key: the path-local curve-flattening
    /// tolerance this outline was built with, derived from the draw-time
    /// device transform (see `GeodeStrokeTolerance.h`). Without it in the
    /// key, zooming in would keep serving an outline tessellated for the
    /// old scale and curves would render as visible polygons. Quantized
    /// upstream to power-of-two scale buckets, so exact comparison is
    /// stable across a continuous zoom.
    double flattenTolerance = 0.0;

    /// Third part of the equality key: set when the cached outline was built from a
    /// `vector-effect: non-scaling-stroke` centerline already transformed into host space, to the
    /// transform that produced it. A runtime change of the element's `vector-effect` keeps the
    /// same `StrokeStyle`, so without this the slot could serve a local-space outline for a
    /// host-space draw; and the entity-level invalidation only watches the spline, so a
    /// transform-only attribute write, a canvas resize, or a second `<use>` instance in the same
    /// frame would otherwise be served the previous CTM's geometry.
    ///
    /// One slot serves both the local-space and host-space variants of an entity, so an element
    /// that draws both in a frame rebuilds the outline each time. The tiny-skia backend keeps the
    /// two variants in separate slots. That is a cache-effectiveness asymmetry, not a correctness
    /// one: the key below distinguishes the variants, so a mismatched slot is always rebuilt.
    std::optional<Transform2d> hostFromLocal;

    /// Cached `Path::strokeToFill` output. Reused across draws of
    /// the same entity + stroke-key combination.
    Path strokedPath;

    /// Cached encode of `strokedPath`. Produced by
    /// `GeodePathEncoder::encode(strokedPath, strokeFillRule)`.
    EncodedPath strokedEncode;

    /// Fill rule used for the cached stroke geometry.
    /// `strokeToFill` produces consistently wound pieces whose
    /// union is encoded with NonZero.
    FillRule strokeFillRule = FillRule::NonZero;

    /// Exact dynamic capacity retained by this cached stroke entry.
    std::optional<std::size_t> retainedBytes() const {
      const std::optional<std::size_t> pathBytes = strokedPath.retainedBytes();
      const std::size_t encodedBytes = strokedEncode.retainedBytes();
      if (!pathBytes.has_value() || encodedBytes == std::numeric_limits<std::size_t>::max() ||
          strokeKey.dashArray.capacity() >
              std::numeric_limits<std::size_t>::max() / sizeof(double)) {
        return std::nullopt;
      }
      const std::size_t dashBytes = strokeKey.dashArray.capacity() * sizeof(double);
      if (*pathBytes > std::numeric_limits<std::size_t>::max() - encodedBytes ||
          *pathBytes + encodedBytes > std::numeric_limits<std::size_t>::max() - dashBytes) {
        return std::nullopt;
      }
      return *pathBytes + encodedBytes + dashBytes;
    }
  };
  std::optional<StrokeSlot>
      strokeSlot;  //!< Cached encoded stroke geometry and its resident GPU resources.

  /// Live retained-byte charge for `strokeSlot`.
  GeodeGeometryCacheReservation strokeReservation;
};

}  // namespace donner::geode
