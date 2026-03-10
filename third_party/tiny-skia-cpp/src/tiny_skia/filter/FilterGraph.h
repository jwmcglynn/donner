#pragma once

/// @file FilterGraph.h
/// @brief Filter graph data structures and executor for SVG filter effects.
///
/// Provides a self-contained filter graph representation and executor that processes
/// a sequence of filter primitive nodes against a source pixmap. The caller is responsible
/// for converting high-level SVG data (user-space coordinates, CSS colors) into pixel-space
/// parameters before building the graph.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "tiny_skia/Pixmap.h"
#include "tiny_skia/filter/Blend.h"
#include "tiny_skia/filter/ColorMatrix.h"
#include "tiny_skia/filter/ComponentTransfer.h"
#include "tiny_skia/filter/Composite.h"
#include "tiny_skia/filter/ConvolveMatrix.h"
#include "tiny_skia/filter/DisplacementMap.h"
#include "tiny_skia/filter/FloatPixmap.h"
#include "tiny_skia/filter/GaussianBlur.h"
#include "tiny_skia/filter/Lighting.h"
#include "tiny_skia/filter/Morphology.h"
#include "tiny_skia/filter/Turbulence.h"

namespace tiny_skia::filter {

/// Standard named inputs available to filter primitives.
enum class StandardInput : std::uint8_t {
  SourceGraphic,  ///< The original element rendering.
  SourceAlpha,    ///< Alpha channel of SourceGraphic (RGB = 0).
  FillPaint,      ///< The element's fill paint.
  StrokePaint,    ///< The element's stroke paint.
};

/// Identifies the input to a filter primitive node.
struct NodeInput {
  /// The input is the output of the immediately preceding primitive (or SourceGraphic for the first
  /// primitive).
  struct Previous {};

  /// The input is a named result from a prior primitive's `result` attribute.
  struct Named {
    std::string name;
  };

  /// Variant of all input types.
  using Type = std::variant<Previous, StandardInput, Named>;

  Type value;  ///< The input specification.

  /// Construct an implicit previous-result input.
  NodeInput() : value(Previous{}) {}

  /// Construct from a standard input keyword.
  /* implicit */ NodeInput(StandardInput standard) : value(standard) {}

  /// Construct from a named result reference.
  /* implicit */ NodeInput(Named named) : value(std::move(named)) {}
};

/// Pixel-space rectangle.
struct PixelRect {
  double x = 0;
  double y = 0;
  double w = 0;
  double h = 0;
};

/// Filter primitive parameter types with owning storage.
///
/// All spatial parameters (sigma, offset, radius) are in pixel space — the caller must convert
/// from user space before building graph nodes.
namespace graph_primitive {

/// Parameters for feGaussianBlur.
struct GaussianBlur {
  double sigmaX = 0.0;  ///< Standard deviation in pixels (X).
  double sigmaY = 0.0;  ///< Standard deviation in pixels (Y).
  BlurEdgeMode edgeMode = BlurEdgeMode::None;
};

/// Parameters for feFlood.
struct Flood {
  std::uint8_t r = 0;    ///< Red (premultiplied sRGB).
  std::uint8_t g = 0;    ///< Green (premultiplied sRGB).
  std::uint8_t b = 0;    ///< Blue (premultiplied sRGB).
  std::uint8_t a = 255;  ///< Alpha.
};

/// Parameters for feOffset.
struct Offset {
  int dx = 0;  ///< Horizontal offset in pixels.
  int dy = 0;  ///< Vertical offset in pixels.
};

/// Parameters for feComposite.
struct Composite {
  CompositeOp op = CompositeOp::Over;
  double k1 = 0;
  double k2 = 0;
  double k3 = 0;
  double k4 = 0;
};

/// Parameters for feBlend.
struct Blend {
  BlendMode mode = BlendMode::Normal;
};

/// Parameters for feMerge. Inputs are stored on the GraphNode.
struct Merge {};

/// Parameters for feColorMatrix. Matrix is already resolved to the 5x4 form.
struct ColorMatrix {
  std::array<double, 20> matrix;
};

/// Parameters for feComponentTransfer with owning table storage.
struct ComponentTransfer {
  /// Owning transfer function (unlike TransferFunc which uses std::span).
  struct Func {
    TransferFuncType type = TransferFuncType::Identity;
    std::vector<double> tableValues;
    double slope = 1.0;
    double intercept = 0.0;
    double amplitude = 1.0;
    double exponent = 1.0;
    double offset = 0.0;
  };

