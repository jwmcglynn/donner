#pragma once
/// @file

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/EcsRegistry_fwd.h"
#include "donner/base/Length.h"
#include "donner/base/RcString.h"
#include "donner/base/Transform.h"
#include "donner/css/Color.h"
#include "donner/svg/SVGDocumentHandle.h"
#include "donner/svg/components/animation/ContextualLengthRecipe.h"
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
/// Bound for aggregate morphology work after splitting a large radius into shader passes.
inline constexpr std::uint64_t kMaximumFilterMorphologyWorkMultiplier =
    2 * (2 * kMaximumFilterPixelRadius + kMaximumFilterMorphologyPassesPerAxis) + 5;
/// Bound for temporary buffers retained by a multi-pass morphology execution.
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
/// Maximum number of coefficients accepted by an `feConvolveMatrix` kernel.
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
  struct Previous {
    /// Equality operator.
    bool operator==(const Previous&) const = default;
  };

  /// The input is a named result from a prior primitive's `result` attribute.
  struct Named {
    RcString name;  ///< The result name to reference.

    /// Equality operator.
    bool operator==(const Named&) const = default;
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

  /// Parse a standard keyword or a named filter-result reference.
  /// @param value SVG input attribute value; the caller admits any owning string allocation.
  static FilterInput FromAttribute(std::string_view value) {
    if (value == "SourceGraphic") {
      return FilterInput(FilterStandardInput::SourceGraphic);
    }
    if (value == "SourceAlpha") {
      return FilterInput(FilterStandardInput::SourceAlpha);
    }
    if (value == "FillPaint") {
      return FilterInput(FilterStandardInput::FillPaint);
    }
    if (value == "StrokePaint") {
      return FilterInput(FilterStandardInput::StrokePaint);
    }
    return FilterInput(Named{RcString(value)});
  }

  /// Equality operator.
  bool operator==(const FilterInput&) const = default;
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

/**
 * Sum an admitted convolution kernel with compensation for cancellation.
 *
 * @param kernel Coefficients admitted by the caller's order and resource checks.
 * @return Compensated sum, or the nonfinite intermediate when accumulation overflows.
 */
inline double ConvolveKernelSum(std::span<const double> kernel) {
  double sum = 0.0;
  double correction = 0.0;
  for (const double coefficient : kernel) {
    const double updated = sum + coefficient;
    if (!std::isfinite(updated)) {
      return updated;
    }
    correction += std::abs(sum) >= std::abs(coefficient) ? (sum - updated) + coefficient
                                                         : (coefficient - updated) + sum;
    sum = updated;
  }
  return sum + correction;
}

namespace detail {

/// Parses a scientific exponent, including the optional positive sign.
/// @param begin First exponent character. @param end One past the exponent.
/// @param power Receives the parsed exponent. @return Whether every character was parsed.
inline bool ParseConvolveDecimalExponent(const char* begin, const char* end, int& power) {
  if (begin != end && *begin == '+') {
    ++begin;
  }
  const auto exponent = std::from_chars(begin, end, power);
  return exponent.ec == std::errc{} && exponent.ptr == end;
}

/// Adds signed significand digits to fixed decimal bins without rounding.
/// @param spelling Signless significand, optionally containing a decimal point.
/// @param power Power of the first digit. @param sign Coefficient sign, either -1 or 1.
/// @param bins Fixed sum storage. @param lowestPower Power represented by the first bin.
/// @return Whether all digits and powers were supported.
inline bool AddConvolveDecimalDigits(std::span<const char> spelling, int power, int sign,
                                     std::span<std::int32_t> bins, int lowestPower) {
  const int highestPower = lowestPower + static_cast<int>(bins.size()) - 1;
  for (const char digit : spelling) {
    if (digit == '.') {
      continue;
    }
    if (digit < '0' || digit > '9' || power < lowestPower || power > highestPower) {
      return false;
    }
    bins[power-- - lowestPower] += sign * (digit - '0');
  }
  return true;
}

/// Adds the shortest round-trip decimal spelling of one finite coefficient.
/// @param coefficient Value to accumulate. @param bins Fixed decimal sum storage.
/// @param lowestPower Power represented by the first bin.
/// @return Whether conversion and accumulation succeeded; false for nonfinite input.
inline bool AddConvolveDecimalCoefficient(double coefficient, std::span<std::int32_t> bins,
                                          int lowestPower) {
  if (!std::isfinite(coefficient)) {
    return false;
  }
  if (coefficient == 0.0) {
    return true;
  }
  std::array<char, 32> spelling{};
  const auto converted = std::to_chars(spelling.data(), spelling.data() + spelling.size(),
                                       coefficient, std::chars_format::scientific);
  if (converted.ec != std::errc{}) {
    return false;
  }
  const char* const exponentMarker = std::find(spelling.data(), converted.ptr, 'e');
  if (exponentMarker == converted.ptr) {
    return false;
  }
  int power = 0;
  if (!ParseConvolveDecimalExponent(exponentMarker + 1, converted.ptr, power)) {
    return false;
  }
  const int sign = coefficient < 0.0 ? -1 : 1;
  const char* firstDigit = spelling.data() + (sign < 0 ? 1 : 0);
  return AddConvolveDecimalDigits(std::span(firstDigit, exponentMarker), power, sign, bins,
                                  lowestPower);
}

}  // namespace detail

/**
 * Tests exact cancellation of the coefficients' shortest round-trip decimal spellings.
 *
 * Original lexical precision is unavailable after parsing. This decimal interpretation recognizes
 * authored kernels such as 0.1 + 0.2 - 0.3 without discarding genuine small residual coefficients.
 * @param kernel Finite coefficients within the admitted kernel-size limit.
 * @return Whether their decimal sum is exactly zero; false for unsupported input.
 */
inline bool ConvolveKernelHasDecimalZeroSum(std::span<const double> kernel) {
  if (kernel.size() > kMaximumFilterKernelValues) {
    return false;
  }
  // Binary64 scientific spellings fit these powers, including 17 digits at exponent -324.
  constexpr int kLowestPower =
      std::numeric_limits<double>::min_exponent10 - 2 * std::numeric_limits<double>::max_digits10;
  constexpr int kHighestPower = std::numeric_limits<double>::max_exponent10 + 5;
  std::array<std::int32_t, kHighestPower - kLowestPower + 1> digits{};
  for (const double coefficient : kernel) {
    if (!detail::AddConvolveDecimalCoefficient(coefficient, digits, kLowestPower)) {
      return false;
    }
  }
  // Each bin is at most 9*4096 before carrying; signed carries add less than 4096.
  for (std::size_t index = 0; index + 1 < digits.size(); ++index) {
    if (digits[index] % 10 != 0) {
      return false;
    }
    digits[index + 1] += digits[index] / 10;
  }
  return digits.back() == 0;
}

/**
 * Resolves an omitted convolution divisor using decimal-zero fallback and compensated summation.
 * @param kernel Coefficients admitted by the caller's order and resource checks.
 * Nonfinite accumulation is preserved for the caller's existing rejection path.
 * @return One for a recognized zero-sum kernel; otherwise the compensated binary sum.
 */
inline double ConvolveKernelDefaultDivisor(std::span<const double> kernel) {
  const double sum = ConvolveKernelSum(kernel);
  return sum == 0.0 || (std::isfinite(sum) && ConvolveKernelHasDecimalZeroSum(kernel)) ? 1.0 : sum;
}

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
  PreserveAspectRatio preserveAspectRatio =
      PreserveAspectRatio::Default();  ///< Image aspect ratio.

  /// Shared loaded image data (RGBA, straight alpha). Null if loading failed or href is a fragment.
  ///
  /// Prepared filter graphs are copied once per rendering instance. Sharing immutable decoded
  /// pixels prevents one referenced raster from being duplicated for every filtered element.
  std::shared_ptr<const std::vector<uint8_t>> imageData;
  int imageWidth = 0;   ///< Width of loaded image in pixels.
  int imageHeight = 0;  ///< Height of loaded image in pixels.

  /// Whether a nonempty decoded raster payload is available.
  [[nodiscard]] bool hasImageData() const { return imageData && !imageData->empty(); }
  /// Size of the decoded raster payload, or zero when no payload is loaded.
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

  /// Filter-region origin in user-space coordinates, supplied by the renderer driver. Fragment
  /// references use this as a placement fallback when primitive-subregion attributes are absent;
  /// otherwise the resolved primitive subregion supplies the placement origin.
  Vector2d fragmentRegionTopLeft;

  /// For fragment references, the transform from fragment raster pixels to the referenced
  /// content's user space, supplied by the renderer driver with the raster. The raster covers the
  /// placed primitive subregion rather than the canvas, so its first pixel is generally not at
  /// the user-space origin. When absent, the raster starts at the user-space origin at the filter
  /// graph's user-to-pixel scale.
  std::optional<Transform2d> fragmentUserFromPixel;

  /// Transform from fragment raster pixels to the referenced content's user space.
  /// @param userToPixelScale The filter graph's user-to-pixel scale, used when the driver did not
  /// supply \ref fragmentUserFromPixel.
  [[nodiscard]] Transform2d resolvedFragmentUserFromPixel(const Vector2d& userToPixelScale) const {
    if (fragmentUserFromPixel.has_value()) {
      return *fragmentUserFromPixel;
    }
    return Transform2d::Scale(userToPixelScale.x > 0.0 ? 1.0 / userToPixelScale.x : 1.0,
                              userToPixelScale.y > 0.0 ? 1.0 / userToPixelScale.y : 1.0);
  }

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

  /// Immutable deferred x/y/width/height stacks, evaluated in the executing filter's context.
  std::array<std::shared_ptr<const ContextualLengthRecipe>, 4> animatedRegion{};

  /// Whether any authored primitive-region coordinate is present.
  bool hasExplicitRegion() const { return x || y || width || height; }

  /// Whether a contextual region stack is present, including a stack with no contributing value.
  bool hasAnimatedRegion() const {
    return std::any_of(animatedRegion.begin(), animatedRegion.end(),
                       [](const auto& recipe) { return recipe != nullptr; });
  }

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

  /// Referencing SVG viewport in user coordinates, independent of filter raster allocation.
  std::optional<Box2d> primitiveViewport;

  /// Scale factor from SVG user-space coordinates to pixel-space coordinates.
  /// Needed by lighting filters to transform light positions from user space to the pixel-space
  /// pixmap. Set by the renderer driver from the viewBox and canvas dimensions.
  Vector2d userToPixelScale = Vector2d(1.0, 1.0);

  /// Returns true if the graph has no nodes.
  [[nodiscard]] bool empty() const { return nodes.empty(); }
};

