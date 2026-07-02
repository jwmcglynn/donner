#include "donner/base/Transform.h"

#include <cmath>

#include "donner/base/FormatNumber.h"

namespace donner {

// Defined out-of-line: Transform.h is included nearly everywhere, and this function's
// `RcString::fromFormat` calls instantiate the `std::format` machinery in every
// including translation unit when defined inline.
RcString toSVGTransformString(const Transform2d& transform) {
  if (transform.isIdentity()) {
    return RcString("");
  }

  const double a = transform.data[0];
  const double b = transform.data[1];
  const double c = transform.data[2];
  const double d = transform.data[3];
  const double e = transform.data[4];
  const double f = transform.data[5];

  // Pure translate: upper-left is identity 2x2.
  if (NearEquals(a, 1.0) && NearZero(b) && NearZero(c) && NearEquals(d, 1.0)) {
    if (NearZero(f)) {
      return RcString::fromFormat("translate({})", detail::FormatNumberForSVG(e));
    }
    return RcString::fromFormat("translate({}, {})", detail::FormatNumberForSVG(e),
                                detail::FormatNumberForSVG(f));
  }

  // Pure rotation around origin: matrix must be [cos, sin, -sin, cos, 0, 0] with
  // `a² + b² ≈ 1` as a final sanity check against near-misses that happen to have
  // the right symmetry but the wrong magnitude (e.g. a rotation+scale composition).
  //
  // This check precedes the scale detection below because `rotate(180°)` =
  // `[-1, 0, 0, -1, 0, 0]` also satisfies the "pure scale" constraints (b=c=0,
  // e=f=0, a=d) — but it's semantically a rotation and the canonical SVG output
  // for that matrix is `rotate(180)`, not `scale(-1)`. Conversely, `scale(2)`
  // has `a² + b² = 4` and fails the `NearEquals(..., 1.0)` check here, so it
  // correctly falls through to the scale branch.
  if (NearZero(e) && NearZero(f) && NearEquals(a, d) && NearEquals(b, -c) &&
      NearEquals(a * a + b * b, 1.0)) {
    const double angleDegrees = std::atan2(b, a) * MathConstants<double>::kRadToDeg;
    return RcString::fromFormat("rotate({})", detail::FormatNumberForSVG(angleDegrees));
  }

  // Pure scale (around origin): no skew or translation.
  if (NearZero(b) && NearZero(c) && NearZero(e) && NearZero(f)) {
    if (NearEquals(a, d)) {
      return RcString::fromFormat("scale({})", detail::FormatNumberForSVG(a));
    }
    return RcString::fromFormat("scale({}, {})", detail::FormatNumberForSVG(a),
                                detail::FormatNumberForSVG(d));
  }

  // General fallback.
  return RcString::fromFormat("matrix({}, {}, {}, {}, {}, {})", detail::FormatNumberForSVG(a),
                              detail::FormatNumberForSVG(b), detail::FormatNumberForSVG(c),
                              detail::FormatNumberForSVG(d), detail::FormatNumberForSVG(e),
                              detail::FormatNumberForSVG(f));
}

}  // namespace donner