  Func funcR;
  Func funcG;
  Func funcB;
  Func funcA;
};

/// Parameters for feConvolveMatrix with owning kernel storage.
struct ConvolveMatrix {
  int orderX = 3;
  int orderY = 3;
  std::vector<double> kernel;  ///< Owning kernel data (orderX * orderY).
  double divisor = 1.0;
  double bias = 0.0;
  int targetX = 1;
  int targetY = 1;
  ConvolveEdgeMode edgeMode = ConvolveEdgeMode::Duplicate;
  bool preserveAlpha = false;
};

/// Parameters for feMorphology.
struct Morphology {
  MorphologyOp op = MorphologyOp::Erode;
  int radiusX = 0;  ///< Horizontal radius in pixels.
  int radiusY = 0;  ///< Vertical radius in pixels.
};

/// Parameters for feTile. Tile region is derived from the input's subregion.
struct Tile {};

/// Parameters for feTurbulence.
struct Turbulence {
  TurbulenceParams params;
};

/// Parameters for feDisplacementMap.
struct DisplacementMap {
  double scale = 0.0;
  DisplacementChannel xChannel = DisplacementChannel::A;
  DisplacementChannel yChannel = DisplacementChannel::A;
};

/// Parameters for feDiffuseLighting.
struct DiffuseLighting {
  DiffuseLightingParams params;
};

/// Parameters for feSpecularLighting.
struct SpecularLighting {
  SpecularLightingParams params;
};

/// Parameters for feDropShadow (decomposed internally into flood+composite+offset+blur+merge).
struct DropShadow {
  std::uint8_t r = 0;    ///< Shadow color red (premultiplied sRGB).
  std::uint8_t g = 0;    ///< Shadow color green (premultiplied sRGB).
  std::uint8_t b = 0;    ///< Shadow color blue (premultiplied sRGB).
  std::uint8_t a = 255;  ///< Shadow color alpha.
  int dx = 0;            ///< Horizontal offset in pixels.
  int dy = 0;            ///< Vertical offset in pixels.
  double sigmaX = 0.0;   ///< Blur standard deviation X in pixels.
  double sigmaY = 0.0;   ///< Blur standard deviation Y in pixels.
};

/// Parameters for feImage. Image data is pre-loaded by the caller and stored here as premultiplied
/// RGBA pixels.
struct Image {
  std::vector<std::uint8_t> pixels;  ///< Premultiplied RGBA pixel data.
  int width = 0;                     ///< Image width in pixels.
  int height = 0;                    ///< Image height in pixels.
  /// Target rectangle within the filter output (pixel space). If empty, uses the full output.
  std::optional<PixelRect> targetRect;
};

}  // namespace graph_primitive

/// Variant holding all filter primitive types for graph nodes.
using GraphPrimitive = std::variant<
    graph_primitive::GaussianBlur, graph_primitive::Flood, graph_primitive::Offset,
    graph_primitive::Composite, graph_primitive::Blend, graph_primitive::Merge,
    graph_primitive::ColorMatrix, graph_primitive::ComponentTransfer,
    graph_primitive::ConvolveMatrix, graph_primitive::Morphology, graph_primitive::Tile,
    graph_primitive::Turbulence, graph_primitive::DisplacementMap, graph_primitive::DiffuseLighting,
    graph_primitive::SpecularLighting, graph_primitive::DropShadow, graph_primitive::Image>;

/// A single node in the filter graph.
struct GraphNode {
  GraphPrimitive primitive;            ///< The filter operation.
  std::vector<NodeInput> inputs;       ///< Input reference(s).
  std::optional<std::string> result;   ///< Named output (for `result` attribute).
  std::optional<PixelRect> subregion;  ///< Pixel-space primitive subregion (for clipping).

  /// Per-node color space override. When set, overrides the graph-level `useLinearRGB`.
  /// true = linearRGB, false = sRGB.
  std::optional<bool> useLinearRGB;
};

/// Complete filter graph specification ready for execution.
struct FilterGraph {
  std::vector<GraphNode> nodes;           ///< Nodes in execution order.
  bool useLinearRGB = true;               ///< Convert to linearRGB for processing.
  bool clipSourceToFilterRegion = false;  ///< Zero SourceGraphic outside filterRegion before eval.

  std::optional<FloatPixmap> fillPaintInput;    ///< Optional FillPaint standard input.
  std::optional<FloatPixmap> strokePaintInput;  ///< Optional StrokePaint standard input.

  /// Filter region in pixel space (clips subregions). If not set, uses full pixmap extent.
  std::optional<PixelRect> filterRegion;
};

/// Execute a filter graph on a source pixmap.
///
/// The source pixmap is modified in place with the filter result. All spatial parameters
/// in the graph (sigma, offset, radius, subregion) must already be in pixel space.
///
/// @param sourceGraphic The rendered element content. Modified in-place on success.
/// @param graph The filter graph to execute.
/// @return true if a filter result was produced, false if the graph produced no output.
bool executeFilterGraph(Pixmap& sourceGraphic, const FilterGraph& graph);

}  // namespace tiny_skia::filter
