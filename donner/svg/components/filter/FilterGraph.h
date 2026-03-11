#pragma once
/// @file

#include <any>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/Length.h"
#include "donner/base/RcString.h"
#include "donner/css/Color.h"
#include "donner/svg/components/filter/FilterUnits.h"
#include "donner/svg/core/PreserveAspectRatio.h"

namespace donner::svg::components {

/**
 * Standard named inputs available to filter primitives.
 *
 * @see https://drafts.fxtf.org/filter-effects/#FilterPrimitiveSubRegion
 */
enum class FilterStandardInput : std::uint8_t {
  SourceGraphic,   ///< The original element rendering.
  SourceAlpha,     ///< Alpha channel of SourceGraphic (RGB = 0).
  FillPaint,       ///< The element's fill paint, conceptually infinite.
  StrokePaint,     ///< The element's stroke paint, conceptually infinite.
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

  double stdDeviationX = 0.0;  ///< Standard deviation in X.
  double stdDeviationY = 0.0;  ///< Standard deviation in Y.
  EdgeMode edgeMode = EdgeMode::None;  ///< Edge handling mode.
};

/// Parameters for \c feFlood.
struct Flood {
  css::Color floodColor{css::RGBA(0, 0, 0, 0xFF)};  ///< Flood color (default: black).
  double floodOpacity = 1.0;                          ///< Flood opacity (default: 1).
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
  double k1 = 0.0;              ///< Arithmetic coefficient k1.
  double k2 = 0.0;              ///< Arithmetic coefficient k2.
  double k3 = 0.0;              ///< Arithmetic coefficient k3.
  double k4 = 0.0;              ///< Arithmetic coefficient k4.
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

  Type type = Type::Matrix;      ///< Matrix type.
  std::vector<double> values;    ///< Matrix values (interpretation depends on type).
};

/// Parameters for \c feMerge. Children are represented as additional inputs on the FilterNode.
struct Merge {};

/// Parameters for \c feDropShadow.
struct DropShadow {
  double dx = 2.0;              ///< Horizontal offset.
  double dy = 2.0;              ///< Vertical offset.
  double stdDeviationX = 2.0;   ///< Blur standard deviation X.
  double stdDeviationY = 2.0;   ///< Blur standard deviation Y.
  css::Color floodColor{css::RGBA(0, 0, 0, 0xFF)};  ///< Shadow color (default: black).
  double floodOpacity = 1.0;    ///< Shadow opacity (default: 1).
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
    double intercept = 0.0;             ///< Intercept (for linear).
    double amplitude = 1.0;             ///< Amplitude (for gamma).
    double exponent = 1.0;              ///< Exponent (for gamma).
    double offset = 0.0;               ///< Offset (for gamma).
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

  int orderX = 3;                        ///< Kernel width.
  int orderY = 3;                        ///< Kernel height.
  std::vector<double> kernelMatrix;      ///< Kernel values (orderX * orderY).
  std::optional<double> divisor;          ///< Divisor (nullopt = sum of kernel values).
  double bias = 0.0;                     ///< Bias added to result.
  std::optional<int> targetX;            ///< Target X (nullopt = floor(orderX/2)).
  std::optional<int> targetY;            ///< Target Y (nullopt = floor(orderY/2)).
  EdgeMode edgeMode = EdgeMode::Duplicate;  ///< Edge handling mode.
  bool preserveAlpha = false;            ///< If true, only filter RGB channels.
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

  Type type = Type::Turbulence;    ///< Noise type.
  double baseFrequencyX = 0.0;    ///< Base frequency X.
  double baseFrequencyY = 0.0;    ///< Base frequency Y.
  int numOctaves = 1;             ///< Number of octaves.
  double seed = 0.0;              ///< Random seed.
  bool stitchTiles = false;       ///< Whether to stitch tiles.
};

/// Parameters for \c feImage.
struct Image {
  RcString href;  ///< Image URL or fragment reference.
  PreserveAspectRatio preserveAspectRatio = PreserveAspectRatio::Default();

  /// Loaded image data (RGBA, straight alpha). Empty if loading failed or href is a fragment.
  std::vector<uint8_t> imageData;
  int imageWidth = 0;   ///< Width of loaded image in pixels.
  int imageHeight = 0;  ///< Height of loaded image in pixels.