/// Conservative resource layout for the live float filter execution strategy.
struct GpuFilterWorkingSet {
  std::uint64_t floatTextures = 0;
  std::uint64_t imageBytes = 0;
  std::uint64_t uniformBytes = 0;
  std::uint64_t runtimeUniformBytes = 0;
  std::uint64_t standaloneBufferBytes = 0;
};

/// Minimum allocation of each persistent parameter arena.
inline constexpr std::uint64_t kGpuFilterParameterBlockBytes = 64 * 1024;
/// Two 4096-entry float transfer tables retained by the filter engine.
inline constexpr std::uint64_t kGpuFilterTransferTableBytes = 2 * 4096 * sizeof(float);

/// Bounds new fixed-block allocations, including a partially filled starting block.
/// @param requested Conservative byte demand including aligned-slot padding.
inline std::uint64_t GpuFilterParameterAllocationBound(std::uint64_t requested) {
  if (!requested) {
    return 0;
  }
  // Slots occupy at most half a block, so at least half of every abandoned block was used.
  if (requested > (kMaximumFilterFrameBytes - kGpuFilterParameterBlockBytes) / 2) {
    return kMaximumFilterFrameBytes + 1;
  }
  return requested * 2 + kGpuFilterParameterBlockBytes;
}

/// Derives a byte-format-aware bound without allocating or changing the CPU filter model.
/// @param graph Ordered filter graph. @param result Receives the resource layout on success.
inline bool ComputeGpuFilterWorkingSet(const FilterGraph& graph, GpuFilterWorkingSet& result) {
  if (graph.nodes.size() > kMaximumFilterGraphNodes) {
    return false;
  }
  result = {};
  std::array<std::size_t, kMaximumFilterGraphNodes> lastUse{};
  std::array<std::size_t, kMaximumFilterGraphNodes> nextDefinition{};
  nextDefinition.fill(graph.nodes.size());
  bool sourceAlpha = false;
  for (std::size_t producer = 0; producer < graph.nodes.size(); ++producer) {
    const auto& name = graph.nodes[producer].result;
    for (const FilterInput& input : graph.nodes[producer].inputs) {
      const auto* standard = std::get_if<FilterStandardInput>(&input.value);
      sourceAlpha |= standard && *standard == FilterStandardInput::SourceAlpha;
    }
    if (!name) {
      continue;
    }
    for (std::size_t consumer = producer + 1; consumer < graph.nodes.size(); ++consumer) {
      const FilterNode& node = graph.nodes[consumer];
      if (node.result == name && nextDefinition[producer] == graph.nodes.size()) {
        nextDefinition[producer] = consumer;
      }
      for (const FilterInput& input : node.inputs) {
        if (const auto* named = std::get_if<FilterInput::Named>(&input.value);
            named && named->name == *name) {
          lastUse[producer] = consumer;
        }
      }
    }
  }

  namespace fp = filter_primitive;
  for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
    const FilterNode& node = graph.nodes[index];
    std::uint64_t namedValues = 0;
    for (std::size_t prior = 0; prior < index; ++prior) {
      namedValues +=
          graph.nodes[prior].result && lastUse[prior] >= index && nextDefinition[prior] >= index;
    }
    std::uint64_t scratch = 2;  // Primitive output and node clip.
    if (std::holds_alternative<fp::GaussianBlur>(node.primitive) ||
        std::holds_alternative<fp::Morphology>(node.primitive)) {
      scratch = 3;  // Two ping-pong textures and a possible separate clip.
    } else if (std::holds_alternative<fp::DropShadow>(node.primitive)) {
      scratch = 4;  // Blur pair, shadow composition, and clip.
    } else if (std::holds_alternative<fp::Merge>(node.primitive)) {
      if (node.inputs.size() > kMaximumFilterMergeInputs) {
        return false;
      }
      scratch = std::max<std::uint64_t>(2, node.inputs.size());
    }
    // SourceGraphic may need one float representation; SourceAlpha and retained logical values
    // may need both color representations. The previous output can also be unnamed.
    result.floatTextures =
        std::max(result.floatTextures,
                 1 + (sourceAlpha ? 2u : 0u) + namedValues * 2 + (index ? 2u : 0u) + scratch);
    result.runtimeUniformBytes += 8192;
    const bool rawUniform = std::holds_alternative<fp::GaussianBlur>(node.primitive) ||
                            std::holds_alternative<fp::Blend>(node.primitive) ||
                            std::holds_alternative<fp::Morphology>(node.primitive) ||
                            std::holds_alternative<fp::ComponentTransfer>(node.primitive) ||
                            std::holds_alternative<fp::DisplacementMap>(node.primitive) ||
                            std::holds_alternative<fp::DropShadow>(node.primitive) ||
                            std::holds_alternative<fp::Image>(node.primitive) ||
                            std::holds_alternative<fp::Tile>(node.primitive);
    if (rawUniform) {
      result.uniformBytes += 8192;
    }
    if (const auto* transfer = std::get_if<fp::ComponentTransfer>(&node.primitive)) {
      for (const auto* function :
           {&transfer->funcR, &transfer->funcG, &transfer->funcB, &transfer->funcA}) {
        if (function->tableValues.size() > kMaximumFilterTableValues) {
          return false;
        }
        result.uniformBytes += function->tableValues.size() * sizeof(float) * 2;
      }
    }
    if (const auto* image = std::get_if<fp::Image>(&node.primitive)) {
      const std::uint64_t bytes = std::max<std::uint64_t>(4, image->imageDataSize());
      if (bytes > kMaximumFilterIntermediateBytes - result.imageBytes) {
        return false;
      }
      result.imageBytes += bytes;
    }
    if (std::holds_alternative<fp::Turbulence>(node.primitive)) {
      result.standaloneBufferBytes += 64 * 1024;
    } else if (std::holds_alternative<fp::ConvolveMatrix>(node.primitive) ||
               std::holds_alternative<fp::DiffuseLighting>(node.primitive) ||
               std::holds_alternative<fp::SpecularLighting>(node.primitive)) {
      result.standaloneBufferBytes += 1024;
    }
  }
  if (!graph.empty()) {
    result.runtimeUniformBytes += 1024;  // SourceAlpha and final conversion/resolve parameters.
  }
  return true;
}

