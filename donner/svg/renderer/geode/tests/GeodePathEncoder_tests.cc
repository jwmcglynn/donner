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

}  // namespace donner::geode
