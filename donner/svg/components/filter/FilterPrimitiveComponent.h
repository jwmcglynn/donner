#pragma once
/// @file

#include <optional>

#include "donner/base/EcsRegistry.h"
#include "donner/base/Length.h"
#include "donner/base/RcString.h"
#include "donner/css/Color.h"
#include "donner/svg/components/filter/FilterGraph.h"
#include "donner/svg/core/PreserveAspectRatio.h"
#include "donner/svg/properties/Property.h"
#include "donner/svg/properties/PropertyParsing.h"

namespace donner::svg::components {

/**
 * Parameters for \ref SVGFilterPrimitiveStandardAttributes.
 */
struct FilterPrimitiveComponent {
  std::optional<Lengthd> x;       ///< The x-coordinate of the filter region.
  std::optional<Lengthd> y;       ///< The y-coordinate of the filter region.
  std::optional<Lengthd> width;   ///< The width of the filter region.
  std::optional<Lengthd> height;  ///< The height of the filter region.

  /// Name of the filter primitive, which enables it to be used as a reference for subsequent filter
  /// primitives under the same \ref xml_filter element.
  std::optional<RcString> result;

  /// The input specification for this filter primitive. Parsed from the `in` attribute.
  /// When nullopt, defaults to Previous (output of preceding primitive, or SourceGraphic for the
  /// first).
  std::optional<FilterInput> in;

  /// The second input specification for two-input primitives (e.g., feComposite, feBlend).
  /// Parsed from the `in2` attribute.
  std::optional<FilterInput> in2;
};

/**
 * Parameters for \ref SVGFEGaussianBlurElement.
 */
struct FEGaussianBlurComponent {
  /// Edge handling mode for the blur.
  enum class EdgeMode : std::uint8_t {
    None,       ///< Treat out-of-bounds pixels as transparent black.
    Duplicate,  ///< Clamp to nearest edge pixel.
    Wrap,       ///< Wrap around (modular arithmetic).
  };

  double stdDeviationX = 0.0;  ///< The standard deviation of the Gaussian blur in the x direction.
  double stdDeviationY = 0.0;  ///< The standard deviation of the Gaussian blur in the y direction.
  EdgeMode edgeMode = EdgeMode::None;  ///< Edge handling mode (default: none).
};

/**
 * Parameters for \ref SVGFEFloodElement.
 */
struct FEFloodComponent {
  /// The flood fill color, defaults to black.
  Property<css::Color> floodColor{"flood-color", []() -> std::optional<css::Color> {
                                    return css::Color(css::RGBA(0, 0, 0, 0xFF));
                                  }};

  /// The flood fill opacity, defaults to 1. Range is [0, 1].
  Property<double> floodOpacity{"flood-opacity",
                                []() -> std::optional<double> { return 1.0; }};
};

/**
 * Parameters for \ref SVGFEOffsetElement.
 */
struct FEOffsetComponent {
  double dx = 0.0;  ///< The horizontal offset.
  double dy = 0.0;  ///< The vertical offset.
};

/**
 * Parameters for \ref SVGFECompositeElement.
 */
struct FECompositeComponent {
  /// Porter-Duff compositing operator.
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

/**
 * Parameters for \ref SVGFEColorMatrixElement.
 */
struct FEColorMatrixComponent {
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

/**
 * Parameters for \ref SVGFEBlendElement.
 */
struct FEBlendComponent {
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

/**
 * Parameters for \ref SVGFEDropShadowElement.
 */
struct FEDropShadowComponent {
  double dx = 2.0;               ///< Horizontal offset.
  double dy = 2.0;               ///< Vertical offset.
  double stdDeviationX = 2.0;    ///< Blur standard deviation in X.
  double stdDeviationY = 2.0;    ///< Blur standard deviation in Y.

  /// The flood fill color, defaults to black.
  Property<css::Color> floodColor{"flood-color", []() -> std::optional<css::Color> {
                                    return css::Color(css::RGBA(0, 0, 0, 0xFF));
                                  }};

