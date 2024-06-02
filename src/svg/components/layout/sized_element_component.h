#pragma once
/// @file

#include <optional>
#include <tuple>

#include "src/base/box.h"
#include "src/base/length.h"
#include "src/svg/properties/property.h"

namespace donner::svg::components {

/**
 * Stores an offset/size for elements that are positioned with `x`/`y`/`width`/`height` attributes
 * with respect to their parent. Used for \ref xml_svg, \ref xml_image and \ref xml_foreignObject by
 * the standard, and also internally with \ref use for Donner.
 *
 * If not specified, `x`/`y` default to 0, and `width`/`height` are `std::nullopt`.
 */
struct SizedElementProperties {
  Property<Lengthd> x{"x",
                      []() -> std::optional<Lengthd> { return Lengthd(0, Lengthd::Unit::None); }};
  Property<Lengthd> y{"y",
                      []() -> std::optional<Lengthd> { return Lengthd(0, Lengthd::Unit::None); }};
  Property<Lengthd> width{"width", []() -> std::optional<Lengthd> { return std::nullopt; }};
  Property<Lengthd> height{"height", []() -> std::optional<Lengthd> { return std::nullopt; }};

  auto allProperties() { return std::forward_as_tuple(x, y, width, height); }
};

struct ComputedSizedElementComponent {
  Boxd bounds;            ///< The computed rect of this sized element.
  Boxd inheritedViewbox;  ///< The viewbox of the parent element, used for preserveAspectRatio
                          ///< transformations.
};

enum class InvalidSizeBehavior {
  ZeroSize,
  ReturnDefault,
};

struct SizedElementComponent {
  SizedElementProperties properties;
};

}  // namespace donner::svg::components
