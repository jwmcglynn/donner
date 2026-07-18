#pragma once
/// @file
/// CPU-side Slug band decomposition: converts a Path into GPU-ready band data.

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/Vector2.h"

namespace donner {
class Path;
enum class FillRule : uint8_t;
}  // namespace donner

namespace donner::geode {

/**
 * GPU-ready encoded path data produced by the Slug band decomposition algorithm.
 *
 * This struct contains all the data needed by the Slug vertex and fragment shaders to render a
 * filled path. It is produced by GeodePathEncoder::encode() and consumed by the GPU pipeline.
 *
 * The data is organized as:
 * - **Curves**: Canonical quadratic Bézier control points (3 × Vector2f per curve).
 * - **Curve references**: Compact per-band indexes into the canonical curve arrays.
 * - **Bands**: Compact offset/count pairs into the curve-reference arrays.
 * - **Bounding polygon**: Up to eight convex vertices enclosing the path. The vertex shader
 *   triangulates and dilates it from `vertex_index`, so no resident vertex buffer is required.
 */
struct EncodedPath {
  /// A quadratic Bézier curve segment (3 control points) stored as floats for GPU consumption.
  struct Curve {
    float p0x, p0y;  ///< Start point.
    float p1x, p1y;  ///< Control point.
    float p2x, p2y;  ///< End point.
  };

  /// Compact metadata for one band. Layout matches the WGSL `Band` struct exactly.
  struct Band {
    uint32_t curveStart;  ///< First entry in the axis's curve-reference array.
    uint32_t curveCount;  ///< Number of curve references in the band.
  };
  static_assert(sizeof(Band) == 8, "Band struct must match WGSL layout");

  /// Per-axis encode statistics retained with the path for diagnostics and perf tests.
  struct AxisStats {
    uint32_t canonicalCurveCount = 0;    ///< Curves stored exactly once for this ray.
    uint32_t curveReferenceCount = 0;    ///< Total indexes across all bands.
    uint32_t omittedParallelCurves = 0;  ///< Curves proven parallel to this ray.
    uint32_t gridBandCount = 0;          ///< Dense band-grid cells.
    uint32_t nonemptyBandCount = 0;      ///< Packed nonempty band records.
    uint32_t maxCurvesPerBand = 0;       ///< Worst packed-band reference count.
    uint32_t p95CurvesPerBand = 0;       ///< 95th percentile nonempty-band count.
    double meanCurvesPerBand = 0.0;      ///< Mean references per nonempty band.
  };

  /// Output-neutral instrumentation for the CPU encoding result.
  struct EncodingStats {
    AxisStats horizontal;
    AxisStats vertical;
    double aabbArea = 0.0;
    double boundingGeometryArea = 0.0;
    uint32_t boundingGeometryVertexCount = 0;
  };

  /// One path-space vertex in the convex bounding polygon.
  struct BoundingPoint {
    float x;
    float y;
  };

  static constexpr uint32_t kMaxBoundingVertices = 8u;

  std::vector<Curve> curves;           ///< Canonical horizontal (Y-monotonic) curves.
  std::vector<uint32_t> curveIndices;  ///< Per-band references into `curves`.
  std::vector<Band> bands;  ///< Horizontal band metadata (Y-strips), for the horizontal ray.
  Box2d pathBounds;         ///< Axis-aligned bounding box of the path.

  /// Small convex path enclosure, counter-clockwise in path space. The first vertex is the fan
  /// origin; the vertex shader expands `(vertexCount - 2) * 3` triangle-list vertices from
  /// `vertex_index`. The selected enclosure is either a support-bounds polygon or its AABB
  /// fallback, so it always has 3 to `kMaxBoundingVertices` vertices for a nonempty path.
  std::array<BoundingPoint, kMaxBoundingVertices> boundingVertices{};
  uint32_t boundingVertexCount = 0;

  /// Vertical (X-monotonic) curve + band data, for the Slug **vertical ray** used by the
  /// dual-ray analytic coverage. These mirror `curves`/`bands`, but are split at X-extrema and
  /// binned into vertical (X-strip) bands.
  std::vector<Curve> vCurves;           ///< Canonical X-monotonic curves.
  std::vector<uint32_t> vCurveIndices;  ///< Per-band references into `vCurves`.
  std::vector<Band> vBands;  ///< Vertical band metadata (X-strips), for the vertical ray.

  /// Sentinel for a grid cell with no band (no curves overlap that strip).
  static constexpr uint32_t kNoBand = 0xFFFFFFFFu;

  /// Dense band-grid lookup, for the analytic dual-ray fragment shader (0041 §8.1). The
  /// shader finds its band in O(1) from the sample position:
  ///   slot = hBandGrid[clamp(floor((y - yBase) / hStride), 0, hBandCount-1)]
  ///   if (slot != kNoBand) iterate bands[slot]'s curves for the horizontal ray.
  /// `vBandGrid` indexes `vBands` by `(x - xBase) / vStride` for the vertical ray. The
  /// grids map dense grid cells onto the empty-skipped `bands`/`vBands` arrays, so no
  /// curve data is duplicated. Empty for a degenerate axis (count 0).
  std::vector<uint32_t> hBandGrid;  ///< size == hBandCount; cell → index into `bands`.
  std::vector<uint32_t> vBandGrid;  ///< size == vBandCount; cell → index into `vBands`.
  float yBase = 0.0f;               ///< Top edge of the horizontal band grid (path space).
  float hStride = 0.0f;             ///< Height of each horizontal band cell (path space).
  uint32_t hBandCount = 0;          ///< Number of horizontal band cells.
  float xBase = 0.0f;               ///< Left edge of the vertical band grid (path space).
  float vStride = 0.0f;             ///< Width of each vertical band cell (path space).
  uint32_t vBandCount = 0;          ///< Number of vertical band cells.

  EncodingStats stats;  ///< Encode diagnostics; does not affect rendering.

  /// Triangle-list vertex count emitted by the vertex shader for the bounding fan.
  uint32_t boundingDrawVertexCount() const {
    return boundingVertexCount >= 3u ? (boundingVertexCount - 2u) * 3u : 0u;
  }

  /// Returns true if the encoded path has no bands (empty or degenerate path).
  bool empty() const { return bands.empty(); }
};

/**
 * Encodes a Path into Slug band decomposition format for GPU rendering.
 *
 * The encoding pipeline:
 * 1. Convert cubics to quadratics (Path::cubicToQuadratic)
 * 2. Split curves at Y-monotone points (Path::toMonotonic)
 * 3. Compute path bounds and determine band count
 * 4. Assign curves to bands and sort
 * 5. Generate a small conservative whole-path bounding polygon
 *
 * The output EncodedPath contains all data needed to fill the path on the GPU.
 */
class GeodePathEncoder {
public:
  /**
   * Encode a path for GPU rendering.
   *
   * @param path The path to encode. Will be preprocessed (cubic→quadratic, monotonic split).
   * @param fillRule The fill rule (non-zero or even-odd) - stored for the fragment shader.
   * @param tolerance Quadratic approximation tolerance (default 0.1, suitable for text-size).
   * @return Encoded path data ready for GPU upload, or empty if the path is degenerate.
   */
  static EncodedPath encode(const Path& path, FillRule fillRule, double tolerance = 0.1);
};

}  // namespace donner::geode