  /// The flood fill opacity, defaults to 1.
  Property<double> floodOpacity{"flood-opacity",
                                []() -> std::optional<double> { return 1.0; }};
};

/**
 * Marker component for \ref SVGFEComponentTransferElement.
 *
 * The element itself has no parameters; its children (feFuncR/G/B/A) define the transfer
 * functions for each channel.
 */
struct FEComponentTransferComponent {
  int placeholder = 0;  ///< Placeholder to avoid empty struct issues with entt.
};

/**
 * Parameters for a feFuncR/G/B/A child element within feComponentTransfer.
 *
 * Each func element defines a transfer function for one RGBA channel.
 */
struct FEFuncComponent {
  /// Which channel this function applies to.
  enum class Channel : std::uint8_t { R, G, B, A };

  /// Transfer function type.
  enum class FuncType : std::uint8_t {
    Identity,   ///< No change.
    Table,      ///< Piecewise linear lookup.
    Discrete,   ///< Step function lookup.
    Linear,     ///< slope * C + intercept.
    Gamma,      ///< amplitude * pow(C, exponent) + offset.
  };

  Channel channel;                       ///< Which channel this applies to.
  FuncType type = FuncType::Identity;    ///< Transfer function type.
  std::vector<double> tableValues;       ///< Values for table/discrete types.
  double slope = 1.0;                    ///< Slope for linear type.
  double intercept = 0.0;               ///< Intercept for linear type.
  double amplitude = 1.0;               ///< Amplitude for gamma type.
  double exponent = 1.0;                ///< Exponent for gamma type.
  double offset = 0.0;                  ///< Offset for gamma type.

  explicit FEFuncComponent(Channel ch) : channel(ch) {}
};

/**
 * Parameters for \ref SVGFEConvolveMatrixElement.
 */
struct FEConvolveMatrixComponent {
  /// Edge mode for out-of-bounds pixels.
  enum class EdgeMode : std::uint8_t {
    Duplicate,  ///< Clamp to nearest edge pixel.
    Wrap,       ///< Wrap around (modular arithmetic).
    None,       ///< Treat as transparent black.
  };

  int orderX = 3;                            ///< Kernel width.
  int orderY = 3;                            ///< Kernel height.
  std::vector<double> kernelMatrix;          ///< Kernel values (orderX * orderY).
  std::optional<double> divisor;               ///< Divisor (nullopt = sum of kernel values).
  double bias = 0.0;                         ///< Bias added to result.
  std::optional<int> targetX;                ///< Target X (nullopt = floor(orderX/2)).
  std::optional<int> targetY;                ///< Target Y (nullopt = floor(orderY/2)).
  EdgeMode edgeMode = EdgeMode::Duplicate;   ///< Edge handling mode.
  bool preserveAlpha = false;                ///< If true, only filter RGB channels.
};

/**
 * Parameters for \ref SVGFEMorphologyElement.
 */
struct FEMorphologyComponent {
  /// Morphology operator.
  enum class Operator : std::uint8_t {
    Erode,   ///< Per-channel minimum in the window.
    Dilate,  ///< Per-channel maximum in the window.
  };

  Operator op = Operator::Erode;  ///< Erode or dilate.
  double radiusX = 0.0;           ///< Horizontal radius.
  double radiusY = 0.0;           ///< Vertical radius.
};

/**
 * Marker component for \ref SVGFETileElement.
 *
 * The feTile element has no element-specific parameters. It tiles its input's content
 * (within the input's primitive subregion) across its own primitive subregion.
 */
struct FETileComponent {
  int placeholder = 0;  ///< Placeholder to avoid empty struct issues with entt.
};

/**
 * Parameters for \ref SVGFEImageElement.
 */
struct FEImageComponent {
  RcString href;  ///< Image URL or fragment reference.
  PreserveAspectRatio preserveAspectRatio =
      PreserveAspectRatio::Default();  ///< How to fit the image.
};

/**
 * Parameters for \ref SVGFEDisplacementMapElement.
 */
struct FEDisplacementMapComponent {
  /// Channel selector.
  enum class Channel : std::uint8_t { R, G, B, A };

  double scale = 0.0;                     ///< Displacement scale factor.
  Channel xChannelSelector = Channel::A;  ///< Channel to use for X displacement.
  Channel yChannelSelector = Channel::A;  ///< Channel to use for Y displacement.
};

/**
 * Parameters for \ref SVGFETurbulenceElement.
 */
struct FETurbulenceComponent {
  /// Noise type.
  enum class Type : std::uint8_t {
    FractalNoise,  ///< Fractal Brownian motion.
    Turbulence,    ///< Turbulence (absolute value noise).
  };

