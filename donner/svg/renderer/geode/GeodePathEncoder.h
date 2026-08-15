#pragma once
/// @file
/// CPU-side Slug band decomposition: converts a Path into GPU-ready band data.

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
 * - **Curves**: Quadratic Bézier control points (3 × Vector2f per curve), packed contiguously.
 * - **Bands**: Horizontal slices through the path. Each band references a contiguous range of
 *   curves and has a bounding quad for rasterization.
 * - **quadVertices**: A single bounding quad over the whole path (6 vertices = 2 triangles),
 *   with position, outward normal, and band index attributes for the vertex shader.
 */
struct EncodedPath {
  /// A quadratic Bézier curve segment (3 control points) stored as floats for GPU consumption.
  struct Curve {
    float p0x, p0y;  ///< Start point.
    float p1x, p1y;  ///< Control point.
    float p2x, p2y;  ///< End point.
  };

  /// Metadata for one horizontal band.
  ///
  /// Layout matches the WGSL `Band` struct in shaders/slug_fill.wgsl exactly
  /// (8 × 4 bytes = 32 bytes per band). Two trailing pad fields are required
  /// because storage buffer struct stride must be 16-byte aligned, and
  /// without them WGSL would round the struct to 32 bytes anyway.
  struct Band {
    uint32_t curveStart;  ///< Index of the first curve in this band's curve range.
    uint32_t curveCount;  ///< Number of curves intersecting this band.
    float yMin;           ///< Bottom edge of the band (in path space).
    float yMax;           ///< Top edge of the band (in path space).
    float xMin;           ///< Left extent of curves in this band.
    float xMax;           ///< Right extent of curves in this band.
    float _pad0;          ///< Padding to match WGSL stride.
    float _pad1;          ///< Padding to match WGSL stride.
  };
  static_assert(sizeof(Band) == 32, "Band struct must match WGSL layout");

  /// Vertex for the path bounding quad (input to the Slug vertex shader).
  struct Vertex {
    float posX, posY;        ///< Position in path space.
    float normalX, normalY;  ///< Outward normal (for dynamic half-pixel dilation).
    uint32_t bandIndex;      ///< Which band this vertex belongs to (unused by the dual-ray fill).
  };

  std::vector<Curve> curves;  ///< Horizontal (Y-monotonic) curves, sorted by band.
  std::vector<Band> bands;    ///< Horizontal band metadata (Y-strips), for the horizontal ray.
  Box2d pathBounds;           ///< Axis-aligned bounding box of the path.

  /// Single bounding quad (6 verts) over the whole path, for the analytic dual-ray fill
  /// shader (0041 §8.1). One quad per path means each pixel is rasterized by exactly one
  /// fragment, so folded sampleCount=1 coverage composes correctly (no band-seam
  /// double-count - Blocker B). `bandIndex` is unused (the fragment looks up both its
  /// H- and V-band from `sample_pos` via the band grids).
  std::vector<Vertex> quadVertices;

  /// Vertical (X-monotonic) curve + band data, for the Slug **vertical ray** used by the
  /// dual-ray analytic coverage. These mirror `curves`/`bands`
  /// but are split at X-extrema and binned into vertical (X-strip) bands. For a vertical
  /// `Band` the field semantics are transposed: `xMin`/`xMax` are the band's X-strip
  /// boundaries and `yMin`/`yMax` are the Y-extent of the curves in the band.
  std::vector<Curve> vCurves;  ///< X-monotonic curves, sorted by vertical band.
  std::vector<Band> vBands;    ///< Vertical band metadata (X-strips), for the vertical ray.

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
 * 5. Generate the whole-path bounding quad (6 vertices = 2 triangles)
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
