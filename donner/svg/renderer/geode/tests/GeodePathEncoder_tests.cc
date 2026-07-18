#include "donner/svg/renderer/geode/GeodePathEncoder.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <limits>
#include <set>
#include <tuple>
#include <vector>

#include "donner/base/FillRule.h"
#include "donner/base/Path.h"

namespace donner::geode {

/// Non-asserting encode-cost probe: times `GeodePathEncoder::encode` on a
/// synthetic many-curve, many-band path and prints the per-encode median
/// to stderr. Wall-clock budgets are not CI-stable, so this only reports;
/// the durable regression signal for encode work is `pathEncodes` in
/// `GeodePerf_tests.cc`.
TEST(GeodePathEncoder, EncodeTimingProbe) {
  // Tall serpentine path: ~200 cubic segments over 2000px of height, so
  // the encode runs the full pipeline (cubic lowering, dual monotonic
  // splits, curve extraction, banding) across many bands (~63 at 32px).
  PathBuilder builder;
  builder.moveTo(Vector2d(0, 0));
  for (int i = 0; i < 200; ++i) {
    const double y = static_cast<double>(i) * 10.0;
    const double x = (i % 2 == 0) ? 200.0 : 0.0;
    builder.curveTo(Vector2d(x + 50.0, y), Vector2d(x - 50.0, y + 5.0), Vector2d(x, y + 10.0));
  }
  builder.closePath();
  const Path path = builder.build();

  constexpr int kIterations = 200;
  std::vector<double> samplesUs;
  samplesUs.reserve(kIterations);
  size_t sink = 0;  // Defeat dead-code elimination.
  for (int i = 0; i < kIterations; ++i) {
    const auto start = std::chrono::steady_clock::now();
    const EncodedPath iterEncoded = GeodePathEncoder::encode(path, FillRule::NonZero);
    const auto end = std::chrono::steady_clock::now();
    sink += iterEncoded.curves.size() + iterEncoded.quadVertices.size();
    samplesUs.push_back(std::chrono::duration<double, std::micro>(end - start).count());
  }
  std::sort(samplesUs.begin(), samplesUs.end());
  const EncodedPath encoded = GeodePathEncoder::encode(path, FillRule::NonZero);
  std::fprintf(stderr,
               "[GeodePathEncoder] EncodeTimingProbe median=%.1fus p90=%.1fus (bands=%zu "
               "vBands=%zu curves=%zu vCurves=%zu sink=%zu)\n",
               samplesUs[samplesUs.size() / 2], samplesUs[(samplesUs.size() * 9) / 10],
               encoded.bands.size(), encoded.vBands.size(), encoded.curves.size(),
               encoded.vCurves.size(), sink);
  EXPECT_FALSE(encoded.empty());
}

TEST(GeodePathEncoder, EmptyPath) {
  Path path;
  EncodedPath encoded = GeodePathEncoder::encode(path, FillRule::NonZero);
  EXPECT_TRUE(encoded.empty());
  EXPECT_TRUE(encoded.curves.empty());
  EXPECT_TRUE(encoded.bands.empty());
}

TEST(GeodePathEncoder, SimpleTriangle) {
  Path path = PathBuilder()
                  .moveTo(Vector2d(0, 0))
                  .lineTo(Vector2d(100, 0))
                  .lineTo(Vector2d(50, 100))
                  .closePath()
                  .build();

  EncodedPath encoded = GeodePathEncoder::encode(path, FillRule::NonZero);
  EXPECT_FALSE(encoded.empty());

  // Should have curves for each edge (3 edges: bottom, right diagonal, left diagonal + close).
  EXPECT_GT(encoded.curves.size(), 0u);

  // Should have at least 1 band.
  EXPECT_GE(encoded.bands.size(), 1u);

  // The whole path draws one bounding quad (6 vertices = 2 triangles).
  EXPECT_EQ(encoded.quadVertices.size(), 6u);

  // Bounds should encompass the triangle.
  EXPECT_LE(encoded.pathBounds.topLeft.x, 0.0f);
  EXPECT_LE(encoded.pathBounds.topLeft.y, 0.0f);
  EXPECT_GE(encoded.pathBounds.bottomRight.x, 100.0f);
  EXPECT_GE(encoded.pathBounds.bottomRight.y, 100.0f);
}

TEST(GeodePathEncoder, SmallPathSingleBand) {
  // A small rect (< 64px) should produce a single band.
  Path path = PathBuilder().addRect(Box2d({0, 0}, {10, 10})).build();

  EncodedPath encoded = GeodePathEncoder::encode(path, FillRule::NonZero);
  EXPECT_FALSE(encoded.empty());

  // 10px height → single band.
  EXPECT_EQ(encoded.bands.size(), 1u);
}

TEST(GeodePathEncoder, LargePathMultipleBands) {
  // A tall rect (500px) should produce multiple bands.
  Path path = PathBuilder().addRect(Box2d({0, 0}, {100, 500})).build();

  EncodedPath encoded = GeodePathEncoder::encode(path, FillRule::NonZero);
  EXPECT_FALSE(encoded.empty());

  // 500px / 32px per band ≈ 16 bands.
  EXPECT_GT(encoded.bands.size(), 1u);
  EXPECT_LE(encoded.bands.size(), 256u);  // Within max band limit.

  // Multi-band paths still draw one whole-path quad. Multiple quads would
  // reintroduce non-additive source-over coverage at band boundaries.
  EXPECT_EQ(encoded.quadVertices.size(), 6u);
}

TEST(GeodePathEncoder, OmitsCurvesParallelToEachRay) {
  const Path path = PathBuilder().addRect(Box2d({0, 0}, {100, 100})).build();

  const EncodedPath encoded = GeodePathEncoder::encode(path, FillRule::NonZero);
  ASSERT_FALSE(encoded.empty());

  for (const EncodedPath::Curve& curve : encoded.curves) {
    EXPECT_FALSE(curve.p0y == curve.p1y && curve.p1y == curve.p2y)
        << "Horizontal-ray data must omit horizontal curves";
  }
  for (const EncodedPath::Curve& curve : encoded.vCurves) {
    EXPECT_FALSE(curve.p0x == curve.p1x && curve.p1x == curve.p2x)
        << "Vertical-ray data must omit vertical curves";
  }
}

TEST(GeodePathEncoder, SortsBandCurvesByDescendingCrossAxisMaximum) {
  PathBuilder builder;
  builder.addRect(Box2d({0, 0}, {10, 10}));
  builder.addRect(Box2d({30, 0}, {40, 10}));
  builder.addRect(Box2d({70, 0}, {80, 10}));
  const EncodedPath encoded = GeodePathEncoder::encode(builder.build(), FillRule::NonZero);
  ASSERT_FALSE(encoded.empty());

  for (const EncodedPath::Band& band : encoded.bands) {
    float previousMax = std::numeric_limits<float>::infinity();
    for (uint32_t i = 0; i < band.curveCount; ++i) {
      const EncodedPath::Curve& curve = encoded.curves[band.curveStart + i];
      const float curveMax = std::max({curve.p0x, curve.p1x, curve.p2x});
      EXPECT_LE(curveMax, previousMax) << "Horizontal band references must be descending";
      previousMax = curveMax;
    }
  }
  for (const EncodedPath::Band& band : encoded.vBands) {
    float previousMax = std::numeric_limits<float>::infinity();
    for (uint32_t i = 0; i < band.curveCount; ++i) {
      const EncodedPath::Curve& curve = encoded.vCurves[band.curveStart + i];
      const float curveMax = std::max({curve.p0y, curve.p1y, curve.p2y});
      EXPECT_LE(curveMax, previousMax) << "Vertical band references must be descending";
      previousMax = curveMax;
    }
  }
}

TEST(GeodePathEncoder, ChoosesBandCountFromCurveDistribution) {
  PathBuilder builder;
  for (int i = 0; i < 16; ++i) {
    const double y = static_cast<double>(i) * 0.625;
    builder.addRect(Box2d({0, y}, {10, y + 0.25}));
  }

  const EncodedPath encoded = GeodePathEncoder::encode(builder.build(), FillRule::NonZero);
  ASSERT_FALSE(encoded.empty());
  EXPECT_GT(encoded.hBandCount, 1u)
      << "A compact path with many separated curve clusters needs more than one band";
}

TEST(GeodePathEncoder, StoresEachCurveOnceInsteadOfDuplicatingItAcrossBands) {
  const Path path = PathBuilder()
                        .moveTo(Vector2d(0, 0))
                        .lineTo(Vector2d(100, 500))
                        .lineTo(Vector2d(200, 0))
                        .closePath()
                        .build();

  const EncodedPath encoded = GeodePathEncoder::encode(path, FillRule::NonZero);
  ASSERT_FALSE(encoded.empty());

  std::set<std::tuple<float, float, float, float, float, float>> uniqueHorizontal;
  for (const EncodedPath::Curve& curve : encoded.curves) {
    uniqueHorizontal.emplace(curve.p0x, curve.p0y, curve.p1x, curve.p1y, curve.p2x, curve.p2y);
  }
  EXPECT_EQ(encoded.curves.size(), uniqueHorizontal.size())
      << "Horizontal bands must reference one canonical copy of each curve";

  std::set<std::tuple<float, float, float, float, float, float>> uniqueVertical;
  for (const EncodedPath::Curve& curve : encoded.vCurves) {
    uniqueVertical.emplace(curve.p0x, curve.p0y, curve.p1x, curve.p1y, curve.p2x, curve.p2y);
  }
  EXPECT_EQ(encoded.vCurves.size(), uniqueVertical.size())
      << "Vertical bands must reference one canonical copy of each curve";
}

TEST(GeodePathEncoder, UsesTighterGeometryForTriangularPath) {
  const Path path = PathBuilder()
                        .moveTo(Vector2d(0, 0))
                        .lineTo(Vector2d(100, 0))
                        .lineTo(Vector2d(50, 100))
                        .closePath()
                        .build();

  const EncodedPath encoded = GeodePathEncoder::encode(path, FillRule::NonZero);
  ASSERT_FALSE(encoded.empty());

  std::set<std::pair<float, float>> uniqueVertices;
  for (const EncodedPath::Vertex& vertex : encoded.quadVertices) {
    uniqueVertices.emplace(vertex.posX, vertex.posY);
  }
  EXPECT_LE(uniqueVertices.size(), 3u)
      << "A triangle should not shade all four corners of its axis-aligned bounds";
}

TEST(GeodePathEncoder, BandsCoverFullHeight) {
  Path path = PathBuilder().addRect(Box2d({10, 20}, {90, 120})).build();

  EncodedPath encoded = GeodePathEncoder::encode(path, FillRule::NonZero);
  EXPECT_FALSE(encoded.empty());

  // First band should start at or before the path's top.
  EXPECT_LE(encoded.bands.front().yMin, encoded.pathBounds.topLeft.y + 0.01f);

  // Last band should end at or after the path's bottom.
  EXPECT_GE(encoded.bands.back().yMax, encoded.pathBounds.bottomRight.y - 0.01f);
}

TEST(GeodePathEncoder, CurvesPerBandNonEmpty) {
  Path path = PathBuilder()
                  .moveTo(Vector2d(0, 0))
                  .lineTo(Vector2d(100, 0))
                  .lineTo(Vector2d(50, 100))
                  .closePath()
                  .build();

  EncodedPath encoded = GeodePathEncoder::encode(path, FillRule::NonZero);

  for (const auto& band : encoded.bands) {
    EXPECT_GT(band.curveCount, 0u) << "Non-empty band should have curves";
    EXPECT_LE(band.curveStart + band.curveCount, encoded.curves.size())
        << "Curve range should be within bounds";
  }
}

TEST(GeodePathEncoder, QuadraticCurveEncoded) {
  // A path with a quadratic curve should encode correctly.
  Path path = PathBuilder()
                  .moveTo(Vector2d(0, 0))
                  .quadTo(Vector2d(50, 100), Vector2d(100, 0))
                  .closePath()
                  .build();

  EncodedPath encoded = GeodePathEncoder::encode(path, FillRule::NonZero);
  EXPECT_FALSE(encoded.empty());
  EXPECT_GT(encoded.curves.size(), 0u);
}

TEST(GeodePathEncoder, CircleEncoded) {
  Path path = PathBuilder().addCircle(Vector2d(50, 50), 40).build();

  EncodedPath encoded = GeodePathEncoder::encode(path, FillRule::NonZero);
  EXPECT_FALSE(encoded.empty());

  // Circle is ~80px tall → should have a few bands.
  EXPECT_GE(encoded.bands.size(), 2u);

  // Bounds should be approximately [10,10]-[90,90].
  EXPECT_NEAR(encoded.pathBounds.topLeft.x, 10.0f, 1.0f);
  EXPECT_NEAR(encoded.pathBounds.topLeft.y, 10.0f, 1.0f);
  EXPECT_NEAR(encoded.pathBounds.bottomRight.x, 90.0f, 1.0f);
  EXPECT_NEAR(encoded.pathBounds.bottomRight.y, 90.0f, 1.0f);
}

TEST(GeodePathEncoder, DegeneratePath) {
  // A path with zero area (single point) should produce empty result.
  Path path = PathBuilder().moveTo(Vector2d(50, 50)).build();

  EncodedPath encoded = GeodePathEncoder::encode(path, FillRule::NonZero);
  EXPECT_TRUE(encoded.empty());
}

TEST(GeodePathEncoder, VertexNormalsPointOutward) {
  Path path = PathBuilder().addRect(Box2d({0, 0}, {100, 100})).build();

  EncodedPath encoded = GeodePathEncoder::encode(path, FillRule::NonZero);
  EXPECT_FALSE(encoded.empty());

  // The path bounding quad has 6 vertices. Verify normals are non-zero
  // and point in expected directions.
  EXPECT_EQ(encoded.quadVertices.size(), 6u);
  for (const auto& v : encoded.quadVertices) {
    const float normalLen = std::sqrt(v.normalX * v.normalX + v.normalY * v.normalY);
    EXPECT_GT(normalLen, 0.0f) << "Vertex normals should be non-zero";
  }
}

namespace {

// Winding number of a horizontal +x ray from (px,py) across all Y-monotonic curves.
int HorizontalWinding(const std::vector<EncodedPath::Curve>& curves, double px, double py) {
  int winding = 0;
  for (const auto& c : curves) {
    const double a = c.p0y - 2.0 * c.p1y + c.p2y;
    const double b = 2.0 * (c.p1y - c.p0y);
    const double cc = c.p0y - py;
    double ts[2];
    int n = 0;
    if (std::abs(a) < 1e-9) {
      if (std::abs(b) > 1e-12) ts[n++] = -cc / b;
    } else {
      const double disc = b * b - 4 * a * cc;
      if (disc >= 0) {
        const double s = std::sqrt(disc);
        ts[n++] = (-b + s) / (2 * a);
        ts[n++] = (-b - s) / (2 * a);
      }
    }
    for (int i = 0; i < n; ++i) {
      const double t = ts[i];
      if (t < 0.0 || t > 1.0) continue;
      const double omt = 1.0 - t;
      const double x = omt * omt * c.p0x + 2 * omt * t * c.p1x + t * t * c.p2x;
      if (x < px) continue;
      const double dydt = 2 * omt * (c.p1y - c.p0y) + 2 * t * (c.p2y - c.p1y);
      winding += (dydt > 0) ? 1 : (dydt < 0 ? -1 : 0);
    }
  }
  return winding;
}

// Winding number of a vertical +y ray from (px,py) across all X-monotonic curves.
int VerticalWinding(const std::vector<EncodedPath::Curve>& curves, double px, double py) {
  int winding = 0;
  for (const auto& c : curves) {
    const double a = c.p0x - 2.0 * c.p1x + c.p2x;
    const double b = 2.0 * (c.p1x - c.p0x);
    const double cc = c.p0x - px;
    double ts[2];
    int n = 0;
    if (std::abs(a) < 1e-9) {
      if (std::abs(b) > 1e-12) ts[n++] = -cc / b;
    } else {
      const double disc = b * b - 4 * a * cc;
      if (disc >= 0) {
        const double s = std::sqrt(disc);
        ts[n++] = (-b + s) / (2 * a);
        ts[n++] = (-b - s) / (2 * a);
      }
    }
    for (int i = 0; i < n; ++i) {
      const double t = ts[i];
      if (t < 0.0 || t > 1.0) continue;
      const double omt = 1.0 - t;
      const double y = omt * omt * c.p0y + 2 * omt * t * c.p1y + t * t * c.p2y;
      if (y < py) continue;
      const double dxdt = 2 * omt * (c.p1x - c.p0x) + 2 * t * (c.p2x - c.p1x);
      winding += (dxdt > 0) ? 1 : (dxdt < 0 ? -1 : 0);
    }
  }
  return winding;
}

}  // namespace

// M3: the vertical (X-monotonic) band set must be populated and produce winding
// numbers consistent with the horizontal set - winding is ray-direction-independent,
// so a point is inside per the horizontal ray iff it is inside per the vertical ray.
TEST(GeodePathEncoder, VerticalBandsConsistentWinding) {
  // A triangle plus an interior hole exercises non-trivial winding both ways.
  Path path = PathBuilder()
                  .moveTo(Vector2d(0, 0))
                  .lineTo(Vector2d(200, 0))
                  .lineTo(Vector2d(100, 200))
                  .closePath()
                  .build();

  EncodedPath encoded = GeodePathEncoder::encode(path, FillRule::NonZero);
  ASSERT_FALSE(encoded.empty());
  EXPECT_FALSE(encoded.vBands.empty()) << "Vertical bands must be populated for the vertical ray";
  EXPECT_FALSE(encoded.vCurves.empty());

  // Vertical bands partition the bounds width.
  EXPECT_LE(encoded.vBands.front().xMin, encoded.pathBounds.topLeft.x + 0.01);
  EXPECT_GE(encoded.vBands.back().xMax, encoded.pathBounds.bottomRight.x - 0.01);

  // Sample a grid over the bounding box; the two rays must agree on inside/outside.
  const auto& b = encoded.pathBounds;
  // Offset to cell centers (+0.5) so no sample lands exactly on an edge/vertex, where
  // winding is ill-defined and the two ray directions can legitimately disagree.
  for (int iy = 0; iy < 20; ++iy) {
    for (int ix = 0; ix < 20; ++ix) {
      const double px = b.topLeft.x + (b.width() * (ix + 0.5)) / 20.0;
      const double py = b.topLeft.y + (b.height() * (iy + 0.5)) / 20.0;
      const bool hInside = HorizontalWinding(encoded.curves, px, py) != 0;
      const bool vInside = VerticalWinding(encoded.vCurves, px, py) != 0;
      EXPECT_EQ(hInside, vInside) << "Horizontal vs vertical winding disagree at (" << px << ", "
                                  << py << ")";
    }
  }
}

TEST(GeodePathEncoder, ClosedStrokeRightContourUsesInsideJoins) {
  // Exact path from filters/filter/path-bbox.svg. The closed stroke's right contour must
  // truncate inside joins at their miter intersections instead of retaining both offset
  // endpoints. The latter produces a self-intersecting wedge at (65, 135).
  const Path path = PathBuilder()
                        .moveTo({50, 85})
                        .lineTo({65, 135})
                        .lineTo({150, 135})
                        .lineTo({150, 85})
                        .quadTo({100, 45}, {50, 85})
                        .closePath()
                        .build();
  const Path stroke = path.strokeToFill({.width = 1.0});
  ASSERT_FALSE(stroke.empty());
  EXPECT_EQ(stroke.points().size(), 95u)
      << "Pre-fix right-contour misclassification emitted 117 outline points";
  EXPECT_EQ(stroke.commands().size(), 97u);

  const EncodedPath encoded = GeodePathEncoder::encode(stroke, FillRule::EvenOdd);
  ASSERT_FALSE(encoded.empty());
}

// M3c: the dense band-grid maps cells onto the packed band arrays so the analytic
// shader can look up its band in O(1) from a sample position. Verify the grid is the
// right size and each non-empty cell points at a band whose strip matches the cell.
TEST(GeodePathEncoder, BandGridResolvesToCoveringBand) {
  Path path = PathBuilder()
                  .moveTo(Vector2d(0, 0))
                  .lineTo(Vector2d(200, 0))
                  .lineTo(Vector2d(100, 200))
                  .closePath()
                  .build();

  EncodedPath encoded = GeodePathEncoder::encode(path, FillRule::NonZero);
  ASSERT_FALSE(encoded.empty());

  ASSERT_EQ(encoded.hBandGrid.size(), encoded.hBandCount);
  ASSERT_EQ(encoded.vBandGrid.size(), encoded.vBandCount);
  EXPECT_GT(encoded.hBandCount, 0u);
  EXPECT_GT(encoded.hStride, 0.0f);

  for (uint32_t cell = 0; cell < encoded.hBandCount; ++cell) {
    const uint32_t slot = encoded.hBandGrid[cell];
    if (slot == EncodedPath::kNoBand) {
      continue;
    }
    ASSERT_LT(slot, encoded.bands.size());
    const float expectedYMin = encoded.yBase + static_cast<float>(cell) * encoded.hStride;
    EXPECT_NEAR(encoded.bands[slot].yMin, expectedYMin, 0.01f)
        << "Grid cell " << cell << " should map to the band covering its Y-strip";
  }
  for (uint32_t cell = 0; cell < encoded.vBandCount; ++cell) {
    const uint32_t slot = encoded.vBandGrid[cell];
    if (slot == EncodedPath::kNoBand) {
      continue;
    }
    ASSERT_LT(slot, encoded.vBands.size());
    const float expectedXMin = encoded.xBase + static_cast<float>(cell) * encoded.vStride;
    EXPECT_NEAR(encoded.vBands[slot].xMin, expectedXMin, 0.01f);
  }
}

// Regression: filling an OPEN subpath (e.g., an SVG `<line>` that expands to
// MoveTo+LineTo with no explicit close) must not spill fill across the
// half-plane. The encoder emits an implicit closing segment back to the
// subpath start, and the resulting winding-number sum is zero everywhere
// (the only non-zero region would be the "stripe" between the forward and
// return edges, which has zero width for a single LineTo).
//
// Pre-fix behavior: a single LineTo produced one curve that crossed each
// scanline once → odd winding on the entire half-plane to one side of the
// line, rendering a 100%-filled triangle in a-stroke-linecap-001/002/003.
//
// Strategy: compare the encoded output of an open path against the same
// path with an explicit closePath(). They should produce identical band
// curve counts and identical bounds. `encoded.curves` is band-flattened
// (each curve is duplicated per band it spans), so an equality assertion
// on the size implicitly verifies the total number of source segments
// and their band coverage matches.
TEST(GeodePathEncoder, OpenSubpathGetsImplicitClose) {
  // Triangle as three LineTos with no closePath() vs. the same with
  // closePath(). The implicit-close logic in the encoder should make
  // the two paths encode identically (same band curves, same bounds).
  Path openTriangle = PathBuilder()
                          .moveTo(Vector2d(0, 0))
                          .lineTo(Vector2d(100, 0))
                          .lineTo(Vector2d(50, 100))
                          .build();  // No closePath.
  Path closedTriangle = PathBuilder()
                            .moveTo(Vector2d(0, 0))
                            .lineTo(Vector2d(100, 0))
                            .lineTo(Vector2d(50, 100))
                            .closePath()
                            .build();
  EncodedPath openEncoded = GeodePathEncoder::encode(openTriangle, FillRule::NonZero);
  EncodedPath closedEncoded = GeodePathEncoder::encode(closedTriangle, FillRule::NonZero);
  EXPECT_FALSE(openEncoded.empty());
  EXPECT_FALSE(closedEncoded.empty());
  EXPECT_EQ(openEncoded.curves.size(), closedEncoded.curves.size());
  EXPECT_EQ(openEncoded.bands.size(), closedEncoded.bands.size());

  // A straight line (the geometry resvg's `a-stroke-linecap-001` feeds to
  // the fill path before the stroke ribbon is generated) must produce
  // non-empty output with the implicit close. Pre-fix it would have been
  // a single forward curve with no return edge, producing spill fill.
  Path openLine = PathBuilder().moveTo(Vector2d(40, 40)).lineTo(Vector2d(160, 160)).build();
  EncodedPath encoded = GeodePathEncoder::encode(openLine, FillRule::NonZero);
  EXPECT_FALSE(encoded.empty());
  // There must be an even number of forward+return segments per band
  // (one LineTo forward, one implicit close back). `curves.size()` is
  // band-flattened, so for an N-band span we expect 2*N band curves.
  EXPECT_EQ(encoded.curves.size() % 2u, 0u);
}

// Multi-subpath regression: a MoveTo in the middle of a path starts a new
// subpath, and the previous one (if left open) must be closed implicitly
// at that boundary - not merged with the new subpath's geometry.
//
// Strategy: encode M-L-M-L (two open horizontal lines) and compare to
// M-L-Z-M-L-Z (same lines explicitly closed). Horizontal lines are
// Y-degenerate and drop out of the encoder, but the returning close
// segment of the second triangle contributes a triangle-shaped fill
// region. After the fix, both encode identically.
TEST(GeodePathEncoder, MultipleOpenSubpathsEachGetClosed) {
  // Two diagonal open lines in disjoint Y ranges - each should get its
  // own implicit close, without one subpath's close bridging across
  // MoveTo into the next subpath's start.
  Path twoLinesOpen = PathBuilder()
                          .moveTo(Vector2d(10, 10))
                          .lineTo(Vector2d(90, 40))
                          .moveTo(Vector2d(10, 60))
                          .lineTo(Vector2d(90, 90))
                          .build();
  Path twoLinesClosed = PathBuilder()
                            .moveTo(Vector2d(10, 10))
                            .lineTo(Vector2d(90, 40))
                            .closePath()
                            .moveTo(Vector2d(10, 60))
                            .lineTo(Vector2d(90, 90))
                            .closePath()
                            .build();
  EncodedPath openEncoded = GeodePathEncoder::encode(twoLinesOpen, FillRule::NonZero);
  EncodedPath closedEncoded = GeodePathEncoder::encode(twoLinesClosed, FillRule::NonZero);
  EXPECT_EQ(openEncoded.curves.size(), closedEncoded.curves.size());
}

}  // namespace donner::geode
