#pragma once
/// @file

#include <optional>

#include "donner/base/Box.h"
#include "donner/base/Transform.h"
#include "donner/svg/components/layout/SizedElementComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/registry/Registry.h"

namespace donner::svg::components {

/**
 * Handles layout and bounds calculations for SVG elements. This system is responsible for
 * calculating the the document size, viewboxes, and the bounds for elements.
 *
 * @ingroup ecs_systems
 * @see https://www.w3.org/TR/SVG2/coords.html
 */
class LayoutSystem {
public:
  /// Controls the behavior of \ref LayoutSystem::calculateViewportScaledDocumentSize for invalid
  /// sizes.
  enum class InvalidSizeBehavior {
    ZeroSize,       //!< Return a size of 0x0.
    ReturnDefault,  //!< Return the default document size (512x512).
  };

  /// @name Regular properties
  /// @{

  std::optional<float> intrinsicAspectRatio(EntityHandle entity) const;
  Vector2i calculateDocumentSize(EntityHandle entity) const;

  Vector2i calculateViewportScaledDocumentSize(EntityHandle entity,
                                               InvalidSizeBehavior behavior) const;

  /// @}

  /// @name Computed properties
  /// @{

  void instantiateAllComputedComponents(Registry& registry,
                                        std::vector<parser::ParseError>* outWarnings);

  /**
   * Evaluates SizedElementProperties and returns the resulting bounds, using precomputed style
   * information.
   *
   * @param entity Entity handle.
   * @param sizeProperties Size properties to evaluate.
   * @param unparsedProperties Unparsed properties to evaluate.
   * @param viewbox Viewbox to use when converting size percentages.
   * @param fontMetrics Font metrics, used to scale lengths.
   * @param outWarnings Output vector of parse errors, if any.
   * @return Computed bounds.
   */
  Boxd computeSizeProperties(EntityHandle entity, const SizedElementProperties& sizeProperties,
                             const std::map<RcString, parser::UnparsedProperty>& unparsedProperties,
                             const Boxd& viewbox, FontMetrics fontMetrics,
                             std::vector<parser::ParseError>* outWarnings);

  /**
   * Creates a \ref ComputedSizedElementComponent for the linked entity, using precomputed style
   * information.
   *
   * @param entity Entity handle.
   * @param style Precomputed style information for this element
   * @param fontMetrics Font metrics, used to scale lengths
   * @param outWarnings Output vector of parse errors, if any.
   * @return reference to the computed component
   */
  const ComputedSizedElementComponent& createComputedSizedElementComponentWithStyle(
      EntityHandle handle, const ComputedStyleComponent& style, FontMetrics fontMetrics,
      std::vector<parser::ParseError>* outWarnings);

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
   * Computes the transformation from the parent's coordinate system into the coordinate system
   * established by this sized element.
   *
   * @param handle Entity handle.
   * @param computedSizedElement Precomputed sized element information.
   * @return Transformd Transformation from the parent's coordinate system into the sized element's
   * coordinate system.
   */
  Transformd computeSizedElementTransform(
      EntityHandle handle, const ComputedSizedElementComponent& computedSizedElement) const;

  /// @}

private:
  /**
   * Calculate the size of an element such as \ref xml_svg and \ref xml_use, which defines x, y,
   * width, height, and an optional viewport.
   *
   * @param entity The entity to calculate the size for.
   * @param properties The size properties of the element.
   * @param inheritedViewbox The viewbox of the parent element.
   * @param fontMetrics Font metrics, used to calculate the size of text elements.
   */
  Boxd calculateSizedElementBounds(EntityHandle entity, const SizedElementProperties& properties,
                                   const Boxd& inheritedViewbox, FontMetrics fontMetrics);

  Vector2d calculateRawDocumentSize(EntityHandle handle) const;
};

}  // namespace donner::svg::components
