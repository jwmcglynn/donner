#pragma once
/// @file

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <type_traits>
#include <variant>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/EcsRegistry_fwd.h"
#include "donner/base/Length.h"
#include "donner/base/RcString.h"
#include "donner/css/Color.h"
#include "donner/svg/SVGDocumentHandle.h"
#include "donner/svg/components/filter/FilterUnits.h"
#include "donner/svg/core/ColorInterpolationFilters.h"
#include "donner/svg/core/ImageRendering.h"
#include "donner/svg/core/PreserveAspectRatio.h"

namespace donner::svg::components {

/// Maximum accepted user-space filter offset magnitude.
inline constexpr double kMaximumFilterOffset = 4096.0;

/// Maximum accepted user-space blur standard deviation.
inline constexpr double kMaximumFilterStdDeviation = 256.0;

/// Maximum accepted user-space morphology radius.
inline constexpr double kMaximumFilterMorphologyRadius = 256.0;

/// Maximum filter offset after conversion to device pixels.
inline constexpr int kMaximumFilterPixelOffset = 4096;

/// Maximum blur standard deviation or morphology radius after conversion to device pixels.
inline constexpr int kMaximumFilterPixelRadius = 256;

/// Geode decomposes morphology into shader passes whose per-axis radius is at most 31.
inline constexpr std::uint64_t kMaximumFilterMorphologyPassesPerAxis = 9;
inline constexpr std::uint64_t kMaximumFilterMorphologyWorkMultiplier =
    2 * (2 * kMaximumFilterPixelRadius + kMaximumFilterMorphologyPassesPerAxis) + 5;
inline constexpr std::uint64_t kMaximumFilterMorphologyRetainedBuffers =
    2 * kMaximumFilterMorphologyPassesPerAxis + 3;

/// Maximum dimension of a filter-specific intermediate surface.
inline constexpr int kMaximumFilterSurfaceDimension = 4096;

/// Maximum pixel area of a filter-specific intermediate surface.
///
/// CPU filter execution expands each pixel into several float buffers. Keep the limit below a
/// 4096-by-4096 surface while retaining the established high-zoom rendering envelope.
inline constexpr std::size_t kMaximumFilterSurfacePixels = 12 * 1024 * 1024;

/// Maximum number of primitives retained in one computed filter graph.
inline constexpr std::size_t kMaximumFilterGraphNodes = 32;

/// Maximum number of inputs composited by one feMerge primitive.
inline constexpr std::size_t kMaximumFilterMergeInputs = 16;

/// Maximum number of samples accepted for one feComponentTransfer channel table.
inline constexpr std::size_t kMaximumFilterTableValues = 1024;

/// Maximum numeric payload accepted for feColorMatrix (the 5-by-4 matrix form).
inline constexpr std::size_t kMaximumFilterColorMatrixValues = 20;

/// Maximum convolve order on either axis and aggregate kernel payload.
inline constexpr int kMaximumFilterConvolveOrder = 64;
inline constexpr std::size_t kMaximumFilterKernelValues =
    static_cast<std::size_t>(kMaximumFilterConvolveOrder) * kMaximumFilterConvolveOrder;

/// Maximum aggregate pixel operations accepted for one filter execution.
inline constexpr std::uint64_t kMaximumFilterWorkUnits = 256ULL * 1024 * 1024;

/// Maximum aggregate pixel operations accepted across one rendered frame.
///
/// The established splash asset contains three independent high-zoom blur layers. This allows
/// that bounded workload while rejecting a fourth full-envelope blur and large repeated graphs.
inline constexpr std::uint64_t kMaximumFilterFrameWorkUnits = 512ULL * 1024 * 1024;

/// Maximum estimated bytes retained by filter intermediates during one execution.
///
/// This keeps ordinary production-sized filters available while preventing one graph from
/// consuming a large fraction of the desktop or Wasm heap.
inline constexpr std::uint64_t kMaximumFilterIntermediateBytes = 256ULL * 1024 * 1024;

/// Maximum filter memory reserved across one frame, including RGBA capture surfaces.
///
/// Native builds allow one established high-zoom blur capture plus its in-place float surface.
/// Wasm retains the lower ceiling because its fixed heap cannot safely admit that desktop-only
/// workload. Renderer surfaces and decoded resources remain separately bounded.
#ifdef __EMSCRIPTEN__
inline constexpr std::uint64_t kMaximumFilterFrameBytes = 128ULL * 1024 * 1024;
#else
inline constexpr std::uint64_t kMaximumFilterFrameBytes = 256ULL * 1024 * 1024;
#endif

/// Maximum accepted feTurbulence base-frequency magnitude.
inline constexpr double kMaximumFilterTurbulenceFrequency = 1024.0;

/// Maximum accepted feTurbulence seed magnitude (Park-Miller modulus minus one).
inline constexpr double kMaximumFilterTurbulenceSeed = 2147483646.0;

/// Maximum accepted feTurbulence octave count.
inline constexpr int kMaximumFilterTurbulenceOctaves = 16;

/**
 * Standard named inputs available to filter primitives.
 *
 * @see https://drafts.fxtf.org/filter-effects/#FilterPrimitiveSubRegion
 */
enum class FilterStandardInput : std::uint8_t {
  SourceGraphic,  ///< The original element rendering.
  SourceAlpha,    ///< Alpha channel of SourceGraphic (RGB = 0).
  FillPaint,      ///< The element's fill paint, conceptually infinite.
  StrokePaint,    ///< The element's stroke paint, conceptually infinite.
};

/**
 * Identifies the input to a filter primitive node.
 */
struct FilterInput {
  /// The input is the output of the immediately preceding primitive (or SourceGraphic for the first
  /// primitive).
  struct Previous {};

