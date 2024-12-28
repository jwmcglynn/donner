#pragma once
/// @file

#include <optional>

#include "donner/base/Box.h"
#include "donner/base/EcsRegistry.h"
#include "donner/base/Transform.h"
#include "donner/svg/components/layout/SizedElementComponent.h"
#include "donner/svg/components/layout/TransformComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"

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
  /// Controls the behavior of \ref LayoutSystem::calculateCanvasScaledDocumentSize for invalid
  /// sizes.
  enum class InvalidSizeBehavior {
    ZeroSize,       //!< Return a size of 0x0.
    ReturnDefault,  //!< Return the default document size (512x512).
  };

  /// @name Regular properties
  /// @{

  /**
   * Calculate the intrinsic aspect ratio per
   * https://svgwg.org/svg2-draft/coords.html#SizingSVGInCSS, which defines how content is scaled to
   * fit the viewport. This may return std::nullopt if the aspect ratio is not defined.
   *
   * @param entity Current entity.
   */
  std::optional<float> intrinsicAspectRatio(EntityHandle entity) const;

  /**
   * Calculate the document size of the current entity. This is the size of the document viewport
   * (the area that the SVG content is rendered into).
   *
   * @param registry ECS registry
   */
  Vector2i calculateDocumentSize(Registry& registry) const;

  /**
   * Get the viewport affecting the current entity. This may be the viewport of a viewport-defining
   * parent element or the document viewport.
   *
   * @param entity Current entity.
   */
  Boxd getViewport(EntityHandle entity);

  /**
   * Returns true if the given entity overrides the viewport.
   *
   * @param entity Current entity.
   */
  bool overridesViewport(EntityHandle entity) const;

  /**
   * Get the document size scaled to fit the canvas.
   *
   * @param registry ECS registry
   * @param behavior Controls the behavior when the document size is invalid, either returning a
   * default size or empty box.
   */
  Vector2i calculateCanvasScaledDocumentSize(Registry& registry,
                                             InvalidSizeBehavior behavior) const;

  /**
   * Get the entity-from-parent transform for the current entity.
   *
   * @param entity Current entity.
   */
  Transformd getEntityFromParentTranform(EntityHandle entity);

  /**
   * Get the scale transform from the canvas to the SVG document.
   *
   * @param registry ECS registry
   */
  Transformd getDocumentFromCanvasTransform(Registry& registry);

  /**
   * Get the transform for entityContent-from-entity, which is an additional transform for specific
   * elements that define their own coordinate system, such as nested \ref xml_svg and \ref xml_use
   * elements.
   *
   * This transform is used to convert coordinates from the entity's coordinate system to the
   * coordinate system of its content.
   *
   * For example, a nested SVG element, where for the inner SVG element the content transform is
   * `scale(2) translate(50 50)`:
   * ```xml
   * <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
   *   <svg x="50" y="50" width="100" height="100" viewBox="0 0 50 50">
   *     <rect x="0" y="0" width="50" height="50" fill="red"/>
   *   </svg>
   * </svg>
   * ```
   *
   * @param entity Current entity.
   */
  Transformd getEntityContentFromEntityTransform(EntityHandle entity);

  /**
   * Set the entity-from-parent transform for the current entity.
   *
   * @param entity Current entity.
   * @param entityFromParent New transform.
   */
  void setEntityFromParentTransform(EntityHandle entity, const Transformd& entityFromParent);

  /**
   * Get the computed absolute transform for the current entity. This is the same as \ref
   * getEntityFromWorldTransform except it returns the component containing additional flags as
   * well.
   *
   * @param entity Current entity.
   */
  const ComputedAbsoluteTransformComponent& getAbsoluteTransformComponent(EntityHandle entity);

  /**
   * Get the entity-from-world transform for the current entity, representing the entity position
   * relative to the world after applying all parent transformations.
   *
   * @param entity Current entity.
   */
  Transformd getEntityFromWorldTransform(EntityHandle entity);

  /**
   * Invalidate cached state for the current entity, such as the computed viewport and
   * entity-from-world transform.
   *
   * @param entity Current entity.
   */
  void invalidate(EntityHandle entity);

  /// @}

  /// @name Computed properties
  /// @{

  /**
   * Create all computed components, such as \ref ComputedViewboxComponent and \ref
   * ComputedSizedElementComponent.
   *
   * @param registry ECS registry.
   * @param outWarnings Output vector of parse errors, if any.
   */
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
   * @param handle Entity handle.
   * @param style Precomputed style information for this element
   * @param fontMetrics Font metrics, used to scale lengths
   * @param outWarnings Output vector of parse errors, if any.
   * @return reference to the computed component
   */
  const ComputedSizedElementComponent& createComputedSizedElementComponentWithStyle(
      EntityHandle handle, const ComputedStyleComponent& style, FontMetrics fontMetrics,
      std::vector<parser::ParseError>* outWarnings);

  /**
   * Creates a \ref ComputedLocalTransformComponent for the linked entity, using precomputed style
   * information.
   *
   * @param handle Entity handle.
   * @param style Precomputed style information for this element
   * @param fontMetrics Font metrics, used to scale lengths
   * @param outWarnings Output vector of parse errors, if any.
   */
  const ComputedLocalTransformComponent& createComputedLocalTransformComponentWithStyle(
      EntityHandle handle, const ComputedStyleComponent& style, const FontMetrics& fontMetrics,
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

  /**
   * Get the document size scaled to fit the canvas, as a floating-point number without rounding.
   * This is called internally by \ref calculateDocumentSize.
   *
   * @param registry ECS registry
   */
  Vector2d calculateRawDocumentSize(Registry& registry) const;
};

}  // namespace donner::svg::components