/// Intermediate-retention behavior used to estimate backend memory before filter execution.
enum class FilterMemoryModel : std::uint8_t {
  CpuFloatNamedResults,  ///< TinySkia retains named float RGBA results (16 bytes per pixel).
  GpuAllNodes,           ///< Geode retains live float values and reusable node scratch.
};

/// Computes one primitive's full-surface work multiplier without changing its validation rules.
///
/// Each call operator returns the multiplier for one primitive type, or `std::nullopt` when the
/// primitive's parameters exceed a filter limit and the filter must be refused.
struct FilterWorkMultiplier {
  const FilterNode& node;         //!< Node whose primitive is measured.
  FilterMemoryModel memoryModel;  //!< Backend retention model that sets per-pixel costs.

  /// Kernel taps plus fixed per-pixel overhead, refused for a non-positive order.
  /// @param primitive Convolution parameters.
  std::optional<std::uint64_t> operator()(const filter_primitive::ConvolveMatrix& primitive) const {
    if (primitive.orderX <= 0 || primitive.orderY <= 0) {
      return std::nullopt;
    }
    return static_cast<std::uint64_t>(primitive.orderX) *
               static_cast<std::uint64_t>(primitive.orderY) +
           4;
  }

  /// Per-octave noise work, refused above the octave limit.
  /// @param primitive Turbulence parameters.
  std::optional<std::uint64_t> operator()(const filter_primitive::Turbulence& primitive) const {
    if (primitive.numOctaves > kMaximumFilterTurbulenceOctaves) {
      return std::nullopt;
    }
    const std::uint64_t octaves = static_cast<std::uint64_t>(std::max(primitive.numOctaves, 1));
    const std::uint64_t workPerOctave =
        memoryModel == FilterMemoryModel::CpuFloatNamedResults ? 16 : 4;
    return octaves * workPerOctave + 5;
  }

