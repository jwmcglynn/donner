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
 * - **Vertices**: The bounding quads for each band (6 vertices per band = 2 triangles), with
 *   position, outward normal, and band index attributes for the vertex shader.
 */
struct EncodedPath {
  /// A quadratic Bézier curve segment (3 control points) stored as floats for GPU consumption.
  struct Curve {
    float p0x, p0y;  ///< Start point.
    float p1x, p1y;  ///< Control point.
    float p2x, p2y;  ///< End point.
  };

  /// Metadata for one horizontal band. Packed as 2×16-bit in the GPU SSBO.
  struct Band {
    uint16_t curveStart;  ///< Index of the first curve in this band's curve range.
    uint16_t curveCount;  ///< Number of curves intersecting this band.
    float yMin;           ///< Bottom edge of the band (in path space).
    float yMax;           ///< Top edge of the band (in path space).
    float xMin;           ///< Left extent of curves in this band.
    float xMax;           ///< Right extent of curves in this band.
  };

  /// Vertex for the band bounding quad (input to the Slug vertex shader).
  struct Vertex {
    float posX, posY;       ///< Position in path space.
    float normalX, normalY; ///< Outward normal (for dynamic half-pixel dilation).
    uint32_t bandIndex;     ///< Which band this vertex belongs to.
  };

  std::vector<Curve> curves;     ///< All quadratic curves, sorted by band.
  std::vector<Band> bands;       ///< Band metadata.
  std::vector<Vertex> vertices;  ///< Bounding quad vertices (6 per band).
  Box2d pathBounds;              ///< Axis-aligned bounding box of the path.

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
 * 5. Generate bounding quad vertices for each band
 *
 * The output EncodedPath contains all data needed to fill the path on the GPU.
 */
class GeodePathEncoder {
public:
  /**
   * Encode a path for GPU rendering.
   *
   * @param path The path to encode. Will be preprocessed (cubic→quadratic, monotonic split).
   * @param fillRule The fill rule (non-zero or even-odd) — stored for the fragment shader.
   * @param tolerance Quadratic approximation tolerance (default 0.1, suitable for text-size).
   * @return Encoded path data ready for GPU upload, or empty if the path is degenerate.
   */
  static EncodedPath encode(const Path& path, FillRule fillRule, double tolerance = 0.1);

private:
  /// Determine the number of bands for a path of the given height.
  /// Small paths (< 64px) use 1 band; larger paths scale up.
  static uint16_t computeBandCount(float pathHeight);
};

}  // namespace donner::geode