  /// The input is a named result from a prior primitive's `result` attribute.
  struct Named {
    RcString name;  ///< The result name to reference.
  };

  /// Variant of all input types.
  using Type = std::variant<Previous, FilterStandardInput, Named>;

  Type value;  ///< The input specification.

  /// Construct an implicit previous-result input.
  FilterInput() : value(Previous{}) {}

  /// Construct from a standard input keyword.
  /* implicit */ FilterInput(FilterStandardInput standard) : value(standard) {}

  /// Construct from a named result reference.
  /* implicit */ FilterInput(Named named) : value(std::move(named)) {}

  /// Construct from a variant value.
  /* implicit */ FilterInput(Type value) : value(std::move(value)) {}
};

/**
 * Variant holding the parameters for each type of filter primitive.
 *
 * Each struct corresponds to one SVG filter primitive element and holds only the
 * primitive-specific attributes; common attributes (in, result, subregion) are stored on
 * \ref FilterNode.
 */
namespace filter_primitive {

/// Parameters for \c feGaussianBlur.
struct GaussianBlur {
  /// Edge handling mode.
  enum class EdgeMode : std::uint8_t {
    None,       ///< Treat out-of-bounds pixels as transparent black.
    Duplicate,  ///< Clamp to nearest edge pixel.
    Wrap,       ///< Wrap around (modular arithmetic).
  };

  double stdDeviationX = 0.0;          ///< Standard deviation in X.
  double stdDeviationY = 0.0;          ///< Standard deviation in Y.
  EdgeMode edgeMode = EdgeMode::None;  ///< Edge handling mode.
};

/// Parameters for \c feFlood.
struct Flood {
  css::Color floodColor{css::RGBA(0, 0, 0, 0xFF)};  ///< Flood color (default: black).
  double floodOpacity = 1.0;                        ///< Flood opacity (default: 1).
};

/// Parameters for \c feOffset.
struct Offset {
  double dx = 0.0;  ///< Horizontal offset.
  double dy = 0.0;  ///< Vertical offset.
};

/// Parameters for \c feBlend.
struct Blend {
  /// Blend mode values.
  enum class Mode : std::uint8_t {
    Normal,
    Multiply,
    Screen,
    Darken,
    Lighten,
    Overlay,
    ColorDodge,
    ColorBurn,
    HardLight,
    SoftLight,
    Difference,
    Exclusion,
    Hue,
    Saturation,
    Color,
    Luminosity,
  };

  Mode mode = Mode::Normal;  ///< Blend mode.
};

/// Parameters for \c feComposite.
struct Composite {
  /// Porter-Duff operator.
  enum class Operator : std::uint8_t {
    Over,
    In,
    Out,
    Atop,
    Xor,
    Lighter,
    Arithmetic,
  };

  Operator op = Operator::Over;  ///< Compositing operator.
  double k1 = 0.0;               ///< Arithmetic coefficient k1.
  double k2 = 0.0;               ///< Arithmetic coefficient k2.
  double k3 = 0.0;               ///< Arithmetic coefficient k3.
  double k4 = 0.0;               ///< Arithmetic coefficient k4.
};

/// Parameters for \c feColorMatrix.
struct ColorMatrix {
  /// Matrix type.
  enum class Type : std::uint8_t {
    Matrix,            ///< 5x4 color matrix (20 values).
    Saturate,          ///< Single value 0..1.
    HueRotate,         ///< Angle in degrees.
    LuminanceToAlpha,  ///< No values.
  };