  Type type = Type::Turbulence;     ///< Noise type.
  double baseFrequencyX = 0.0;     ///< Base frequency X.
  double baseFrequencyY = 0.0;     ///< Base frequency Y.
  int numOctaves = 1;              ///< Number of octaves.
  double seed = 0.0;               ///< Random seed.
  bool stitchTiles = false;        ///< Whether to stitch tiles.
};

/**
 * Marker component for \ref SVGFEMergeElement.
 *
 * The merge element itself has no parameters; its children (feMergeNode) specify the inputs.
 */
struct FEMergeComponent {
  int placeholder = 0;  ///< Placeholder to avoid empty struct issues with entt.
};

/**
 * Parameters for a feMergeNode child element within feMerge.
 *
 * Each merge node specifies an input via the `in` attribute. The inputs are composited
 * bottom-to-top using Source Over.
 */
struct FEMergeNodeComponent {
  /// The input to this merge node. Parsed from the `in` attribute.
  std::optional<FilterInput> in;
};

/**
 * Light source parameters, stored on feDistantLight, fePointLight, or feSpotLight child elements.
 */
struct LightSourceComponent {
  /// Light source type.
  enum class Type : std::uint8_t { Distant, Point, Spot };

  Type type;

  // feDistantLight attributes.
  double azimuth = 0.0;     ///< Angle in the XY plane (degrees).
  double elevation = 0.0;   ///< Angle above the XY plane (degrees).

  // fePointLight / feSpotLight attributes.
  double x = 0.0;  ///< X position.
  double y = 0.0;  ///< Y position.
  double z = 0.0;  ///< Z position.

  // feSpotLight attributes.
  double pointsAtX = 0.0;   ///< X target.
  double pointsAtY = 0.0;   ///< Y target.
  double pointsAtZ = 0.0;   ///< Z target.
  double spotExponent = 1.0; ///< Spotlight exponent (falloff).
  std::optional<double> limitingConeAngle;  ///< Cone angle limit (degrees).

  explicit LightSourceComponent(Type t) : type(t) {}
};

/**
 * Parameters for \ref SVGFEDiffuseLightingElement.
 */
struct FEDiffuseLightingComponent {
  double surfaceScale = 1.0;      ///< Height of surface when alpha=1.
  double diffuseConstant = 1.0;   ///< Diffuse reflection constant (kd).

  /// `lighting-color` property, specifying the color of the light source (default: white).
  Property<css::Color> lightingColor{"lighting-color", []() -> std::optional<css::Color> {
    return css::Color(css::RGBA(0xFF, 0xFF, 0xFF, 0xFF));
  }};
};

/**
 * Parameters for \ref SVGFESpecularLightingElement.
 */
struct FESpecularLightingComponent {
  double surfaceScale = 1.0;       ///< Height of surface when alpha=1.
  double specularConstant = 1.0;   ///< Specular reflection constant (ks).
  double specularExponent = 1.0;   ///< Specular exponent (1..128).

  /// `lighting-color` property, specifying the color of the light source (default: white).
  Property<css::Color> lightingColor{"lighting-color", []() -> std::optional<css::Color> {
    return css::Color(css::RGBA(0xFF, 0xFF, 0xFF, 0xFF));
  }};
};

// Forward declare for presentation attribute parsing helpers.
ParseResult<bool> ParseFeFloodPresentationAttribute(EntityHandle handle, std::string_view name,
                                                    const parser::PropertyParseFnParams& params);
ParseResult<bool> ParseFeDropShadowPresentationAttribute(
    EntityHandle handle, std::string_view name, const parser::PropertyParseFnParams& params);
ParseResult<bool> ParseFeDiffuseLightingPresentationAttribute(
    EntityHandle handle, std::string_view name, const parser::PropertyParseFnParams& params);
ParseResult<bool> ParseFeSpecularLightingPresentationAttribute(
    EntityHandle handle, std::string_view name, const parser::PropertyParseFnParams& params);

}  // namespace donner::svg::components
