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
#include <cstring>
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

/// Uniform block the generic solid-fill shader IR declares. Both vertical slices fill this one
/// definition, so a change to the IR's block cannot reach one slice and miss the other - which
/// is how the Metal slice came to rasterize geometry the production renderer had retired.
struct alignas(16) SolidFillUniforms {
  float mvp[16];                 //!< Column-major clip-from-scene matrix.
  float patternFromPath[16];     //!< Pattern transform (identity for solid fills).
  float viewport[2];             //!< Viewport size in pixels.
  float tileSize[2];             //!< Pattern tile size (unused for solid fills).
  float color[4];                //!< Premultiplied fill color.
  uint32_t fillRule;             //!< 0 = non-zero, 1 = even-odd.
  uint32_t paintMode;            //!< 0 = solid color.
  float patternOpacity;          //!< 1.0 for solid fills.
  uint32_t hasClipPolygon;       //!< 0 = no clip polygon.
  uint32_t hasClipMask;          //!< 0 = no clip mask.
  uint32_t pad0;                 //!< Padding to the grid block.
  uint32_t pad1;                 //!< Padding.
  uint32_t pad2;                 //!< Padding.
  float gridYBase;               //!< Horizontal band grid base.
  float gridHStride;             //!< Horizontal band stride.
  uint32_t gridHBandCount;       //!< Horizontal band count.
  float gridXBase;               //!< Vertical band grid base.
  float gridVStride;             //!< Vertical band stride.
  uint32_t gridVBandCount;       //!< Vertical band count.
  uint32_t boundingVertexCount;  //!< Vertices in the convex bounding polygon (3 to 8).
  uint32_t gridPad1;             //!< Padding.
  float clipPolygonPlanes[16];   //!< Four vec4 half-planes (unused: hasClipPolygon == 0).
  float boundingVertices[16];    //!< Two path-space vec2 vertices per vec4, up to eight.
};
static_assert(sizeof(SolidFillUniforms) == 352, "SolidFillUniforms must match the shader layout");

/// Builds the same clip-space MVP the production encoder computes: scene -> pixel via
/// \p pixelFromScene, then pixel -> clip with x_clip = 2x/W - 1 and y_clip = -2y/H + 1 (the Y
/// flip for a top-left pixel origin). Column-major mat4.
///
/// @param pixelFromScene Scene-to-pixel transform.
/// @param out16 Receives sixteen column-major floats.
inline void BuildSolidFillMvp(const Transform2d& pixelFromScene, float* out16) {
  const double sx = 2.0 / static_cast<double>(kBaselineSize);
  const double sy = -2.0 / static_cast<double>(kBaselineSize);
  const double a = pixelFromScene.data[0];
  const double b = pixelFromScene.data[1];
  const double c = pixelFromScene.data[2];
  const double d = pixelFromScene.data[3];
  const double e = pixelFromScene.data[4];
  const double f = pixelFromScene.data[5];

  std::memset(out16, 0, 16 * sizeof(float));
  out16[0] = static_cast<float>(sx * a);
  out16[1] = static_cast<float>(sy * b);
  out16[4] = static_cast<float>(sx * c);
  out16[5] = static_cast<float>(sy * d);
  out16[10] = 1.0f;
  out16[12] = static_cast<float>(sx * e - 1.0);
  out16[13] = static_cast<float>(sy * f + 1.0);
  out16[15] = 1.0f;
}

/// Writes an identity 4x4 into \p out16 (column-major). @param out16 Receives sixteen floats.
inline void BuildIdentity4x4(float* out16) {
  std::memset(out16, 0, 16 * sizeof(float));
  out16[0] = out16[5] = out16[10] = out16[15] = 1.0f;
}

/// Mirrors the production encoder's bounding-polygon uniform packing: the vertex count plus two
/// path-space vec2 vertices per vec4.
///
/// @param encoded Encoded path carrying the polygon.
/// @param uniforms Uniform block to fill.
inline void WriteBoundingPolygon(const EncodedPath& encoded, SolidFillUniforms& uniforms) {
  uniforms.boundingVertexCount = encoded.boundingVertexCount;
  for (uint32_t i = 0; i < encoded.boundingVertexCount; ++i) {
    uniforms.boundingVertices[i * 2u] = encoded.boundingVertices[i].x;
    uniforms.boundingVertices[i * 2u + 1u] = encoded.boundingVertices[i].y;
  }
}

}  // namespace donner::gpu::tests
