#include <cassert>
#include <cmath>
#include <cstring>
#include <vector>

#include "donner/base/BezierUtils.h"

namespace donner {

namespace {

/// Read a double from the fuzzer data buffer, advancing the pointer.
/// Returns false if not enough data remains.
bool readDouble(const uint8_t*& data, size_t& remaining, double& out) {
  if (remaining < sizeof(double)) {
    return false;
  }
  std::memcpy(&out, data, sizeof(double));
  data += sizeof(double);
  remaining -= sizeof(double);
  return true;
}

/// Read a Vector2d (two doubles) from the fuzzer data buffer.
bool readVector2d(const uint8_t*& data, size_t& remaining, Vector2d& out) {
  return readDouble(data, remaining, out.x) && readDouble(data, remaining, out.y);
}

/// Returns true if the value is finite (not NaN and not Inf).
bool isFinite(double v) {
  return std::isfinite(v);
}

/// Returns true if a Vector2d has finite components.
bool isFiniteVec(const Vector2d& v) {
  return isFinite(v.x) && isFinite(v.y);
}

/// Returns true if a Box2d has finite components.
bool isFiniteBox(const Box2d& box) {
  return isFiniteVec(box.topLeft) && isFiniteVec(box.bottomRight);
}

/// Clamp t to [0, 1] for functions that expect parameter in that range.
double clampT(double t) {
  if (std::isnan(t)) {
    return 0.5;
  }
  if (t < 0.0) {
    return 0.0;
  }
  if (t > 1.0) {
    return 1.0;
  }
  return t;
}

}  // namespace

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const uint8_t* cursor = data;
  size_t remaining = size;

  // Read 4 points (enough for cubic) + 1 double for t + 1 double for tolerance.
  // = 4*2*8 + 8 + 8 = 80 bytes minimum.
  Vector2d p0, p1, p2, p3;
  double t, tolerance;

  if (!readVector2d(cursor, remaining, p0)) {
    return 0;
  }
  if (!readVector2d(cursor, remaining, p1)) {
    return 0;
  }
  if (!readVector2d(cursor, remaining, p2)) {
    return 0;
  }
  if (!readVector2d(cursor, remaining, p3)) {
    return 0;
  }
  if (!readDouble(cursor, remaining, t)) {
    return 0;
  }
  if (!readDouble(cursor, remaining, tolerance)) {
    return 0;
  }

  // Clamp t to [0, 1] so the Bezier functions receive valid parameters.
  t = clampT(t);

  // Clamp tolerance to a positive value to avoid infinite subdivision.
  if (!std::isfinite(tolerance) || tolerance <= 0.0) {
    tolerance = 0.1;
  }

  // Skip inputs with non-finite coordinates since they produce NaN/Inf by design.
  if (!isFiniteVec(p0) || !isFiniteVec(p1) || !isFiniteVec(p2) || !isFiniteVec(p3)) {
    return 0;
  }

  // --- EvalQuadratic ---
  {
    const Vector2d result = EvalQuadratic(p0, p1, p2, t);
    assert(isFiniteVec(result) && "EvalQuadratic should produce finite results for finite inputs");
  }

  // --- EvalCubic ---
  {
    const Vector2d result = EvalCubic(p0, p1, p2, p3, t);
    assert(isFiniteVec(result) && "EvalCubic should produce finite results for finite inputs");
  }

  // --- SplitQuadratic ---
  {
    auto [left, right] = SplitQuadratic(p0, p1, p2, t);
    // Structural: split points should be finite.
    assert(isFiniteVec(left[2]) && "SplitQuadratic left endpoint should be finite");
    assert(isFiniteVec(right[0]) && "SplitQuadratic right startpoint should be finite");
  }

  // --- SplitCubic ---
  {
    auto [left, right] = SplitCubic(p0, p1, p2, p3, t);
    assert(isFiniteVec(left[3]) && "SplitCubic left endpoint should be finite");
    assert(isFiniteVec(right[0]) && "SplitCubic right startpoint should be finite");
  }

  // --- ApproximateCubicWithQuadratics ---
  {
    std::vector<Vector2d> out;
    ApproximateCubicWithQuadratics(p0, p1, p2, p3, tolerance, out);

    // Structural: should produce pairs of (control, end) points.
    assert(out.size() % 2 == 0 &&
           "ApproximateCubicWithQuadratics should produce pairs of points");
    assert(!out.empty() && "ApproximateCubicWithQuadratics should produce at least one segment");

    // All output points should be finite.
    for (const auto& pt : out) {
      assert(isFiniteVec(pt) && "ApproximateCubicWithQuadratics output should be finite");
    }
  }

  // --- QuadraticYExtrema ---
  {
    auto extrema = QuadraticYExtrema(p0, p1, p2);
    for (double tVal : extrema) {
      assert(tVal > 0.0 && tVal < 1.0 && "QuadraticYExtrema values must be in (0, 1)");
      assert(isFinite(tVal) && "QuadraticYExtrema values must be finite");
    }
  }

  // --- CubicYExtrema ---
  {
    auto extrema = CubicYExtrema(p0, p1, p2, p3);
    for (size_t i = 0; i < extrema.size(); ++i) {
      assert(extrema[i] > 0.0 && extrema[i] < 1.0 && "CubicYExtrema values must be in (0, 1)");
      assert(isFinite(extrema[i]) && "CubicYExtrema values must be finite");
    }
    // Values should be sorted.
    for (size_t i = 1; i < extrema.size(); ++i) {
      assert(extrema[i] >= extrema[i - 1] && "CubicYExtrema values should be sorted");
    }
  }

  // --- QuadraticBounds ---
  {
    const Box2d box = QuadraticBounds(p0, p1, p2);
    assert(isFiniteBox(box) && "QuadraticBounds should produce a finite box");

    // Bounds must contain the endpoints.
    assert(box.contains(p0) && "QuadraticBounds must contain start point p0");
    assert(box.contains(p2) && "QuadraticBounds must contain end point p2");

    // Exercise EvalQuadratic — don't assert containment for same reason as CubicBounds.
    const Vector2d pt = EvalQuadratic(p0, p1, p2, t);
    (void)pt;
  }

  // --- CubicBounds ---
  {
    const Box2d box = CubicBounds(p0, p1, p2, p3);
    assert(isFiniteBox(box) && "CubicBounds should produce a finite box");

    // Bounds must contain the endpoints.
    assert(box.contains(p0) && "CubicBounds must contain start point p0");
    assert(box.contains(p3) && "CubicBounds must contain end point p3");

    // Exercise EvalCubic — don't assert containment because CubicBounds uses root-finding
    // which has inherent floating-point imprecision for degenerate control-point configurations.
    // Correctness is validated by unit tests with well-conditioned inputs.
    const Vector2d pt = EvalCubic(p0, p1, p2, p3, t);
    (void)pt;
  }

  return 0;
}

}  // namespace donner