  Type type = Type::Matrix;    ///< Matrix type.
  std::vector<double> values;  ///< Matrix values (interpretation depends on type).
};

/// Parameters for \c feMerge. Children are represented as additional inputs on the FilterNode.
struct Merge {};

/// Parameters for \c feDropShadow.
struct DropShadow {
  double dx = 2.0;                                  ///< Horizontal offset.
  double dy = 2.0;                                  ///< Vertical offset.
  double stdDeviationX = 2.0;                       ///< Blur standard deviation X.
  double stdDeviationY = 2.0;                       ///< Blur standard deviation Y.
  css::Color floodColor{css::RGBA(0, 0, 0, 0xFF)};  ///< Shadow color (default: black).
  double floodOpacity = 1.0;                        ///< Shadow opacity (default: 1).
};

/// Parameters for \c feComponentTransfer.
struct ComponentTransfer {
  /// Transfer function type.
  enum class FuncType : std::uint8_t {
    Identity,
    Table,
    Discrete,
    Linear,
    Gamma,
  };

  /// A single channel's transfer function.
  struct Func {
    FuncType type = FuncType::Identity;  ///< Function type.
    std::vector<double> tableValues;     ///< Table values (for table/discrete).
    double slope = 1.0;                  ///< Slope (for linear).
    double intercept = 0.0;              ///< Intercept (for linear).
    double amplitude = 1.0;              ///< Amplitude (for gamma).
    double exponent = 1.0;               ///< Exponent (for gamma).
    double offset = 0.0;                 ///< Offset (for gamma).
  };

  Func funcR;  ///< Red channel transfer function.
  Func funcG;  ///< Green channel transfer function.
  Func funcB;  ///< Blue channel transfer function.
  Func funcA;  ///< Alpha channel transfer function.
};

/// Parameters for \c feConvolveMatrix.
struct ConvolveMatrix {
  /// Edge mode for out-of-bounds pixels.
  enum class EdgeMode : std::uint8_t {
    Duplicate,
    Wrap,
    None,
  };

  int orderX = 3;                           ///< Kernel width.
  int orderY = 3;                           ///< Kernel height.
  std::vector<double> kernelMatrix;         ///< Kernel values (orderX * orderY).
  std::optional<double> divisor;            ///< Divisor (nullopt = sum of kernel values).
  double bias = 0.0;                        ///< Bias added to result.
  std::optional<int> targetX;               ///< Target X (nullopt = floor(orderX/2)).
  std::optional<int> targetY;               ///< Target Y (nullopt = floor(orderY/2)).
  EdgeMode edgeMode = EdgeMode::Duplicate;  ///< Edge handling mode.
  bool preserveAlpha = false;               ///< If true, only filter RGB channels.
};

/// Parameters for \c feMorphology.
struct Morphology {
  /// Morphology operator.
  enum class Operator : std::uint8_t {
    Erode,
    Dilate,
  };

  Operator op = Operator::Erode;  ///< Erode or dilate.
  double radiusX = 0.0;           ///< Horizontal radius.
  double radiusY = 0.0;           ///< Vertical radius.
};

/// Parameters for \c feTile.
struct Tile {};

/// Parameters for \c feTurbulence.
struct Turbulence {
  /// Noise type.
  enum class Type : std::uint8_t {
    FractalNoise,
    Turbulence,
  };

  Type type = Type::Turbulence;  ///< Noise type.
  double baseFrequencyX = 0.0;   ///< Base frequency X.
  double baseFrequencyY = 0.0;   ///< Base frequency Y.
  int numOctaves = 1;            ///< Number of octaves.
  double seed = 0.0;             ///< Random seed.
  bool stitchTiles = false;      ///< Whether to stitch tiles.
};

/// Parameters for \c feImage.
struct Image {
  RcString href;  ///< Image URL or fragment reference.
  PreserveAspectRatio preserveAspectRatio = PreserveAspectRatio::Default();

  /// Shared loaded image data (RGBA, straight alpha). Null if loading failed or href is a fragment.
  ///
  /// Prepared filter graphs are copied once per rendering instance. Sharing immutable decoded
  /// pixels prevents one referenced raster from being duplicated for every filtered element.
  std::shared_ptr<const std::vector<uint8_t>> imageData;
  int imageWidth = 0;   ///< Width of loaded image in pixels.
  int imageHeight = 0;  ///< Height of loaded image in pixels.

  [[nodiscard]] bool hasImageData() const { return imageData && !imageData->empty(); }
  [[nodiscard]] std::size_t imageDataSize() const { return imageData ? imageData->size() : 0; }

  /// Shared handle to an external SVG sub-document. The renderer pre-renders this to pixel data
  /// before filter execution.
  SVGDocumentHandle svgSubDocument;

  /// Fragment ID for same-document element references (e.g., href="#rect1" stores "rect1").
  /// The renderer resolves this to pixel data before filter execution.
  RcString fragmentId;

  /// True when this image was rendered from a same-document element reference. Fragment images
  /// are rendered in the SVG's user-space coordinate system and should be placed at 1:1 in the
  /// filter pixmap without preserveAspectRatio scaling.
  bool isFragmentReference = false;

  /// The filter region top-left in user-space coordinates, used by fragment references to apply
  /// a device-space post-translation that positions the fragment content at the filter primitive
  /// subregion origin. Set by the renderer driver during pre-rendering.
  Vector2d fragmentRegionTopLeft;

