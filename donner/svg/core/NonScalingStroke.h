#pragma once
/// @file

#include <cstdint>

#include "donner/base/Transform.h"
#include "donner/svg/core/Stroke.h"
#include "donner/svg/core/VectorEffect.h"

namespace donner::svg {

/**
 * How an element's stroke is realized under its current CTM.
 *
 * `vector-effect: non-scaling-stroke` is defined in terms of a constant width in host space, but
 * only geometry that the renderer can map into host space achieves that exactly. This enum names
 * which of the two realizations applies, so the draw path, the culling bounds, and the editor's
 * selection bounds all answer the question the same way.
 */
enum class NonScalingStrokeMode : uint8_t {
  /// Not a non-scaling stroke: the authored width applies in the element's own local space and
  /// scales with the CTM like the geometry does.
  Scaling,
  /// The authored width divided by `sqrt(|det(CTM)|)` applies in local space. Exact when the CTM
  /// is a similarity; under an anisotropic CTM the drawn width is the geometric mean of the two
  /// axis widths, because the geometry is stroked before the CTM is applied.
  ScaledLocal,
  /// The centerline is mapped into host (root canvas) space and stroked there at the authored
  /// width, which is exact under any affine CTM.
  HostSpace,
};

/**
 * Returns whether \p transform's linear part is a similarity: uniform scale combined with rotation
 * and optionally reflection.
 *
 * Under a similarity the CTM's scale is exactly `sqrt(|det|)`, so a non-scaling stroke reaches its
 * authored width from local space with a scalar width adjustment; only a non-uniform scale or
 * shear needs the geometry in host space.
 *
 * @param transform Transform to classify.
 */
bool IsSimilarityTransform(const Transform2d& transform);

/**
 * Resolves how an element's stroke is realized.
 *
 * A CTM with a non-finite component never resolves to \ref NonScalingStrokeMode::HostSpace.
 * Mapping the centerline through it would produce non-finite geometry, and the caches keyed on
 * that transform could never hit again, because a NaN component compares unequal to itself.
 *
 * @param vectorEffect The element's computed `vector-effect`.
 * @param worldFromEntity The element's CTM.
 * @param geometryMustStayLocal True when the stroke pass cannot hand host-space geometry to the
 *   backend, which is the case for a pattern-painted stroke (the tile placement is built in
 *   path-local space) and for text (the stroke expands glyph outlines, not a path).
 */
NonScalingStrokeMode ResolveNonScalingStrokeMode(VectorEffect vectorEffect,
                                                 const Transform2d& worldFromEntity,
                                                 bool geometryMustStayLocal);

/**
 * Returns the stroke width the renderer expands the centerline by, in the space that centerline is
 * stroked in: local space for \ref NonScalingStrokeMode::Scaling and
 * \ref NonScalingStrokeMode::ScaledLocal, host space for \ref NonScalingStrokeMode::HostSpace.
 *
 * @param authoredWidth The computed `stroke-width`.
 * @param mode The element's resolved mode.
 * @param worldFromEntity The element's CTM.
 */
double EffectiveStrokeWidth(double authoredWidth, NonScalingStrokeMode mode,
                            const Transform2d& worldFromEntity);

/**
 * Returns the furthest distance a stroke of \p strokeWidth can reach from its centerline.
 *
 * Half the stroke width, except at the two places a stroke reaches past that:
 *
 * - A miter join's outer tip sits `strokeWidth / (2 * sin(theta/2))` from the vertex, clamped by
 *   `stroke-miterlimit` to `miterLimit * strokeWidth / 2`.
 * - A square cap extends the segment by half the width, putting its far corners
 *   `sqrt(2) * strokeWidth / 2` from the endpoint.
 *
 * Callers inflating a centerline box for a culling or selection decision need that worst case, not
 * the half width. An infinite `stroke-miterlimit` yields an infinite result rather than a smaller
 * finite one: such a bound may only over-count, so the caller must decline to cull instead of
 * substituting a tighter number.
 *
 * @param strokeWidth Stroke width in the space the centerline is stroked in.
 * @param linecap The element's computed `stroke-linecap`.
 * @param linejoin The element's computed `stroke-linejoin`.
 * @param miterLimit The element's computed `stroke-miterlimit`.
 */
double StrokeCullHalfExtent(double strokeWidth, StrokeLinecap linecap, StrokeLinejoin linejoin,
                            double miterLimit);

}  // namespace donner::svg