  /// Per-input composite work, refused above the merge input limit.
  std::optional<std::uint64_t> operator()(const filter_primitive::Merge&) const {
    if (node.inputs.size() > kMaximumFilterMergeInputs) {
      return std::nullopt;
    }
    const std::uint64_t inputs = static_cast<std::uint64_t>(node.inputs.size());
    return std::max<std::uint64_t>(inputs * 3 + 2, 5);
  }

  /// A higher multiplier when either standard deviation is positive.
  /// @param primitive Blur parameters.
  std::optional<std::uint64_t> operator()(const filter_primitive::GaussianBlur& primitive) const {
    return primitive.stdDeviationX > 0.0 || primitive.stdDeviationY > 0.0 ? 12 : 5;
  }

  /// A higher multiplier when either standard deviation is positive.
  /// @param primitive Drop shadow parameters.
  std::optional<std::uint64_t> operator()(const filter_primitive::DropShadow& primitive) const {
    return primitive.stdDeviationX > 0.0 || primitive.stdDeviationY > 0.0 ? 20 : 12;
  }

  /// The morphology work limit when either radius is positive.
  /// @param primitive Morphology parameters.
  std::optional<std::uint64_t> operator()(const filter_primitive::Morphology& primitive) const {
    return primitive.radiusX > 0.0 || primitive.radiusY > 0.0
               ? kMaximumFilterMorphologyWorkMultiplier
               : 5;
  }