  /// Resolved sampling policy inherited by the source `<feImage>`.
  ImageRendering imageRendering = ImageRendering::Auto;
};

/// Parameters for \c feDisplacementMap.
struct DisplacementMap {
  /// Channel selector.
  enum class Channel : std::uint8_t { R, G, B, A };

  double scale = 0.0;                     ///< Displacement scale factor.
  Channel xChannelSelector = Channel::A;  ///< Channel to use for X displacement.
  Channel yChannelSelector = Channel::A;  ///< Channel to use for Y displacement.
};

/// Light source parameters for lighting filter primitives.
struct LightSource {
  /// Light source type.
  enum class Type : std::uint8_t { Distant, Point, Spot };

  Type type = Type::Distant;  ///< Which kind of light source is represented.

  // feDistantLight
  double azimuth = 0.0;    ///< Angle in the XY plane (degrees).
  double elevation = 0.0;  ///< Angle above the XY plane (degrees).

  // fePointLight / feSpotLight
  double x = 0.0;  ///< X position.
  double y = 0.0;  ///< Y position.
  double z = 0.0;  ///< Z position.

  // feSpotLight
  double pointsAtX = 0.0;                   ///< X target.
  double pointsAtY = 0.0;                   ///< Y target.
  double pointsAtZ = 0.0;                   ///< Z target.
  double spotExponent = 1.0;                ///< Spotlight exponent.
  std::optional<double> limitingConeAngle;  ///< Cone angle limit (degrees).
};

/// Parameters for \c feDiffuseLighting.
struct DiffuseLighting {
  double surfaceScale = 1.0;                                    ///< Height of surface.
  double diffuseConstant = 1.0;                                 ///< Diffuse reflection constant.
  css::Color lightingColor{css::RGBA(0xFF, 0xFF, 0xFF, 0xFF)};  ///< Light color (default: white).
  std::optional<LightSource> light;  ///< Light source (from child element).
};

/// Parameters for \c feSpecularLighting.
struct SpecularLighting {
  double surfaceScale = 1.0;                                    ///< Height of surface.
  double specularConstant = 1.0;                                ///< Specular reflection constant.
  double specularExponent = 1.0;                                ///< Specular exponent (1..128).
  css::Color lightingColor{css::RGBA(0xFF, 0xFF, 0xFF, 0xFF)};  ///< Light color (default: white).
  std::optional<LightSource> light;  ///< Light source (from child element).
};

}  // namespace filter_primitive

/**
 * Variant holding any filter primitive type.
 */
using FilterPrimitive =
    std::variant<filter_primitive::GaussianBlur, filter_primitive::Flood, filter_primitive::Offset,
                 filter_primitive::Merge, filter_primitive::Blend, filter_primitive::Composite,
                 filter_primitive::ColorMatrix, filter_primitive::DropShadow,
                 filter_primitive::ComponentTransfer, filter_primitive::ConvolveMatrix,
                 filter_primitive::Morphology, filter_primitive::Tile, filter_primitive::Turbulence,
                 filter_primitive::Image, filter_primitive::DisplacementMap,
                 filter_primitive::DiffuseLighting, filter_primitive::SpecularLighting>;

/// Dynamic numeric storage retained by one primitive payload.
inline std::uint64_t FilterPrimitivePayloadBytes(const FilterPrimitive& primitive) {
  return std::visit(
      [](const auto& value) -> std::uint64_t {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, filter_primitive::ColorMatrix>) {
          return value.values.capacity() * sizeof(double);
        } else if constexpr (std::is_same_v<T, filter_primitive::ConvolveMatrix>) {
          return value.kernelMatrix.capacity() * sizeof(double);
        } else if constexpr (std::is_same_v<T, filter_primitive::ComponentTransfer>) {
          return (value.funcR.tableValues.capacity() + value.funcG.tableValues.capacity() +
                  value.funcB.tableValues.capacity() + value.funcA.tableValues.capacity()) *
                 sizeof(double);
        } else {
          return 0;
        }
      },
      primitive);
}

/**
 * A single node in the filter graph, representing one filter primitive.
 *
 * Nodes are executed in document order. Each node reads from its inputs (which may be
 * standard inputs like SourceGraphic, or outputs of prior nodes), applies its primitive
 * operation, and writes to an output buffer.
 */
struct FilterNode {
  FilterPrimitive primitive;  ///< The filter primitive operation.

  /// Input(s) to this primitive. A slot this list leaves unpopulated resolves exactly like an
  /// unspecified `in`/`in2` attribute: to the previous primitive's result, and to SourceGraphic
  /// only when the node is the first primitive in the graph. A Composite, Blend, or
  /// DisplacementMap node built with a single input therefore reads the previous result as its
  /// second input, never the source graphic.
  std::vector<FilterInput> inputs;

