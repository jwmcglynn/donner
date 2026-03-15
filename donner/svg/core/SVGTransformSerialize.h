#pragma once
/// @file
///
/// Serialize a \ref donner::Transformd to SVG `transform` attribute format.

#include "donner/base/RcString.h"
#include "donner/base/Transform.h"

namespace donner::svg {

/**
 * Serialize a transform to SVG `transform` attribute format.
 *
 * Attempts to decompose the transform into the simplest representation:
 * - Identity → empty string
 * - Pure translation → `"translate(tx, ty)"`
 * - Pure uniform scale → `"scale(s)"`
 * - Pure non-uniform scale → `"scale(sx, sy)"`
 * - General → `"matrix(a, b, c, d, e, f)"`
 *
 * @param transform The transform to serialize.
 * @return SVG transform attribute string, or empty if identity.
 */
[[nodiscard]] RcString toSVGTransformString(const Transformd& transform);

}  // namespace donner::svg