  /// A higher multiplier for non-pixelated sampling on the CPU memory model.
  /// @param primitive Image parameters.
  std::optional<std::uint64_t> operator()(const filter_primitive::Image& primitive) const {
    return memoryModel == FilterMemoryModel::CpuFloatNamedResults &&
                   primitive.imageRendering != ImageRendering::Pixelated
               ? 68
               : 5;
  }

  /// Fixed per-pixel work for every other primitive.
  template <typename Primitive>
  std::optional<std::uint64_t> operator()(const Primitive&) const {
    return 5;
  }
};

/// Adds contextual-region sampling work for one node.
inline bool AddFilterRecipeWork(const FilterNode& node, std::uint64_t executions,
                                std::uint64_t& workUnits) {
  for (const auto& recipe : node.animatedRegion) {
    if (!recipe) {
      continue;
    }
    if (recipe->evaluationWork > kMaximumFilterWorkUnits / executions) {
      return false;
    }
    const std::uint64_t sampleWork = recipe->evaluationWork * executions;
    if (sampleWork > kMaximumFilterWorkUnits - workUnits) {
      return false;
    }
    workUnits += sampleWork;
  }
  return true;
}

/// Adds one node's primitive work while preserving the established overflow checks.
inline bool AddFilterNodeWork(const FilterNode& node, std::uint64_t pixelCount,
                              std::uint64_t executions, FilterMemoryModel memoryModel,
                              std::uint64_t& workUnits) {
  if (!AddFilterRecipeWork(node, executions, workUnits)) {
    return false;
  }
  const auto multiplier = std::visit(FilterWorkMultiplier{node, memoryModel}, node.primitive);
  if (!multiplier || *multiplier > kMaximumFilterWorkUnits ||
      (*multiplier != 0 && pixelCount > kMaximumFilterWorkUnits / *multiplier)) {
    return false;
  }
  const std::uint64_t nodeWork = pixelCount * *multiplier;
  if (nodeWork > kMaximumFilterWorkUnits - workUnits) {
    return false;
  }
  workUnits += nodeWork;
  return true;
}

/// Initializes the work estimate, including CPU animated-region coverage scans.
inline bool InitializeFilterWork(const FilterGraph& graph, std::uint64_t pixelCount,
                                 FilterMemoryModel memoryModel, std::uint64_t& workUnits) {
  workUnits = 0;
  if (memoryModel != FilterMemoryModel::CpuFloatNamedResults ||
      !std::any_of(graph.nodes.begin(), graph.nodes.end(),
                   [](const auto& node) { return node.hasAnimatedRegion(); })) {
    return true;
  }
  if (pixelCount > kMaximumFilterWorkUnits / 2) {
    return false;
  }
  workUnits = pixelCount * 2;
  return true;
}

/// Standard filter inputs that a graph reads, other than SourceGraphic.
struct FilterStandardInputUsage {
  bool sourceAlpha = false;  //!< Some node reads SourceAlpha.
  bool fillPaint = false;    //!< Some node reads FillPaint.
  bool strokePaint = false;  //!< Some node reads StrokePaint.
};

/// Collects the standard inputs retained by a CPU filter execution.
inline FilterStandardInputUsage StandardInputUsage(const FilterGraph& graph) {
  FilterStandardInputUsage result;
  for (const FilterNode& node : graph.nodes) {
    for (const FilterInput& input : node.inputs) {
      if (const auto* standard = std::get_if<FilterStandardInput>(&input.value)) {
        result.sourceAlpha |= *standard == FilterStandardInput::SourceAlpha;
        result.fillPaint |= *standard == FilterStandardInput::FillPaint;
        result.strokePaint |= *standard == FilterStandardInput::StrokePaint;
      }
    }
  }
  return result;
}

/// Computes one CPU primitive's transient float-buffer demand.
///
/// Each call operator returns the number of transient full-surface float buffers that one
/// primitive type needs while it runs.
struct CpuTransientBufferCount {
  const FilterGraph& graph;  //!< Graph that owns the node.
  const FilterNode& node;    //!< Node whose primitive is measured.
  bool linearRgb;            //!< Whether the node converts its inputs to linearRGB.