  std::optional<RcString> result;  ///< Named output (for `result` attribute).
  std::optional<Lengthd> x;        ///< Primitive subregion X.
  std::optional<Lengthd> y;        ///< Primitive subregion Y.
  std::optional<Lengthd> width;    ///< Primitive subregion width.
  std::optional<Lengthd> height;   ///< Primitive subregion height.

  /// Per-primitive color-interpolation-filters. When set, overrides the graph-level default.
  std::optional<ColorInterpolationFilters> colorInterpolationFilters;
};

/**
 * The complete filter graph for a \c \<filter\> element.
 *
 * Contains an ordered list of filter nodes derived from the filter element's children.
 * The output of the last node is the filter result that gets composited onto the canvas.
 */
struct FilterGraph {
  std::vector<FilterNode> nodes;  ///< Nodes in document (execution) order.

  /// Color space for filter operations (linearRGB or sRGB).
  ColorInterpolationFilters colorInterpolationFilters = ColorInterpolationFilters::Default;

  /// Coordinate system for primitive subregion and primitive-specific length attributes.
  PrimitiveUnits primitiveUnits = PrimitiveUnits::Default;

  /// Bounding box of the referencing element, used when primitiveUnits=objectBoundingBox.
  /// Set by the renderer driver before passing to the renderer.
  std::optional<Box2d> elementBoundingBox;

  /// The filter region in user-space coordinates.
  std::optional<Box2d> filterRegion;

  /// Scale factor from SVG user-space coordinates to pixel-space coordinates.
  /// Needed by lighting filters to transform light positions from user space to the pixel-space
  /// pixmap. Set by the renderer driver from the viewBox and canvas dimensions.
  Vector2d userToPixelScale = Vector2d(1.0, 1.0);

  /// Returns true if the graph has no nodes.
  [[nodiscard]] bool empty() const { return nodes.empty(); }
};

/// Intermediate-retention behavior used to estimate backend memory before filter execution.
enum class FilterMemoryModel : std::uint8_t {
  CpuFloatNamedResults,  ///< TinySkia retains named float RGBA results (16 bytes per pixel).
  GpuAllNodes,           ///< Geode may retain several RGBA textures for every graph node.
};

/**
 * Estimate a filter graph's aggregate execution cost for a pixel surface.
 *
 * This rejects attacker-controlled graphs before allocating per-node buffers or starting
 * convolution loops. The estimate is intentionally conservative: GPU primitives may need up to
 * four intermediate RGBA textures, while the CPU backend retains every named float result.
 */
