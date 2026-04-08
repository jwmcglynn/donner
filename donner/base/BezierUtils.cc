#include "donner/base/BezierUtils.h"

#include <algorithm>
#include <cmath>

#include "donner/base/MathUtils.h"

namespace donner {

namespace {

/// Linear interpolation between two points using double precision.
Vector2d Lerp2d(const Vector2d& a, const Vector2d& b, double t) {
  return a * (1.0 - t) + b * t;
}

/// Maximum recursion depth for cubic-to-quadratic approximation.
constexpr int kMaxApproximationDepth = 10;

/// Recursive helper for ApproximateCubicWithQuadratics.
void approximateCubicWithQuadraticsImpl(const Vector2d& p0, const Vector2d& p1, const Vector2d& p2,
                                        const Vector2d& p3, double tolerance,
                                        std::vector<Vector2d>& out, int depth) {
  // Compute the best-fit quadratic control point: Q1 = (3*(p1+p2) - p0 - p3) / 4
  const Vector2d q1 = (3.0 * (p1 + p2) - p0 - p3) * 0.25;

  // Measure the maximum distance between the cubic and the quadratic approximation.
  // The error is bounded by the distance between the cubic's control points and the
  // quadratic's interpolated control points. A conservative estimate is the max of:
  //   |p1 - lerp(p0, q1, 1/3)| and |p2 - lerp(q1, p3, 2/3)|
  // But a simpler and common metric is: max(|p1 - q1_at_1/3|, |p2 - q1_at_2/3|)
  // where q1_at_t is the quadratic evaluated at t.
  //
  // A practical approach: compute the distance between the cubic control points and the
  // quadratic control polygon. The error bound for a cubic approximated by a single quadratic
  // is: max(dist(p1, lerp(p0, q1, 1/3)), dist(p2, lerp(q1, p3, 2/3)))
  // which simplifies to checking the deviation of p1 and p2 from the quadratic control polygon.
  //
  // However, the simplest correct metric is:
  // d = max(|(3*p1 - p0)/2 - (3*q1 - p0)/2|, |(3*p2 - p3)/2 - (3*q1 - p3)/2|) / 2
  // Simplifying: d = max(|p1 - (2*q1 + p0)/3|, |p2 - (2*q1 + p3)/3|)
  //
  // An even simpler bound: the max distance between the cubic evaluated at several sample points
  // and the quadratic at the same points. But to keep this efficient, we use the distance
  // between the "inner" cubic control points and where they'd need to be for an exact quadratic:
  // For a quadratic with control point q1, the equivalent cubic has inner control points at
  //   c1 = p0 + 2/3*(q1 - p0) and c2 = p3 + 2/3*(q1 - p3)
  // So the error is max(|p1 - c1|, |p2 - c2|).

  const Vector2d c1 = p0 + (q1 - p0) * (2.0 / 3.0);
  const Vector2d c2 = p3 + (q1 - p3) * (2.0 / 3.0);
  const double err1 = (p1 - c1).length();
  const double err2 = (p2 - c2).length();
  const double error = std::max(err1, err2);

  if (error <= tolerance || depth >= kMaxApproximationDepth) {
    // Emit the quadratic: (control point, end point)
    out.push_back(q1);
    out.push_back(p3);
    return;
  }

  // Split at t=0.5 and recurse.
  auto [left, right] = SplitCubic(p0, p1, p2, p3, 0.5);
  approximateCubicWithQuadraticsImpl(left[0], left[1], left[2], left[3], tolerance, out,
                                     depth + 1);
  approximateCubicWithQuadraticsImpl(right[0], right[1], right[2], right[3], tolerance, out,
                                     depth + 1);
}

/// Find parameter values in (0,1) where dx/dt = 0 for a quadratic Bezier.
SmallVector<double, 1> QuadraticXExtrema(const Vector2d& p0, const Vector2d& p1,
                                         const Vector2d& p2) {
  SmallVector<double, 1> result;

  // dx/dt = 2(1-t)(p1.x - p0.x) + 2t(p2.x - p1.x) = 0
  // Solving: t = (p0.x - p1.x) / (p0.x - 2*p1.x + p2.x)
  const double denom = p0.x - 2.0 * p1.x + p2.x;
  if (std::abs(denom) > 1e-12) {
    const double t = (p0.x - p1.x) / denom;
    if (t > 0.0 && t < 1.0) {
      result.push_back(t);
    }
  }

  return result;
}

/// Find parameter values in (0,1) where dx/dt = 0 for a cubic Bezier.
SmallVector<double, 2> CubicXExtrema(const Vector2d& p0, const Vector2d& p1, const Vector2d& p2,
                                     const Vector2d& p3) {
  SmallVector<double, 2> result;

  // dx/dt = 3[(1-t)^2(p1.x-p0.x) + 2t(1-t)(p2.x-p1.x) + t^2(p3.x-p2.x)]
  // Expanding: at^2 + bt + c = 0 where
  //   a = p0.x - 3*p1.x + 3*p2.x - p3.x (note: derivative is 3*(a*t^2+b*t+c))
  //   b = 2*(p1.x - 2*p2.x + p3.x) ... wait, let's be more careful.
  //
  // B'(t) = 3[(p1-p0)(1-t)^2 + 2(p2-p1)t(1-t) + (p3-p2)t^2] for the full curve.
  // For x component: B'_x(t) = 3[(p1.x-p0.x)(1-t)^2 + 2(p2.x-p1.x)t(1-t) + (p3.x-p2.x)t^2]
  // Setting to zero and dividing by 3:
  //   (p1.x-p0.x)(1-2t+t^2) + 2(p2.x-p1.x)(t-t^2) + (p3.x-p2.x)t^2 = 0
  // Let a0 = p1.x-p0.x, a1 = p2.x-p1.x, a2 = p3.x-p2.x
  //   a0 - 2*a0*t + a0*t^2 + 2*a1*t - 2*a1*t^2 + a2*t^2 = 0
  //   (a0 - 2*a1 + a2)*t^2 + (-2*a0 + 2*a1)*t + a0 = 0
  //   (a0 - 2*a1 + a2)*t^2 + 2*(a1 - a0)*t + a0 = 0

  const double a0 = p1.x - p0.x;
  const double a1 = p2.x - p1.x;
  const double a2 = p3.x - p2.x;

  const double a = a0 - 2.0 * a1 + a2;
  const double b = 2.0 * (a1 - a0);
  const double c = a0;

  if (std::abs(a) < 1e-12) {
    // Degenerate case: linear equation b*t + c = 0.
    if (std::abs(b) > 1e-12) {
      const double t = -c / b;
      if (t > 0.0 && t < 1.0) {
        result.push_back(t);
      }
    }
  } else {
    auto sol = SolveQuadratic(a, b, c);
    if (sol.hasSolution) {
      // Add solutions that are strictly in (0, 1), sorted.
      double t0 = sol.solution[0];
      double t1 = sol.solution[1];
      if (t0 > t1) {
        std::swap(t0, t1);
      }
      if (t0 > 0.0 && t0 < 1.0) {
        result.push_back(t0);
      }
      if (t1 > 0.0 && t1 < 1.0 && std::abs(t1 - t0) > 1e-12) {
        result.push_back(t1);
      }
    }
  }

  return result;
}

}  // namespace

Vector2d EvalQuadratic(const Vector2d& p0, const Vector2d& p1, const Vector2d& p2, double t) {
  const double s = 1.0 - t;
  return p0 * (s * s) + p1 * (2.0 * s * t) + p2 * (t * t);
}

Vector2d EvalCubic(const Vector2d& p0, const Vector2d& p1, const Vector2d& p2, const Vector2d& p3,
                   double t) {
  const double s = 1.0 - t;
  const double s2 = s * s;
  const double t2 = t * t;
  return p0 * (s2 * s) + p1 * (3.0 * s2 * t) + p2 * (3.0 * s * t2) + p3 * (t2 * t);
}

std::pair<std::array<Vector2d, 3>, std::array<Vector2d, 3>> SplitQuadratic(const Vector2d& p0,
                                                                           const Vector2d& p1,
                                                                           const Vector2d& p2,
                                                                           double t) {
  const Vector2d q0 = Lerp2d(p0, p1, t);
  const Vector2d q1 = Lerp2d(p1, p2, t);
  const Vector2d m = Lerp2d(q0, q1, t);

  return {std::array<Vector2d, 3>{p0, q0, m}, std::array<Vector2d, 3>{m, q1, p2}};
}

std::pair<std::array<Vector2d, 4>, std::array<Vector2d, 4>> SplitCubic(const Vector2d& p0,
                                                                       const Vector2d& p1,
                                                                       const Vector2d& p2,
                                                                       const Vector2d& p3,
                                                                       double t) {
  // Level 1: interpolate between adjacent control points.
  const Vector2d q0 = Lerp2d(p0, p1, t);
  const Vector2d q1 = Lerp2d(p1, p2, t);
  const Vector2d q2 = Lerp2d(p2, p3, t);

  // Level 2: interpolate between level-1 results.
  const Vector2d r0 = Lerp2d(q0, q1, t);
  const Vector2d r1 = Lerp2d(q1, q2, t);

  // Level 3: the split point.
  const Vector2d m = Lerp2d(r0, r1, t);

  return {std::array<Vector2d, 4>{p0, q0, r0, m}, std::array<Vector2d, 4>{m, r1, q2, p3}};
}

void ApproximateCubicWithQuadratics(const Vector2d& p0, const Vector2d& p1, const Vector2d& p2,
                                    const Vector2d& p3, double tolerance,
                                    std::vector<Vector2d>& out) {
  approximateCubicWithQuadraticsImpl(p0, p1, p2, p3, tolerance, out, 0);
}

SmallVector<double, 1> QuadraticYExtrema(const Vector2d& p0, const Vector2d& p1,
                                         const Vector2d& p2) {
  SmallVector<double, 1> result;

  // dy/dt = 2(1-t)(p1.y - p0.y) + 2t(p2.y - p1.y) = 0
  // Solving: t = (p0.y - p1.y) / (p0.y - 2*p1.y + p2.y)
  const double denom = p0.y - 2.0 * p1.y + p2.y;
  if (std::abs(denom) > 1e-12) {
    const double t = (p0.y - p1.y) / denom;
    if (t > 0.0 && t < 1.0) {
      result.push_back(t);
    }
  }

  return result;
}

SmallVector<double, 2> CubicYExtrema(const Vector2d& p0, const Vector2d& p1, const Vector2d& p2,
                                     const Vector2d& p3) {
  SmallVector<double, 2> result;

  // Same derivation as CubicXExtrema but for y component.
  const double a0 = p1.y - p0.y;
  const double a1 = p2.y - p1.y;
  const double a2 = p3.y - p2.y;

  const double a = a0 - 2.0 * a1 + a2;
  const double b = 2.0 * (a1 - a0);
  const double c = a0;

  if (std::abs(a) < 1e-12) {
    // Degenerate case: linear equation b*t + c = 0.
    if (std::abs(b) > 1e-12) {
      const double t = -c / b;
      if (t > 0.0 && t < 1.0) {
        result.push_back(t);
      }
    }
  } else {
    auto sol = SolveQuadratic(a, b, c);
    if (sol.hasSolution) {
      double t0 = sol.solution[0];
      double t1 = sol.solution[1];
      if (t0 > t1) {
        std::swap(t0, t1);
      }
      if (t0 > 0.0 && t0 < 1.0) {
        result.push_back(t0);
      }
      if (t1 > 0.0 && t1 < 1.0 && std::abs(t1 - t0) > 1e-12) {
        result.push_back(t1);
      }
    }
  }

  return result;
}

Box2d QuadraticBounds(const Vector2d& p0, const Vector2d& p1, const Vector2d& p2) {
  // Start with a box containing the endpoints.
  Box2d box = Box2d::CreateEmpty(p0);
  box.addPoint(p2);

  // Add X extrema.
  for (const double t : QuadraticXExtrema(p0, p1, p2)) {
    box.addPoint(EvalQuadratic(p0, p1, p2, t));
  }

  // Add Y extrema.
  for (const double t : QuadraticYExtrema(p0, p1, p2)) {
    box.addPoint(EvalQuadratic(p0, p1, p2, t));
  }

  return box;
}

Box2d CubicBounds(const Vector2d& p0, const Vector2d& p1, const Vector2d& p2, const Vector2d& p3) {
  // Start with a box containing the endpoints.
  Box2d box = Box2d::CreateEmpty(p0);
  box.addPoint(p3);

  // Add X extrema.
  for (const double t : CubicXExtrema(p0, p1, p2, p3)) {
    box.addPoint(EvalCubic(p0, p1, p2, p3, t));
  }

  // Add Y extrema.
  for (const double t : CubicYExtrema(p0, p1, p2, p3)) {
    box.addPoint(EvalCubic(p0, p1, p2, p3, t));
  }

  return box;
}

}  // namespace donner