  /// Non-owning pointer to an SVG sub-document (`std::any` containing `SVGDocument`), set when
  /// the feImage href references an external SVG file. The renderer pre-renders this to pixel data
  /// before filter execution. Owned by \ref SubDocumentCache.
  std::any* svgSubDocument = nullptr;
};

/// Parameters for \c feDisplacementMap.
struct DisplacementMap {
  /// Channel selector.
  enum class Channel : std::uint8_t { R, G, B, A };

  double scale = 0.0;                   ///< Displacement scale factor.
  Channel xChannelSelector = Channel::A;  ///< Channel to use for X displacement.
  Channel yChannelSelector = Channel::A;  ///< Channel to use for Y displacement.
};

/// Light source parameters for lighting filter primitives.
struct LightSource {
  /// Light source type.
  enum class Type : std::uint8_t { Distant, Point, Spot };

  Type type = Type::Distant;

  // feDistantLight
  double azimuth = 0.0;     ///< Angle in the XY plane (degrees).
  double elevation = 0.0;   ///< Angle above the XY plane (degrees).

  // fePointLight / feSpotLight
  double x = 0.0;  ///< X position.
  double y = 0.0;  ///< Y position.
  double z = 0.0;  ///< Z position.

  // feSpotLight
  double pointsAtX = 0.0;   ///< X target.
  double pointsAtY = 0.0;   ///< Y target.
  double pointsAtZ = 0.0;   ///< Z target.
  double spotExponent = 1.0; ///< Spotlight exponent.
  std::optional<double> limitingConeAngle;  ///< Cone angle limit (degrees).
};

/// Parameters for \c feDiffuseLighting.
struct DiffuseLighting {
  double surfaceScale = 1.0;       ///< Height of surface.
  double diffuseConstant = 1.0;    ///< Diffuse reflection constant.
  css::Color lightingColor{css::RGBA(0xFF, 0xFF, 0xFF, 0xFF)};  ///< Light color (default: white).
  std::optional<LightSource> light;  ///< Light source (from child element).
};

/// Parameters for \c feSpecularLighting.
struct SpecularLighting {
  double surfaceScale = 1.0;        ///< Height of surface.
  double specularConstant = 1.0;    ///< Specular reflection constant.
  double specularExponent = 1.0;    ///< Specular exponent (1..128).
  css::Color lightingColor{css::RGBA(0xFF, 0xFF, 0xFF, 0xFF)};  ///< Light color (default: white).
  std::optional<LightSource> light;  ///< Light source (from child element).
};

}  // namespace filter_primitive

/**
 * Variant holding any filter primitive type.
 */
using FilterPrimitive = std::variant<
    filter_primitive::GaussianBlur,
    filter_primitive::Flood,
    filter_primitive::Offset,
    filter_primitive::Merge,
    filter_primitive::Blend,
    filter_primitive::Composite,
    filter_primitive::ColorMatrix,
    filter_primitive::DropShadow,
    filter_primitive::ComponentTransfer,
    filter_primitive::ConvolveMatrix,
    filter_primitive::Morphology,
    filter_primitive::Tile,
    filter_primitive::Turbulence,
    filter_primitive::Image,
    filter_primitive::DisplacementMap,
    filter_primitive::DiffuseLighting,
    filter_primitive::SpecularLighting>;

/**
 * A single node in the filter graph, representing one filter primitive.
 *
 * Nodes are executed in document order. Each node reads from its inputs (which may be
 * standard inputs like SourceGraphic, or outputs of prior nodes), applies its primitive
 * operation, and writes to an output buffer.
 */
struct FilterNode {
  FilterPrimitive primitive;           ///< The filter primitive operation.
  std::vector<FilterInput> inputs;     ///< Input(s) to this primitive.
  std::optional<RcString> result;      ///< Named output (for `result` attribute).
  std::optional<Lengthd> x;           ///< Primitive subregion X.
  std::optional<Lengthd> y;           ///< Primitive subregion Y.
  std::optional<Lengthd> width;       ///< Primitive subregion width.
  std::optional<Lengthd> height;      ///< Primitive subregion height.

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
  std::optional<Boxd> elementBoundingBox;

  /// The filter region in user-space coordinates.
  std::optional<Boxd> filterRegion;

  /// Scale factor from SVG user-space coordinates to pixel-space coordinates.
  /// Needed by lighting filters to transform light positions from user space to the pixel-space
  /// pixmap. Set by the renderer driver from the viewBox and canvas dimensions.
  Vector2d userToPixelScale = Vector2d(1.0, 1.0);

  /// Returns true if the graph has no nodes.
  [[nodiscard]] bool empty() const { return nodes.empty(); }
};

}  // namespace donner::svg::components