inline bool FilterGraphExecutionCost(const FilterGraph& graph, std::uint64_t pixelCount,
                                     FilterMemoryModel memoryModel, std::uint64_t& workUnitsOut,
                                     std::uint64_t& intermediateBytesOut) {
  if (pixelCount > kMaximumFilterSurfacePixels || graph.nodes.size() > kMaximumFilterGraphNodes) {
    return false;
  }

  std::uint64_t workUnits = 0;
  std::uint64_t totalMergeInputs = 0;
  for (const FilterNode& node : graph.nodes) {
    // Every executed node performs its primitive pass plus a mandatory subregion clip. Default
    // linearRGB processing can add input/output conversion passes, and binary primitives convert
    // both inputs. Five full-surface passes is a conservative baseline for the generic case.
    std::uint64_t workMultiplier = 5;
    if (const auto* convolve = std::get_if<filter_primitive::ConvolveMatrix>(&node.primitive)) {
      if (convolve->orderX <= 0 || convolve->orderY <= 0) {
        return false;
      }
      workMultiplier = static_cast<std::uint64_t>(convolve->orderX) *
                           static_cast<std::uint64_t>(convolve->orderY) +
                       4;
    } else if (const auto* turbulence =
                   std::get_if<filter_primitive::Turbulence>(&node.primitive)) {
      if (turbulence->numOctaves > kMaximumFilterTurbulenceOctaves) {
        return false;
      }
      const std::uint64_t octaves = static_cast<std::uint64_t>(std::max(turbulence->numOctaves, 1));
      // TinySkia evaluates four lattice gradients and interpolates four color channels for every
      // octave. Geode runs the same octave loop in a compute shader. Charge the CPU scalar work
      // conservatively and retain a lower, still octave-weighted GPU estimate.
      const std::uint64_t workPerOctave =
          memoryModel == FilterMemoryModel::CpuFloatNamedResults ? 16 : 4;
      workMultiplier = octaves * workPerOctave + 5;
    } else if (std::holds_alternative<filter_primitive::Merge>(node.primitive)) {
      if (node.inputs.size() > kMaximumFilterMergeInputs) {
        return false;
      }
      const std::uint64_t mergeInputs = static_cast<std::uint64_t>(node.inputs.size());
      // Linear merge converts and composites each input, then clips the node output.
      workMultiplier = std::max<std::uint64_t>(mergeInputs * 3 + 2, 5);
      totalMergeInputs += mergeInputs;
    } else if (const auto* blur = std::get_if<filter_primitive::GaussianBlur>(&node.primitive)) {
      // Two-axis large-sigma blur uses six convolution passes plus two transposes. Include the
      // node copy and linear-RGB conversions in the conservative full-surface work estimate.
      workMultiplier = blur->stdDeviationX > 0.0 || blur->stdDeviationY > 0.0 ? 12 : 5;
    } else if (const auto* shadow = std::get_if<filter_primitive::DropShadow>(&node.primitive)) {
      // Drop shadow adds alpha extraction, flood, composite, offset, and merge passes around the
      // blur, with additional color conversions in linear RGB.
      workMultiplier = shadow->stdDeviationX > 0.0 || shadow->stdDeviationY > 0.0 ? 20 : 12;
    } else if (const auto* morphology =
                   std::get_if<filter_primitive::Morphology>(&node.primitive)) {
      workMultiplier = morphology->radiusX > 0.0 || morphology->radiusY > 0.0
                           ? kMaximumFilterMorphologyWorkMultiplier
                           : 5;
    } else if (const auto* image = std::get_if<filter_primitive::Image>(&node.primitive)) {
      // TinySkia's default Mitchell sampling visits a 4x4 source footprint for each of four
      // channels. Pixelated sampling and Geode use a single-sample GPU path plus clipping.
      workMultiplier = memoryModel == FilterMemoryModel::CpuFloatNamedResults &&
                               image->imageRendering != ImageRendering::Pixelated
                           ? 68
                           : 5;
    }

    if (workMultiplier > kMaximumFilterWorkUnits ||
        (workMultiplier != 0 && pixelCount > kMaximumFilterWorkUnits / workMultiplier)) {
      return false;
    }
    const std::uint64_t nodeWork = pixelCount * workMultiplier;
    if (nodeWork > kMaximumFilterWorkUnits - workUnits) {
      return false;
    }
    workUnits += nodeWork;
  }

  std::uint64_t retainedBuffers = 0;
  if (memoryModel == FilterMemoryModel::CpuFloatNamedResults) {
    bool usesSourceAlpha = false;
    bool usesFillPaint = false;
    bool usesStrokePaint = false;
    for (const FilterNode& node : graph.nodes) {
      for (const FilterInput& input : node.inputs) {
        if (const auto* standard = std::get_if<FilterStandardInput>(&input.value)) {
          usesSourceAlpha |= *standard == FilterStandardInput::SourceAlpha;
          usesFillPaint |= *standard == FilterStandardInput::FillPaint;
          usesStrokePaint |= *standard == FilterStandardInput::StrokePaint;
        }
      }
    }

    // TinySkia retains SourceGraphic and any materialized standard inputs for the execution. Track
    // the maximum live transient set per node instead of charging every possible optional buffer
    // to every graph: a single blur at the established high-zoom envelope needs only source and
    // output, while multi-node, named-result, merge, and color-conversion graphs retain more.
    const std::uint64_t fixedBuffers = 1 + static_cast<std::uint64_t>(usesSourceAlpha) +
                                       static_cast<std::uint64_t>(usesFillPaint) +
                                       static_cast<std::uint64_t>(usesStrokePaint);
    std::uint64_t priorNamedResults = 0;
    for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
      const FilterNode& node = graph.nodes[index];
      const bool linearRgb =
          node.colorInterpolationFilters.value_or(graph.colorInterpolationFilters) !=
          ColorInterpolationFilters::SRGB;
      std::uint64_t transientBuffers = 1;
      if (std::holds_alternative<filter_primitive::GaussianBlur>(node.primitive)) {
        const bool implicitSource =
            node.inputs.empty() ||
            std::holds_alternative<FilterInput::Previous>(node.inputs.front().value) ||
            (std::holds_alternative<FilterStandardInput>(node.inputs.front().value) &&
             std::get<FilterStandardInput>(node.inputs.front().value) ==
                 FilterStandardInput::SourceGraphic);
        // A sole SourceGraphic blur consumes its float source in place and uses bounded line
        // scratch. A blur inside a larger graph must retain its input and one output buffer.
        transientBuffers = graph.nodes.size() == 1 && implicitSource ? 0 : 1;
      } else if (std::holds_alternative<filter_primitive::Merge>(node.primitive)) {
        transientBuffers = linearRgb ? static_cast<std::uint64_t>(node.inputs.size()) + 1 : 1;
      } else if (std::holds_alternative<filter_primitive::Blend>(node.primitive) ||
                 std::holds_alternative<filter_primitive::Composite>(node.primitive) ||
                 std::holds_alternative<filter_primitive::DisplacementMap>(node.primitive)) {
        transientBuffers = linearRgb ? 3 : 1;
      } else if (std::holds_alternative<filter_primitive::ConvolveMatrix>(node.primitive) ||
                 std::holds_alternative<filter_primitive::Morphology>(node.primitive) ||
                 std::holds_alternative<filter_primitive::DiffuseLighting>(node.primitive) ||
                 std::holds_alternative<filter_primitive::SpecularLighting>(node.primitive)) {
        transientBuffers = linearRgb ? 2 : 1;
      } else if (std::holds_alternative<filter_primitive::DropShadow>(node.primitive)) {
        transientBuffers = linearRgb ? 6 : 5;
      }

      const std::uint64_t previousOutput = index == 0 ? 0 : 1;
      const std::uint64_t namedResultCopy = node.result.has_value() ? 1 : 0;
      retainedBuffers =
          std::max(retainedBuffers, fixedBuffers + previousOutput + priorNamedResults +
                                        transientBuffers + namedResultCopy);
      priorNamedResults += namedResultCopy;
    }
  } else {
    // Geode's arena retains per-node textures and every merge conversion and accumulator until
    // execution completes.
    retainedBuffers = 2 + totalMergeInputs * 2;
    for (const FilterNode& node : graph.nodes) {
      if (std::holds_alternative<filter_primitive::GaussianBlur>(node.primitive)) {
        const bool linearRgb =
            node.colorInterpolationFilters.value_or(graph.colorInterpolationFilters) !=
            ColorInterpolationFilters::SRGB;
        // A two-axis box blur retains up to six pass textures. The default linearRGB path also
        // retains its input and output color-conversion textures, and every node retains its
        // subregion-clipped output in the frame arena.
        retainedBuffers += linearRgb ? 9 : 7;
      } else if (std::holds_alternative<filter_primitive::DropShadow>(node.primitive)) {
        retainedBuffers += 8;
      } else if (const auto* morphology =
                     std::get_if<filter_primitive::Morphology>(&node.primitive);
                 morphology && (morphology->radiusX > 0.0 || morphology->radiusY > 0.0)) {
        retainedBuffers += kMaximumFilterMorphologyRetainedBuffers;
      } else {
        // Linear two-input primitives retain up to four conversion/primitive textures, followed
        // by the mandatory per-node subregion clip.
        retainedBuffers += 5;
      }
    }
  }
  const std::uint64_t bytesPerPixel =
      memoryModel == FilterMemoryModel::CpuFloatNamedResults ? 16 : 4;
  if (retainedBuffers != 0 &&
      (pixelCount > kMaximumFilterIntermediateBytes / bytesPerPixel / retainedBuffers)) {
    return false;
  }

  workUnitsOut = workUnits;
  intermediateBytesOut = pixelCount * bytesPerPixel * retainedBuffers;
  return true;
}

