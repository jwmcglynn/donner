#pragma once
/// @file

#include <optional>

#include "donner/base/Length.h"
#include "donner/base/RcString.h"
#include "donner/css/Color.h"
#include "donner/svg/components/filter/FilterGraph.h"
#include "donner/svg/properties/Property.h"

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
  double stdDeviationX = 0.0;  ///< The standard deviation of the Gaussian blur in the x direction.
  double stdDeviationY = 0.0;  ///< The standard deviation of the Gaussian blur in the y direction.

  // TODO(https://github.com/jwmcglynn/donner/issues/151): edgeMode parameter.
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

}  // namespace donner::svg::components
