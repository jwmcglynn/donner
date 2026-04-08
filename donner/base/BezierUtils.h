#pragma once
/// @file

#include <array>
#include <utility>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/SmallVector.h"
#include "donner/base/Vector2.h"

namespace donner {

/**
 * @defgroup bezier_utils Bezier Utilities
 *
 * Free functions for evaluating, splitting, approximating, and computing bounding boxes of
 * quadratic and cubic Bezier curves.
 *
 * These are used by the GPU rendering backend (Geode) for path processing.
 *
 * @{
 */

/**
 * Evaluate a quadratic Bezier curve at parameter \p t using the standard basis expansion.
 *
 * \f$ B(t) = (1-t)^2 p_0 + 2t(1-t) p_1 + t^2 p_2 \f$
 *
 * @param p0 Start point.
 * @param p1 Control point.
 * @param p2 End point.
 * @param t Parameter in [0, 1].
 * @return The point on the curve at parameter \p t.
 */
Vector2d EvalQuadratic(const Vector2d& p0, const Vector2d& p1, const Vector2d& p2, double t);

/**
 * Evaluate a cubic Bezier curve at parameter \p t using the standard basis expansion.
 *
 * \f$ B(t) = (1-t)^3 p_0 + 3t(1-t)^2 p_1 + 3t^2(1-t) p_2 + t^3 p_3 \f$
 *
 * @param p0 Start point.
 * @param p1 First control point.
 * @param p2 Second control point.
 * @param p3 End point.
 * @param t Parameter in [0, 1].
 * @return The point on the curve at parameter \p t.
 */
Vector2d EvalCubic(const Vector2d& p0, const Vector2d& p1, const Vector2d& p2, const Vector2d& p3,
                   double t);

/**
 * Split a quadratic Bezier curve at parameter \p t using De Casteljau subdivision.
 *
 * Returns two quadratic curves whose union equals the original curve.
 *
 * @param p0 Start point.
 * @param p1 Control point.
 * @param p2 End point.
 * @param t Split parameter in [0, 1].
 * @return A pair of {left, right} point arrays, each describing a quadratic Bezier.
 */
std::pair<std::array<Vector2d, 3>, std::array<Vector2d, 3>> SplitQuadratic(const Vector2d& p0,
                                                                           const Vector2d& p1,
                                                                           const Vector2d& p2,
                                                                           double t);

/**
 * Split a cubic Bezier curve at parameter \p t using De Casteljau subdivision.
 *
 * Returns two cubic curves whose union equals the original curve.
 *
 * @param p0 Start point.
 * @param p1 First control point.
 * @param p2 Second control point.
 * @param p3 End point.
 * @param t Split parameter in [0, 1].
 * @return A pair of {left, right} point arrays, each describing a cubic Bezier.
 */
std::pair<std::array<Vector2d, 4>, std::array<Vector2d, 4>> SplitCubic(const Vector2d& p0,
                                                                       const Vector2d& p1,
                                                                       const Vector2d& p2,
                                                                       const Vector2d& p3,
                                                                       double t);

/**
 * Approximate a cubic Bezier curve as a sequence of quadratic Bezier curves within a given
 * tolerance.
 *
 * Appends quadratic control-point pairs (control, end) to \p out. The start point of each
 * quadratic is the end point of the previous one (or \p p0 for the first). Uses recursive
 * subdivision with a maximum depth of 10 to avoid infinite loops.
 *
 * @param p0 Start point.
 * @param p1 First control point.
 * @param p2 Second control point.
 * @param p3 End point.
 * @param tolerance Maximum allowed distance between the cubic and its quadratic approximation.
 * @param[out] out Output vector to which (control, end) point pairs are appended.
 */
void ApproximateCubicWithQuadratics(const Vector2d& p0, const Vector2d& p1, const Vector2d& p2,
                                    const Vector2d& p3, double tolerance,
                                    std::vector<Vector2d>& out);

/**
 * Find parameter values where the Y-derivative is zero for a quadratic Bezier curve.
 *
 * These are the Y-monotonic split points. Returns 0 or 1 parameter values in the open interval
 * (0, 1).
 *
 * @param p0 Start point.
 * @param p1 Control point.
 * @param p2 End point.
 * @return A SmallVector containing 0 or 1 parameter values.
 */
SmallVector<double, 1> QuadraticYExtrema(const Vector2d& p0, const Vector2d& p1,
                                         const Vector2d& p2);

/**
 * Find parameter values where the Y-derivative is zero for a cubic Bezier curve.
 *
 * These are the Y-monotonic split points. Returns 0, 1, or 2 parameter values in the open
 * interval (0, 1), sorted in ascending order.
 *
 * @param p0 Start point.
 * @param p1 First control point.
 * @param p2 Second control point.
 * @param p3 End point.
 * @return A SmallVector containing 0 to 2 parameter values.
 */
SmallVector<double, 2> CubicYExtrema(const Vector2d& p0, const Vector2d& p1, const Vector2d& p2,
                                     const Vector2d& p3);

/**
 * Compute the tight axis-aligned bounding box of a quadratic Bezier curve.
 *
 * Evaluates the curve at both X and Y extrema (not just the control-point hull) to produce the
 * tightest axis-aligned bounds.
 *
 * @param p0 Start point.
 * @param p1 Control point.
 * @param p2 End point.
 * @return The tight bounding box.
 */
Box2d QuadraticBounds(const Vector2d& p0, const Vector2d& p1, const Vector2d& p2);

/**
 * Compute the tight axis-aligned bounding box of a cubic Bezier curve.
 *
 * Evaluates the curve at both X and Y extrema (not just the control-point hull) to produce the
 * tightest axis-aligned bounds.
 *
 * @param p0 Start point.
 * @param p1 First control point.
 * @param p2 Second control point.
 * @param p3 End point.
 * @return The tight bounding box.
 */
Box2d CubicBounds(const Vector2d& p0, const Vector2d& p1, const Vector2d& p2, const Vector2d& p3);

/** @} */

}  // namespace donner
