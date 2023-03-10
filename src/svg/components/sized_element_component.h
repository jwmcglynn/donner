#pragma once
/// @file

#include <optional>
#include <tuple>

#include "src/base/box.h"
#include "src/base/length.h"
#include "src/base/transform.h"
#include "src/svg/components/computed_style_component.h"
#include "src/svg/properties/presentation_attribute_parsing.h"
#include "src/svg/registry/registry.h"

namespace donner::svg {

/**
 * Stores an offset/size for elements that are positioned with x/y/width/height attributes with
 * respect to their parent. Used for <svg>, <image> and <foreignObject> by the standard, and also
 * internally with <use> for Donner.
 *
 * If not specified, x/y default to 0, and width/height are std::nullopt.
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
  ComputedSizedElementComponent(EntityHandle handle, SizedElementProperties properties,
                                const std::map<RcString, UnparsedProperty>& unparsedProperties,
                                Boxd inheritedViewbox, FontMetrics fontMetrics,
                                std::vector<ParseError>* outWarnings);

  Boxd bounds;            //!< The computed rect of this sized element.
  Boxd inheritedViewbox;  //!< The viewbox of the parent element, used for preserveAspectRatio
                          //!< transformations.

  /**
   * If this element establishes a clipping context, returns the clip rect in the parent's
   * coordinate system.
   *
   * @param handle Entity handle.
   * @return std::optional<Boxd> Clip rect, or std::nullopt if this element does not establish a
   *   clipping context.
   */
  std::optional<Boxd> clipRect(EntityHandle handle) const;

  /**
   * Computes the transformation from the parent's cordinate system into the coordinate system
   * established by this sized element.
   *
   * @param handle Entity handle.
   * @return Transformd Transformation from the parent's coordinate system into the sized element's
   * c  oordinate system.
   */
  Transformd computeTransform(EntityHandle handle) const;
};

enum class InvalidSizeBehavior {
  ZeroSize,
  ReturnDefault,
};

struct SizedElementComponent {
  SizedElementProperties properties;

  std::optional<float> intrinsicAspectRatio(Registry& registry) const;
  Vector2i calculateDocumentSize(Registry& registry) const;

  Vector2i calculateViewportScaledDocumentSize(Registry& registry, InvalidSizeBehavior behavior) const;

  void computeWithPrecomputedStyle(EntityHandle handle, const ComputedStyleComponent& style,
                                   FontMetrics fontMetrics, std::vector<ParseError>* outWarnings);

  void compute(EntityHandle handle);

private:
  Vector2d calculateRawDocumentSize(Registry& registry) const;
};

}  // namespace donner::svg
