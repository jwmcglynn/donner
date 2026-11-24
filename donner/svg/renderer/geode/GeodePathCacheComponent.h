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
#include <memory>
#include <optional>

#include "donner/base/Path.h"
#include "donner/base/Transform.h"
#include "donner/svg/renderer/geode/GeodeFillVariants.h"
#include "donner/svg/renderer/geode/GeodePathEncoder.h"
#include "donner/svg/renderer/geode/GeodeResourceBudget.h"

namespace donner::geode {

/// Per-entity cache of Geode's encoded-path output (and strokeToFill
/// results). Installed lazily by `RendererGeode` at the encode call
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
    /// Frame generation of the last draw this variant served, on the device `lastUseDeviceId`.
    /// While a frame of that device at or before this generation is open, a recorded or pending
    /// draw may still read the variant, so it is not replaced.
    std::uint64_t lastUseFrame = 0;
    /// `GeodeDevice::deviceId()` of the device whose generation `lastUseFrame` is.
    std::uint64_t lastUseDeviceId = 0;

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
  /// and an animated scale in the same frame. A further bucket replaces the least recently used
  /// variant, unless every variant may still be read by an open frame; then it draws from a
  /// scratch encode.
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

  /// One cached stroke outline: the `Path::strokeToFill` output of the entity's geometry for one
  /// stroke key, and its encode. The key is everything the outline depends on besides the
  /// geometry, whose changes remove the whole component: the stroke style, the device-derived
  /// flattening tolerance, and the host transform of a `non-scaling-stroke` centerline.
  struct StrokeVariant {
    /// Stroke parameters the outline was built with: width, cap, join, miter limit, dash array,
    /// dash offset, and `pathLength`.
    StrokeStyle strokeKey;

    /// Path-local curve-flattening tolerance the outline was built with, derived from the
    /// draw-time device transform (see `GeodeStrokeTolerance.h`) and quantized to power-of-two
    /// scale buckets, so exact comparison is stable across a continuous zoom.
    double flattenTolerance = 0.0;

    /// Set when the outline was built from a `vector-effect: non-scaling-stroke` centerline
    /// already transformed into host space, to the transform that produced it. A runtime change
    /// of the element's `vector-effect` keeps the same `StrokeStyle`, and the entity-level
    /// invalidation only watches the spline, so a transform-only attribute write, a canvas
    /// resize, or another `<use>` instance would otherwise be served another CTM's geometry.
    std::optional<Transform2d> hostFromLocal;

    /// Cached `Path::strokeToFill` output.
    Path strokedPath;

    /// Cached encode of `strokedPath`, produced with `strokeFillRule`.
    EncodedPath strokedEncode;

    /// Fill rule of the cached stroke geometry. `strokeToFill` produces consistently wound
    /// pieces whose union is encoded with NonZero.
    FillRule strokeFillRule = FillRule::NonZero;

    /// Live retained-byte charge for this variant, as reported by `retainedBytes`.
    GeodeGeometryCacheReservation reservation;
    /// Value of `strokeUseClock` when this variant last served a draw; picks the one to replace.
    std::uint64_t lastUse = 0;
    /// Frame generation of the last draw this variant served, on the device `lastUseDeviceId`.
    /// While a frame of that device at or before this generation is open, a pending batch may
    /// still point at this variant, so it is neither replaced nor freed.
    std::uint64_t lastUseFrame = 0;
    /// `GeodeDevice::deviceId()` of the device whose generation `lastUseFrame` is.
    std::uint64_t lastUseDeviceId = 0;

