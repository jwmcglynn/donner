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
#include <cmath>
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
inline BaselinePathSpec BaselineCircle() {
  const Vector2d center(70.0, 78.0);
  const double radius = 46.0;
  // Control points sit at radius / cos(pi/8) so each quadratic's midpoint touches the circle.
  const double controlRadius = radius / std::cos(3.14159265358979323846 / 8.0);

  PathBuilder builder;
  const auto pointAt = [&](double angleRadians, double r) {
    return center + Vector2d(r * std::cos(angleRadians), r * std::sin(angleRadians));
  };
  builder.moveTo(pointAt(0.0, radius));
  for (int segment = 0; segment < 8; ++segment) {
    const double startAngle = segment * (3.14159265358979323846 / 4.0);
    const double endAngle = (segment + 1) * (3.14159265358979323846 / 4.0);
    const double midAngle = (startAngle + endAngle) * 0.5;
    builder.quadTo(pointAt(midAngle, controlRadius), pointAt(endAngle, radius));
  }
  builder.closePath();
  return BaselinePathSpec{builder.build(), css::RGBA(255, 0, 0, 128), FillRule::NonZero};
}

/// (b) A self-intersecting five-point star; semi-transparent blue, even-odd fill (exercises the
/// even-odd triangle-wave fold on the doubled-coverage core).
inline BaselinePathSpec BaselineStar() {
  const Vector2d center(170.0, 90.0);
  const double radius = 62.0;

  PathBuilder builder;
  for (int i = 0; i < 5; ++i) {
    // Connect every second vertex of a pentagon: angles step by 144 degrees.
    const double angle = -3.14159265358979323846 / 2.0 + i * (4.0 * 3.14159265358979323846 / 5.0);
    const Vector2d point = center + Vector2d(radius * std::cos(angle), radius * std::sin(angle));
    if (i == 0) {
      builder.moveTo(point);
    } else {
      builder.lineTo(point);
    }
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