  /// None when the graph's only node blurs the source graphic, otherwise one.
  std::uint64_t operator()(const filter_primitive::GaussianBlur&) const {
    const bool implicitSource =
        node.inputs.empty() ||
        std::holds_alternative<FilterInput::Previous>(node.inputs.front().value) ||
        (std::holds_alternative<FilterStandardInput>(node.inputs.front().value) &&
         std::get<FilterStandardInput>(node.inputs.front().value) ==
             FilterStandardInput::SourceGraphic);
    return graph.nodes.size() == 1 && implicitSource ? 0 : 1;
  }

  /// One, plus one per input when converting to linearRGB.
  std::uint64_t operator()(const filter_primitive::Merge&) const {
    return linearRgb ? static_cast<std::uint64_t>(node.inputs.size()) + 1 : 1;
  }

  /// One, plus two when converting to linearRGB.
  std::uint64_t operator()(const filter_primitive::Blend&) const { return linearRgb ? 3 : 1; }
  /// One, plus two when converting to linearRGB.
  std::uint64_t operator()(const filter_primitive::Composite&) const { return linearRgb ? 3 : 1; }
  /// One, plus two when converting to linearRGB.
  std::uint64_t operator()(const filter_primitive::DisplacementMap&) const {
    return linearRgb ? 3 : 1;
  }
  /// One, plus one when converting to linearRGB.
  std::uint64_t operator()(const filter_primitive::ConvolveMatrix&) const {
    return linearRgb ? 2 : 1;
  }
  /// One, plus one when converting to linearRGB.
  std::uint64_t operator()(const filter_primitive::Morphology&) const { return linearRgb ? 2 : 1; }
  /// One, plus one when converting to linearRGB.
  std::uint64_t operator()(const filter_primitive::DiffuseLighting&) const {
    return linearRgb ? 2 : 1;
  }
  /// One, plus one when converting to linearRGB.
  std::uint64_t operator()(const filter_primitive::SpecularLighting&) const {
    return linearRgb ? 2 : 1;
  }
  /// Five, plus one when converting to linearRGB.
  std::uint64_t operator()(const filter_primitive::DropShadow&) const { return linearRgb ? 6 : 5; }

  /// One for every other primitive.
  template <typename Primitive>
  std::uint64_t operator()(const Primitive&) const {
    return 1;
  }
};

/// Computes the maximum live CPU float-buffer count across the graph.
inline std::uint64_t CpuRetainedBuffers(const FilterGraph& graph) {
  const FilterStandardInputUsage usage = StandardInputUsage(graph);
  const std::uint64_t fixedBuffers = 1 + static_cast<std::uint64_t>(usage.sourceAlpha) +
                                     static_cast<std::uint64_t>(usage.fillPaint) +
                                     static_cast<std::uint64_t>(usage.strokePaint);
  std::uint64_t retainedBuffers = 0;
  std::uint64_t priorNamedResults = 0;
  for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
    const FilterNode& node = graph.nodes[index];
    const bool linearRgb = node.colorInterpolationFilters.value_or(
                               graph.colorInterpolationFilters) != ColorInterpolationFilters::SRGB;
    const std::uint64_t transientBuffers =
        std::visit(CpuTransientBufferCount{graph, node, linearRgb}, node.primitive);
    const std::uint64_t previousOutput = index == 0 ? 0 : 1;
    const std::uint64_t namedResultCopy = node.result.has_value() ? 1 : 0;
    retainedBuffers = std::max(retainedBuffers, fixedBuffers + previousOutput + priorNamedResults +
                                                    transientBuffers + namedResultCopy);
    priorNamedResults += namedResultCopy;
  }
  return retainedBuffers;
}

/// Finalizes the CPU intermediate-memory estimate.
inline bool CpuFilterExecutionBytes(const FilterGraph& graph, std::uint64_t pixelCount,
                                    std::uint64_t workUnits, std::uint64_t& workUnitsOut,
                                    std::uint64_t& intermediateBytesOut) {
  constexpr std::uint64_t bytesPerPixel = 16;
  const std::uint64_t retainedBuffers = CpuRetainedBuffers(graph);
  if (retainedBuffers != 0 &&
      pixelCount > kMaximumFilterIntermediateBytes / bytesPerPixel / retainedBuffers) {
    return false;
  }
  workUnitsOut = workUnits;
  intermediateBytesOut = pixelCount * bytesPerPixel * retainedBuffers;
  return true;
}

