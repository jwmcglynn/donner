#pragma once
/// @file
/// Device-aware curve-flattening tolerance for stroke-outline generation.
///
/// `Path::strokeToFill` flattens curves to line segments before offsetting
/// them, and the tolerance it takes is expressed in the path's OWN coordinate
/// space. A renderer that submits geometry in document space and lets the GPU
/// apply the view transform therefore gets a polygon that was tessellated once
/// at document scale and then magnified: at any meaningful zoom a circle
/// (4 kappa cubics) degenerates into a visibly faceted segment chain.
///
/// The fix is to derive the path-local tolerance from the transform the
/// geometry is actually rasterized with, so the chord error stays bounded in
/// DEVICE pixels no matter what the view scale is. Curves rendering as visible
/// line segments is a correctness violation, not a cosmetic one, so this
/// derivation is mandatory for every renderer-side stroke call site.
///
/// The derived scale is quantized to power-of-two buckets so that a continuous
/// zoom gesture does not invalidate the stroke cache on every frame: within a
/// bucket the tolerance is bit-identical, and crossing a bucket boundary
/// re-derives (and, via the cache key, re-flattens) exactly once.

#include <algorithm>
#include <cmath>

#include "donner/base/Transform.h"

namespace donner::geode {

/// Target flattening chord error for stroke outlines, in device pixels. Matches
/// the historical path-local default so unscaled rendering is unchanged.
inline constexpr double kStrokeFlattenDevicePixels = 0.25;

/// Lower clamp for the derived path-local tolerance. Bounds tessellation work
/// (and recursion depth) at absurd zoom levels. The device-pixel guarantee
/// holds up to a scale of `kStrokeFlattenDevicePixels / kMinStrokeFlattenTolerance`.
inline constexpr double kMinStrokeFlattenTolerance = 1e-4;

/// Upper clamp for the derived path-local tolerance: the historical path-local
/// default. The derivation only ever REFINES flattening, never coarsens it.
///
/// Coarsening minified geometry would technically stay inside the device-pixel
/// budget, but it trades output quality for CPU on content that is not the
/// defect this derivation exists to fix, and it perturbs the rendering of every
/// downscaled document (filter sub-renders, pattern tiles, thumbnails). Refining
/// only keeps the change strictly additive.
inline constexpr double kMaxStrokeFlattenTolerance = kStrokeFlattenDevicePixels;

/**
 * Largest factor by which \p transform can stretch a vector, i.e. the maximum
 * singular value of its 2x2 linear part.
 *
 * Using the maximum singular value (rather than, say, the average of the axis
 * scales) is what makes the device-space bound a guarantee: for any local-space
 * error vector `e`, `|M e| <= sigmaMax * |e|`, so bounding the local chord
 * error by `target / sigmaMax` bounds the device chord error by `target`.
 *
 * @param transform Transform whose linear part is measured. Translation is ignored.
 */
inline double MaxAbsScaleFactor(const Transform2d& transform) {
  const double a = transform.data[0];
  const double b = transform.data[1];
  const double c = transform.data[2];
  const double d = transform.data[3];

  // Closed-form maximum singular value of [[a, c], [b, d]]:
  //   sqrt( ( (a^2+b^2+c^2+d^2) + sqrt( (a^2+b^2-c^2-d^2)^2 + 4 (ac+bd)^2 ) ) / 2 )
  const double col0 = a * a + b * b;
  const double col1 = c * c + d * d;
  const double cross = a * c + b * d;
  const double sum = col0 + col1;
  const double diff = col0 - col1;
  const double root = std::sqrt(diff * diff + 4.0 * cross * cross);
  const double sigmaSquared = 0.5 * (sum + root);
  return sigmaSquared > 0.0 ? std::sqrt(sigmaSquared) : 0.0;
}

/**
 * Round \p scale up to the next power of two, with a floor of 1.
 *
 * Quantizing before deriving the tolerance keeps the tolerance stable across a
 * continuous zoom, so the stroke cache is rebuilt on bucket crossings only.
 * Rounding UP (never down) keeps the derived tolerance conservative: the
 * flattening is at least as fine as the exact scale demands.
 *
 * The floor of 1 is what makes the derivation refine-only - see
 * \ref kMaxStrokeFlattenTolerance. Minified geometry keeps the path-local
 * default rather than being tessellated more coarsely.
 *
 * @param scale Linear scale factor. Non-finite and non-positive inputs fall
 *   back to 1.0 (a degenerate transform paints nothing, so any bucket works).
 */
inline double StrokeFlattenScaleBucket(double scale) {
  if (!std::isfinite(scale) || scale <= 1.0) {
    return 1.0;
  }
  const double bucket = std::exp2(std::ceil(std::log2(scale)));
  return (std::isfinite(bucket) && bucket > 0.0) ? bucket : 1.0;
}

/**
 * Path-local curve-flattening tolerance for stroke geometry that will be drawn
 * with \p deviceFromLocal.
 *
 * The result keeps the flattening chord error under
 * `kStrokeFlattenDevicePixels` device pixels for every scale up to
 * `kStrokeFlattenDevicePixels / kMinStrokeFlattenTolerance`, and is clamped to
 * `[kMinStrokeFlattenTolerance, kMaxStrokeFlattenTolerance]`.
 *
 * @param deviceFromLocal Transform applied to the geometry at draw time.
 */
inline double StrokeFlattenToleranceFor(const Transform2d& deviceFromLocal) {
  const double bucket = StrokeFlattenScaleBucket(MaxAbsScaleFactor(deviceFromLocal));
  return std::clamp(kStrokeFlattenDevicePixels / bucket, kMinStrokeFlattenTolerance,
                    kMaxStrokeFlattenTolerance);
}

}  // namespace donner::geode