/// Return whether one filter graph fits the execution budget for a pixel surface.
inline bool FilterGraphFitsExecutionBudget(const FilterGraph& graph, std::uint64_t pixelCount,
                                           FilterMemoryModel memoryModel) {
  std::uint64_t workUnits = 0;
  std::uint64_t intermediateBytes = 0;
  return FilterGraphExecutionCost(graph, pixelCount, memoryModel, workUnits, intermediateBytes);
}

/**
 * Shared per-frame filter budget.
 *
 * A graph that is safe in isolation can still exhaust memory or CPU when an SVG repeats it across
 * many elements or `<use>` instances. Renderers reset this object at `beginFrame()` and consume it
 * before every graph execution.
 */
class FilterExecutionBudget {
public:
  enum class RejectionReason : std::uint8_t {
    None,
    InvalidGraph,
    ExecutionLimit,
    WorkLimit,
    MemoryLimit,
    External,
  };

  /// Maximum number of graph executions, including zero-area and otherwise inexpensive graphs.
  static constexpr std::uint64_t kMaximumExecutions = 1024;

  /// Successful preflight reservation made before allocating a filter capture surface.
  struct Reservation {
    std::uint64_t captureBytes = 0;
    FilterMemoryModel memoryModel = FilterMemoryModel::CpuFloatNamedResults;
    bool active = false;
  };

  /// Reset all per-frame accounting.
  void reset() {
    executions_ = 0;
    workUnits_ = 0;
    intermediateBytes_ = 0;
    liveCpuCaptureBytes_ = 0;
    activeGpuReservations_ = 0;
    captureBytesReserved_ = 0;
    rejectionReason_ = RejectionReason::None;
    rejected_ = false;
  }

  /** Release GPU-retained accounting after submitting one ordered command-buffer chunk. */
  bool beginChunkAfterSubmit() {
    if (rejectionReason_ != RejectionReason::MemoryLimit || activeGpuReservations_ != 0) {
      return false;
    }
    intermediateBytes_ = 0;
    liveCpuCaptureBytes_ = 0;
    rejectionReason_ = RejectionReason::None;
    rejected_ = false;
    ++chunks_;
    return true;
  }

