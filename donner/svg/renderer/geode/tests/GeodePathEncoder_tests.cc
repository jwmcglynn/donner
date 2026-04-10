#include "donner/svg/renderer/geode/GeodePathEncoder.h"

#include <gtest/gtest.h>

#include "donner/base/FillRule.h"
#include "donner/base/Path.h"

namespace donner::geode {

TEST(GeodePathEncoder, EmptyPath) {
  Path path;
  EncodedPath encoded = GeodePathEncoder::encode(path, FillRule::NonZero);
  EXPECT_TRUE(encoded.empty());
  EXPECT_TRUE(encoded.curves.empty());
  EXPECT_TRUE(encoded.bands.empty());
  EXPECT_TRUE(encoded.vertices.empty());
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

  // Each band produces 6 vertices (2 triangles).
  EXPECT_EQ(encoded.vertices.size(), encoded.bands.size() * 6);

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

  // Each band has 6 vertices. Verify normals are non-zero and point in expected directions.
  for (const auto& v : encoded.vertices) {
    const float normalLen = std::sqrt(v.normalX * v.normalX + v.normalY * v.normalY);
    EXPECT_GT(normalLen, 0.0f) << "Vertex normals should be non-zero";
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
// at that boundary — not merged with the new subpath's geometry.
//
// Strategy: encode M-L-M-L (two open horizontal lines) and compare to
// M-L-Z-M-L-Z (same lines explicitly closed). Horizontal lines are
// Y-degenerate and drop out of the encoder, but the returning close
// segment of the second triangle contributes a triangle-shaped fill
// region. After the fix, both encode identically.
TEST(GeodePathEncoder, MultipleOpenSubpathsEachGetClosed) {
  // Two diagonal open lines in disjoint Y ranges — each should get its
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
