#pragma once
/// @file

#include <optional>

#include "src/base/box.h"
#include "src/base/transform.h"
#include "src/svg/components/layout/sized_element_component.h"
#include "src/svg/components/style/computed_style_component.h"
#include "src/svg/registry/registry.h"

namespace donner::svg::components {

class LayoutSystem {
public:
  /// @name Regular properties
  /// @{

  std::optional<float> intrinsicAspectRatio(EntityHandle entity) const;
  Vector2i calculateDocumentSize(EntityHandle entity) const;

  Boxd calculateBounds(EntityHandle entity, const SizedElementProperties& properties,
                       const Boxd& inheritedViewbox, FontMetrics fontMetrics);

  Vector2i calculateViewportScaledDocumentSize(EntityHandle entity,
                                               InvalidSizeBehavior behavior) const;

  /// @}

  /// @name Computed properties
  /// @{

  void instantiateAllComputedComponents(Registry& registry, std::vector<ParseError>* outWarnings);

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
      std::vector<ParseError>* outWarnings);

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
  Transformd computeTransform(EntityHandle handle,
                              const ComputedSizedElementComponent& computedSizedElement) const;

  /// @}

private:
  Vector2d calculateRawDocumentSize(EntityHandle handle) const;
};

}  // namespace donner::svg::components