  /**
   * Reserve graph execution and capture memory before allocating the capture surface.
   *
   * Rejection latches the frame closed, preventing a document from repeatedly attempting large
   * allocations that were never charged. CPU capture bytes are released at pop; GPU capture and
   * intermediate textures remain charged until frame reset because command buffers retain them.
   */
  std::optional<Reservation> reserve(const FilterGraph& graph, std::uint64_t pixelCount,
                                     FilterMemoryModel memoryModel, std::uint64_t captureBytes) {
    std::uint64_t graphWorkUnits = 0;
    std::uint64_t graphIntermediateBytes = 0;
    if (rejected_) {
      return std::nullopt;
    }
    if (!FilterGraphExecutionCost(graph, pixelCount, memoryModel, graphWorkUnits,
                                  graphIntermediateBytes)) {
      rejectionReason_ = RejectionReason::InvalidGraph;
      rejected_ = true;
      return std::nullopt;
    }

    const bool gpu = memoryModel == FilterMemoryModel::GpuAllNodes;
    const std::uint64_t retainedBeforeExecution = gpu ? intermediateBytes_ : liveCpuCaptureBytes_;
    if (executions_ >= kMaximumExecutions) {
      rejectionReason_ = RejectionReason::ExecutionLimit;
      rejected_ = true;
      return std::nullopt;
    }
    if (workUnits_ > kMaximumFilterFrameWorkUnits ||
        graphWorkUnits > kMaximumFilterFrameWorkUnits - workUnits_) {
      rejectionReason_ = RejectionReason::WorkLimit;
      rejected_ = true;
      return std::nullopt;
    }
    if (retainedBeforeExecution > kMaximumFilterFrameBytes ||
        captureBytes > kMaximumFilterFrameBytes - retainedBeforeExecution ||
        graphIntermediateBytes >
            kMaximumFilterFrameBytes - retainedBeforeExecution - captureBytes) {
      rejectionReason_ = RejectionReason::MemoryLimit;
      rejected_ = true;
      return std::nullopt;
    }

    ++executions_;
    workUnits_ += graphWorkUnits;
    captureBytesReserved_ += captureBytes;
    if (gpu) {
      intermediateBytes_ += captureBytes + graphIntermediateBytes;
      ++activeGpuReservations_;
    } else {
      liveCpuCaptureBytes_ += captureBytes;
    }
    return Reservation{captureBytes, memoryModel, true};
  }

  /// Release a successful CPU capture reservation after its filter layer is popped.
  void release(Reservation& reservation) {
    if (!reservation.active) {
      return;
    }
    if (reservation.memoryModel == FilterMemoryModel::CpuFloatNamedResults) {
      liveCpuCaptureBytes_ -= reservation.captureBytes;
    } else if (activeGpuReservations_ != 0) {
      --activeGpuReservations_;
    }
    reservation.active = false;
  }

  /// Consume a graph at execution time for direct callers without a capture preflight.
  bool consume(const FilterGraph& graph, std::uint64_t pixelCount, FilterMemoryModel memoryModel) {
    std::optional<Reservation> reservation = reserve(graph, pixelCount, memoryModel, 0);
    if (!reservation.has_value()) {
      return false;
    }
    release(*reservation);
    return true;
  }

  /// Latch the frame closed after an allocation or other external preflight failure.
  void reject() {
    rejectionReason_ = RejectionReason::External;
    rejected_ = true;
  }

  [[nodiscard]] std::uint64_t executions() const { return executions_; }
  [[nodiscard]] std::uint64_t workUnits() const { return workUnits_; }
  [[nodiscard]] std::uint64_t retainedBytes() const {
    return intermediateBytes_ + liveCpuCaptureBytes_;
  }
  [[nodiscard]] std::uint64_t captureBytesReserved() const { return captureBytesReserved_; }
  [[nodiscard]] std::uint64_t activeGpuReservations() const { return activeGpuReservations_; }
  /// Ordered GPU chunks submitted since this budget was constructed.
  [[nodiscard]] std::uint64_t chunks() const { return chunks_; }
  [[nodiscard]] bool rejected() const { return rejected_; }
  [[nodiscard]] RejectionReason rejectionReason() const { return rejectionReason_; }

private:
  std::uint64_t executions_ = 0;
  std::uint64_t workUnits_ = 0;
  std::uint64_t intermediateBytes_ = 0;
  std::uint64_t liveCpuCaptureBytes_ = 0;
  std::uint64_t activeGpuReservations_ = 0;
  std::uint64_t captureBytesReserved_ = 0;
  std::uint64_t chunks_ = 0;
  RejectionReason rejectionReason_ = RejectionReason::None;
  bool rejected_ = false;
};

}  // namespace donner::svg::components