    /// True when this variant holds the outline a draw with the given key would build.
    ///
    /// Only an exact tolerance match serves a draw: an outline flattened for a coarser device
    /// scale would show its segments at a finer one, and serving a finer outline at a coarser
    /// scale would make a draw's pixels depend on which scale stroked the entity first.
    /// @param style Stroke parameters of the draw.
    /// @param tolerance Device-derived flattening tolerance of the draw.
    /// @param drawHostFromLocal Host transform of a `non-scaling-stroke` draw, or nullopt for a
    ///   local-space draw.
    bool serves(const StrokeStyle& style, double tolerance,
                const std::optional<Transform2d>& drawHostFromLocal) const {
      if (hostFromLocal.has_value() != drawHostFromLocal.has_value() ||
          (hostFromLocal.has_value() && !(*hostFromLocal == *drawHostFromLocal))) {
        return false;
      }
      return flattenTolerance == tolerance && strokeKey == style;
    }

    /// Bytes this variant retains: the variant itself, which is allocated on its own, plus the
    /// dynamic capacity of its outline, encode, and dash key. Nullopt on overflow.
    std::optional<std::size_t> retainedBytes() const {
      const std::optional<std::size_t> pathBytes = strokedPath.retainedBytes();
      const std::size_t encodedBytes = strokedEncode.retainedBytes();
      if (!pathBytes.has_value() || encodedBytes == std::numeric_limits<std::size_t>::max() ||
          strokeKey.dashArray.capacity() >
              std::numeric_limits<std::size_t>::max() / sizeof(double)) {
        return std::nullopt;
      }
      const std::size_t dashBytes = strokeKey.dashArray.capacity() * sizeof(double);
      const std::size_t fixedBytes = sizeof(StrokeVariant);
      if (*pathBytes > std::numeric_limits<std::size_t>::max() - encodedBytes ||
          *pathBytes + encodedBytes > std::numeric_limits<std::size_t>::max() - dashBytes ||
          *pathBytes + encodedBytes + dashBytes >
              std::numeric_limits<std::size_t>::max() - fixedBytes) {
        return std::nullopt;
      }
      return *pathBytes + encodedBytes + dashBytes + fixedBytes;
    }
  };

  /// Number of stroke keys cached at once. Eight covers an element whose `<use>` instances apply
  /// several stroke styles to it. A further key replaces the least recently used variant, unless
  /// every variant may still be read by an open frame (the current one, or an outer frame an
  /// offscreen pass runs inside); then it draws from a per-frame outline, so the keys of one frame
  /// cannot evict each other and a pending batch never loses the outline it points at.
  static constexpr std::size_t kStrokeVariantCount = kGeodeStrokeVariantCount;

  /// Most outlines one stroke style keeps across tolerance buckets and host transforms, the same
  /// bound fills keep across tolerance buckets: a rest scale and an animated one. Zooming through
  /// further scale buckets replaces the style's own least recently used outline instead of
  /// filling the cache.
  static constexpr std::size_t kStrokeVariantsPerStyle = kGeodeFillVariantCount;

  /// Stroke outlines, one per recently drawn stroke key. Each is allocated on first use, so an
  /// entity that is never stroked keeps only the pointers, and stays at one address while cached,
  /// so a pending batch can keep drawing from it. Index `i` pairs with the resident stroke slots
  /// of the same index in `GeodeResidentPathSlots`.
  std::array<std::unique_ptr<StrokeVariant>, kStrokeVariantCount> strokeVariants;
  /// Monotonic use counter for `StrokeVariant::lastUse`.
  std::uint64_t strokeUseClock = 0;

  /// Index of the stroke variant serving a draw with the given key, or nullopt when none does.
  /// @param style Stroke parameters of the draw.
  /// @param tolerance Device-derived flattening tolerance of the draw.
  /// @param hostFromLocal Host transform of a `non-scaling-stroke` draw, or nullopt.
  std::optional<std::size_t> strokeVariantFor(
      const StrokeStyle& style, double tolerance,
      const std::optional<Transform2d>& hostFromLocal) const {
    for (std::size_t i = 0; i < strokeVariants.size(); ++i) {
      if (strokeVariants[i] && strokeVariants[i]->serves(style, tolerance, hostFromLocal)) {
        return i;
      }
    }
    return std::nullopt;
  }
};

}  // namespace donner::geode