/// Finalizes the GPU parameter, image, and texture-memory estimate.
inline bool GpuFilterExecutionBytes(const FilterGraph& graph, std::uint64_t memoryPixels,
                                    std::uint64_t executions, std::uint64_t workUnits,
                                    std::uint64_t& workUnitsOut,
                                    std::uint64_t& intermediateBytesOut) {
  GpuFilterWorkingSet layout;
  if (!ComputeGpuFilterWorkingSet(graph, layout)) {
    return false;
  }
  const std::uint64_t bytesPerPixel = layout.floatTextures * 16 + (graph.empty() ? 0 : 4);
  const std::uint64_t parameters =
      GpuFilterParameterAllocationBound(layout.uniformBytes * executions) +
      GpuFilterParameterAllocationBound(layout.runtimeUniformBytes * executions) +
      layout.standaloneBufferBytes * executions + kGpuFilterTransferTableBytes;
  const std::uint64_t imageBytes = layout.imageBytes * executions;
  if (parameters > kMaximumFilterIntermediateBytes ||
      imageBytes > kMaximumFilterIntermediateBytes - parameters ||
      (bytesPerPixel && memoryPixels > (kMaximumFilterIntermediateBytes - parameters - imageBytes) /
                                           bytesPerPixel)) {
    return false;
  }
  workUnitsOut = workUnits;
  intermediateBytesOut =
      memoryPixels == 0 ? 0 : memoryPixels * bytesPerPixel + imageBytes + parameters;
  return true;
}

/// Validates graph and logical-surface limits before estimating any node work.
inline bool FilterGraphSizeIsValid(const FilterGraph& graph, std::uint64_t pixelCount) {
  return pixelCount <= kMaximumFilterSurfacePixels &&
         graph.nodes.size() <= kMaximumFilterGraphNodes;
}

/// Validates retained-surface and repeated-execution dimensions.
inline bool FilterMemoryRequestIsValid(std::uint64_t memoryPixels, std::uint64_t executions) {
  return memoryPixels <= kMaximumFilterSurfacePixels && executions != 0 &&
         executions <= kMaximumFilterSurfacePixels;
}

/**
 * Estimate a filter graph's aggregate execution cost for a pixel surface.
 *
 * This rejects attacker-controlled graphs before allocating per-node buffers or starting
 * convolution loops. The estimate is intentionally conservative: GPU primitives may need up to
 * reusable float working textures, while the CPU backend retains every named float result.
 * @param graph Ordered graph. @param pixelCount Total pixels visited, including repeated halos.
 * @param memoryModel Backend retention model. @param workUnitsOut Receives bounded work.
 * @param intermediateBytesOut Receives allocation-byte bound, excluding caller-owned captures.
 * @param memoryPixels Working surface pixels; defaults to pixelCount for untiled execution.
 * @param executions Repetitions sharing working textures but consuming distinct parameter slots.
 */
