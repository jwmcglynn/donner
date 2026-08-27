#pragma once
/// @file
/// The frozen-baseline scene shared by the baseline capture tool and the per-backend vertical
/// slice tests.
///
/// All consumers must render IDENTICAL inputs: the capture tool renders this scene through the
/// current production renderer as a black box and commits the PNG; the Metal slice renders the
/// same scene through donner::gpu + the MSL emitter and compares pixels against that PNG; the
/// Vulkan slice renders it through donner::gpu + the SPIR-V emitter and compares against an
/// in-process production render of the same scene. Only Donner-owned, deterministic content
/// appears here.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/FillRule.h"
#include "donner/base/Path.h"
#include "donner/base/Transform.h"
#include "donner/css/Color.h"
#include "donner/svg/renderer/geode/GeodePathEncoder.h"

namespace donner::gpu::tests {

/// Baseline render target size in pixels (square RGBA8, transparent background).
inline constexpr uint32_t kBaselineSize = 256;

/// One filled path of the baseline scene.
struct BaselinePathSpec {
  Path path;        //!< Path geometry in scene space.
  css::RGBA color;  //!< Solid fill color (not premultiplied).
  FillRule rule;    //!< Fill rule.
};

/// View transform applied to every path: a translate plus a non-unit scale so coverage math is
/// exercised away from the identity.
inline Transform2d BaselinePixelFromScene() {
  return Transform2d::Scale(0.94) * Transform2d::Translate(8.0, 8.0);
}

/// (a) A circle approximated with eight quadratic segments; red at 50 percent opacity,
/// non-zero fill.
///
/// The control and end points were transcribed once from the parametric form (center (70, 78),
/// radius 46, controls at radius / cos(pi/8) so each quadratic's midpoint touches the circle) and
/// are literals here on purpose. Evaluating them with `std::cos`/`std::sin` at runtime would make
/// the scene's geometry depend on the host math library, whose last bit is not specified, and the
/// frozen counters derived from this path are compared for exact equality across platforms.
inline BaselinePathSpec BaselineCircle() {
  /// Control point followed by end point for one of the eight quadratic segments.
  struct QuadSegment {
    Vector2d control;
    Vector2d end;
  };
  static constexpr std::array<QuadSegment, 8> kSegments = {{
      {{116.0, 97.05382386916237}, {102.5269119345812, 110.5269119345812}},
      {{89.05382386916237, 124.0}, {70.0, 124.0}},
      {{50.94617613083763, 124.0}, {37.473088065418814, 110.5269119345812}},
      {{24.0, 97.05382386916239}, {24.0, 78.0}},
      {{23.999999999999993, 58.94617613083763}, {37.47308806541881, 45.473088065418814}},
      {{50.9461761308376, 32.000000000000014}, {69.99999999999999, 32.0}},
      {{89.05382386916239, 32.00000000000001}, {102.52691193458118, 45.47308806541881}},
      {{115.99999999999999, 58.9461761308376}, {116.0, 77.99999999999999}},
  }};

  PathBuilder builder;
  builder.moveTo(Vector2d(116.0, 78.0));
  for (const QuadSegment& segment : kSegments) {
    builder.quadTo(segment.control, segment.end);
  }
  builder.closePath();
  return BaselinePathSpec{builder.build(), css::RGBA(255, 0, 0, 128), FillRule::NonZero};
}

/// (b) A self-intersecting five-point star; semi-transparent blue, even-odd fill (exercises the
/// even-odd triangle-wave fold on the doubled-coverage core).
///
/// The vertices were transcribed once from the parametric form (center (170, 90), radius 62,
/// angles stepping by 144 degrees from straight up, so every second vertex of a pentagon is
/// connected) and are literals here for the same reason the circle's are.
inline BaselinePathSpec BaselineStar() {
  static constexpr std::array<Vector2d, 5> kVertices = {{
      {170.0, 28.0},
      {206.44268564213334, 140.15905365124675},
      {111.03449598970047, 70.84094634875326},
      {228.96550401029953, 70.84094634875325},
      {133.5573143578667, 140.15905365124675},
  }};

  PathBuilder builder;
  builder.moveTo(kVertices[0]);
  for (size_t i = 1; i < kVertices.size(); ++i) {
    builder.lineTo(kVertices[i]);
  }
  builder.closePath();
  return BaselinePathSpec{builder.build(), css::RGBA(40, 80, 255, 150), FillRule::EvenOdd};
}

/// (c) A curvy cubic blob overlapping both other shapes; opaque green, non-zero fill
/// (exercises premultiplied source-over blending on top of (a) and (b)).
inline BaselinePathSpec BaselineBlob() {
  PathBuilder builder;
  builder.moveTo(Vector2d(60.0, 150.0));
  builder.curveTo(Vector2d(40.0, 110.0), Vector2d(120.0, 96.0), Vector2d(150.0, 128.0));
  builder.curveTo(Vector2d(196.0, 112.0), Vector2d(224.0, 150.0), Vector2d(198.0, 182.0));
  builder.curveTo(Vector2d(212.0, 222.0), Vector2d(140.0, 236.0), Vector2d(116.0, 210.0));
  builder.curveTo(Vector2d(76.0, 226.0), Vector2d(48.0, 190.0), Vector2d(60.0, 150.0));
  builder.closePath();
  return BaselinePathSpec{builder.build(), css::RGBA(24, 160, 60, 255), FillRule::NonZero};
}

/// The full scene, in draw order.
inline std::vector<BaselinePathSpec> BaselineScenePaths() {
  std::vector<BaselinePathSpec> paths;
  paths.push_back(BaselineCircle());
  paths.push_back(BaselineStar());
  paths.push_back(BaselineBlob());
  return paths;
}

// ----- Shader IR interop -----
// The generic solid-fill shader IR both vertical slices compile takes contiguous per-band
// curves and an explicit vertex buffer, while the production encoder emits compact curve
// references and expands its bounding fan in the vertex shader. These adapt one to the
// other, and live here so both slices consume identical geometry.

using donner::geode::EncodedPath;

/// Legacy band layout consumed by BuildSolidFillModule. The generic shader IR intentionally
/// remains on contiguous per-band curves while Geode's production shaders use compact references.
struct LegacyBand {
  uint32_t curveStart;
  uint32_t curveCount;
  float yMin = 0.0f;
  float yMax = 0.0f;
  float xMin = 0.0f;
  float xMax = 0.0f;
  float pad0 = 0.0f;
  float pad1 = 0.0f;
};
static_assert(sizeof(LegacyBand) == 32, "LegacyBand must match the shader IR layout");

struct LegacyAxis {
  std::vector<LegacyBand> bands;
  std::vector<EncodedPath::Curve> curves;
};

/// Vertex layout consumed by BuildSolidFillModule's current vertex-buffer interface.
struct LegacyVertex {
  float posX;
  float posY;
  float normalX;
  float normalY;
  uint32_t bandIndex = 0;
};
static_assert(sizeof(LegacyVertex) == 20, "LegacyVertex must match the shader IR layout");

/// Expresses the encoded path's conservative AABB through the generic shader IR's legacy quad.
inline std::array<LegacyVertex, 6> BuildLegacyQuad(const Box2d& bounds) {
  const auto xMin = static_cast<float>(bounds.topLeft.x);
  const auto yMin = static_cast<float>(bounds.topLeft.y);
  const auto xMax = static_cast<float>(bounds.bottomRight.x);
  const auto yMax = static_cast<float>(bounds.bottomRight.y);
  return {{{xMin, yMin, -1.0f, -1.0f},
           {xMax, yMin, 1.0f, -1.0f},
           {xMax, yMax, 1.0f, 1.0f},
           {xMin, yMin, -1.0f, -1.0f},
           {xMax, yMax, 1.0f, 1.0f},
           {xMin, yMax, -1.0f, 1.0f}}};
}

/// Expands compact per-band curve references into the contiguous layout consumed by the current
/// generic solid-fill shader IR. Returns false for a malformed reference range or index.
inline bool ExpandLegacyAxis(std::span<const EncodedPath::Band> bands,
                             std::span<const uint32_t> curveIndices,
                             std::span<const EncodedPath::Curve> canonicalCurves,
                             LegacyAxis& result) {
  for (const EncodedPath::Band& band : bands) {
    if (band.curveStart > curveIndices.size() ||
        band.curveCount > curveIndices.size() - band.curveStart) {
      return false;
    }

    LegacyBand legacyBand{static_cast<uint32_t>(result.curves.size()), band.curveCount};
    for (uint32_t i = 0; i < band.curveCount; ++i) {
      const uint32_t curveIndex = curveIndices[band.curveStart + i];
      if (curveIndex >= canonicalCurves.size()) {
        return false;
      }
      result.curves.push_back(canonicalCurves[curveIndex]);
    }
    result.bands.push_back(legacyBand);
  }
  return true;
}

}  // namespace donner::gpu::tests