inline bool FilterGraphExecutionCost(const FilterGraph& graph, std::uint64_t pixelCount,
                                     FilterMemoryModel memoryModel, std::uint64_t& workUnitsOut,
                                     std::uint64_t& intermediateBytesOut,
                                     std::uint64_t memoryPixels = UINT64_MAX,
                                     std::uint64_t executions = 1) {
  if (!FilterGraphSizeIsValid(graph, pixelCount)) {
    return false;
  }

  if (memoryPixels == UINT64_MAX) {
    memoryPixels = pixelCount;
  }
  if (!FilterMemoryRequestIsValid(memoryPixels, executions)) {
    return false;
  }
  std::uint64_t workUnits = 0;
  if (!InitializeFilterWork(graph, pixelCount, memoryModel, workUnits)) {
    return false;
  }
  for (const FilterNode& node : graph.nodes) {
    if (!AddFilterNodeWork(node, pixelCount, executions, memoryModel, workUnits)) {
      return false;
    }
  }
  if (memoryModel == FilterMemoryModel::CpuFloatNamedResults) {
    return CpuFilterExecutionBytes(graph, pixelCount, workUnits, workUnitsOut,
                                   intermediateBytesOut);
  }
  return GpuFilterExecutionBytes(graph, memoryPixels, executions, workUnits, workUnitsOut,
                                 intermediateBytesOut);
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
  /// Reason the frame stopped admitting filter work.
  enum class RejectionReason : std::uint8_t {
    None,            ///< No rejection has occurred.
    InvalidGraph,    ///< A graph failed validation or cost estimation.
    ExecutionLimit,  ///< The frame exceeded its graph execution count.
    WorkLimit,       ///< The frame exceeded its estimated pixel-work limit.
    MemoryLimit,     ///< Intermediate or capture memory exceeded its limit.
    External,        ///< A renderer-owned allocation or preflight failed.
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
    reservedGpuSurfaces_ = 0;
    persistentGpuBytes_ = 0;
    liveCpuCaptureBytes_ = 0;
    activeGpuReservations_ = 0;
    captureBytesReserved_ = 0;
    rejectionReason_ = RejectionReason::None;
    rejected_ = false;
  }

  /** Release GPU-retained accounting after submitting one ordered command-buffer chunk. */
  bool beginChunkAfterSubmit() {
    if (rejectionReason_ != RejectionReason::MemoryLimit || activeGpuReservations_ != 0 ||
        intermediateBytes_ == 0) {
      return false;
    }
    intermediateBytes_ = 0;
    reservedGpuSurfaces_ = 0;
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
                                     FilterMemoryModel memoryModel, std::uint64_t captureBytes,
                                     std::uint64_t retainedGpuBufferBytes = 0,
                                     std::uint64_t memoryPixels = UINT64_MAX,
                                     std::uint64_t executions = 1, std::size_t gpuSurfaces = 0) {
    std::uint64_t graphWorkUnits = 0;
    std::uint64_t graphIntermediateBytes = 0;
    if (rejected_) {
      return std::nullopt;
    }
    if (!FilterGraphExecutionCost(graph, pixelCount, memoryModel, graphWorkUnits,
                                  graphIntermediateBytes, memoryPixels, executions)) {
      rejectionReason_ = RejectionReason::InvalidGraph;
      rejected_ = true;
      return std::nullopt;
    }

    const bool gpu = memoryModel == FilterMemoryModel::GpuAllNodes;
    const std::uint64_t persistentBytes = std::max(persistentGpuBytes_, retainedGpuBufferBytes);
    if (gpu && (persistentBytes > kMaximumFilterFrameBytes ||
                gpuSurfaces > std::numeric_limits<std::size_t>::max() - reservedGpuSurfaces_)) {
      rejectionReason_ = RejectionReason::MemoryLimit;
      rejected_ = true;
      return std::nullopt;
    }
    const std::uint64_t retainedBeforeExecution =
        gpu ? intermediateBytes_ + persistentBytes : liveCpuCaptureBytes_;
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
      persistentGpuBytes_ = persistentBytes;
      reservedGpuSurfaces_ += gpuSurfaces;
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
  bool consume(const FilterGraph& graph, std::uint64_t pixelCount, FilterMemoryModel memoryModel,
               std::uint64_t retainedGpuBufferBytes = 0) {
    std::optional<Reservation> reservation =
        reserve(graph, pixelCount, memoryModel, 0, retainedGpuBufferBytes);
    if (!reservation.has_value()) {
      return false;
    }
    release(*reservation);
    return true;
  }

  /// Requests an ordered chunk when another shared GPU budget lacks forecast capacity.
  void requireMemoryChunk() {
    if (!rejected_) {
      rejectionReason_ = RejectionReason::MemoryLimit;
      rejected_ = true;
    }
  }

  /// Forecast surface count retained by GPU executions until the next submitted chunk.
  std::size_t reservedGpuSurfaces() const { return reservedGpuSurfaces_; }

  /// Latch the frame closed after an allocation or other external preflight failure.
  void reject() {
    rejectionReason_ = RejectionReason::External;
    rejected_ = true;
  }

  /// Filter graph executions admitted during the current frame.
  [[nodiscard]] std::uint64_t executions() const { return executions_; }
  /// Estimated pixel-work units admitted during the current frame.
  [[nodiscard]] std::uint64_t workUnits() const { return workUnits_; }
  /// Total intermediate, persistent GPU, and live CPU capture bytes accounted for this frame.
  [[nodiscard]] std::uint64_t retainedBytes() const {
    return intermediateBytes_ + persistentGpuBytes_ + liveCpuCaptureBytes_;
  }
  /// Bytes reserved for capture surfaces before allocation.
  [[nodiscard]] std::uint64_t captureBytesReserved() const { return captureBytesReserved_; }
  /// GPU capture reservations still active in the current frame.
  [[nodiscard]] std::uint64_t activeGpuReservations() const { return activeGpuReservations_; }
  /// Intermediate and persistent GPU bytes retained for filter execution.
  [[nodiscard]] std::uint64_t retainedGpuBytes() const {
    return intermediateBytes_ + persistentGpuBytes_;
  }
  /// Ordered GPU chunks submitted since this budget was constructed.
  [[nodiscard]] std::uint64_t chunks() const { return chunks_; }
  /// Whether the frame has stopped admitting filter work.
  [[nodiscard]] bool rejected() const { return rejected_; }
  /// Current recorded reason for rejecting filter work in this frame.
  [[nodiscard]] RejectionReason rejectionReason() const { return rejectionReason_; }

private:
  std::uint64_t executions_ = 0;
  std::uint64_t workUnits_ = 0;
  std::uint64_t intermediateBytes_ = 0;
  std::uint64_t persistentGpuBytes_ = 0;
  std::size_t reservedGpuSurfaces_ = 0;
  std::uint64_t liveCpuCaptureBytes_ = 0;
  std::uint64_t activeGpuReservations_ = 0;
  std::uint64_t captureBytesReserved_ = 0;
  std::uint64_t chunks_ = 0;
  RejectionReason rejectionReason_ = RejectionReason::None;
  bool rejected_ = false;
};

}  // namespace donner::svg::components
