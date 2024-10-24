#pragma once
/// @file

#include "donner/base/EcsRegistry.h"
#include "donner/svg/components/shape/CircleComponent.h"
#include "donner/svg/components/shape/ComputedPathComponent.h"
#include "donner/svg/components/shape/EllipseComponent.h"
#include "donner/svg/components/shape/LineComponent.h"
#include "donner/svg/components/shape/PathComponent.h"
#include "donner/svg/components/shape/PolyComponent.h"
#include "donner/svg/components/shape/RectComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/style/StyleSystem.h"

namespace donner::svg::components {

/**
 * Creates computed path for shapes such as \ref xml_circle, \ref xml_line, and \ref xml_path.
 *
 * @ingroup ecs_systems
 * @see https://www.w3.org/TR/SVG2/shapes.html
 */
class ShapeSystem {
public:
  /**
   * Convert the given shape to a path, evaluating CSS styling information and presentation
   * attributes. Returns the \ref ComputedPathComponent if the path was successfully created, or
   * nullptr if the path could not be created.
   *
   * Paths may not be created if the shape is invalid, such as a circle with a negative radius.
   *
   * Suported components:
   * - \ref CircleComponent
   * - \ref EllipseComponent
   * - \ref LineComponent
   * - \ref PathComponent
   * - \ref PolyComponent
   * - \ref RectComponent
   *
   * @param handle Entity handle to create the computed path for
   * @param component Shape component attached to \p handle
   * @param fontMetrics Font metrics, used to scale lengths
   * @param outWarnings Containing any warnings found
   */
  template <typename T>
  ComputedPathComponent* createComputedPath(EntityHandle handle, const T& component,
                                            const FontMetrics& fontMetrics,
                                            std::vector<parser::ParseError>* outWarnings) {
    const ComputedStyleComponent& style = StyleSystem().computeStyle(handle, outWarnings);
    return createComputedShapeWithStyle(handle, component, style, fontMetrics, outWarnings);
  }

  /**
   * Convert the given shape to a path, if the provided entity contains a shape. Returns the \ref
   * ComputedPathComponent if the path was successfully created, or nullptr if the path could not be
   * created.
   *
   * Paths may not be created if the shape is invalid, such as a circle with a negative radius.
   *
   * @param handle Entity handle to create the computed path for
   * @param fontMetrics Font metrics, used to scale lengths
   * @param outWarnings Containing any warnings found
   */
  ComputedPathComponent* createComputedPathIfShape(EntityHandle handle,
                                                   const FontMetrics& fontMetrics,
                                                   std::vector<parser::ParseError>* outWarnings);

  /**
   * Create \ref ComputedPathComponent for all entities in the registry that have a shape component.
   *
   * This assumes that \ref StyleSystem::computeAllStyles has already been called.
   *
   * @param registry Registry used to find all shape components
   * @param outWarnings Containing any warnings found
   */
  void instantiateAllComputedPaths(Registry& registry,
                                   std::vector<parser::ParseError>* outWarnings);

  /**
   * Get the bounds for the given entity in the entity's local space, if it has a shape component.
   *
   * @param handle Entity handle to get the bounds for
   */
  std::optional<Boxd> getShapeBounds(EntityHandle handle);

  /**
   * Get the bounds for the given entity in world space, if it has a shape component.
   *
   * @param handle Entity handle to get the bounds for
   */
  std::optional<Boxd> getShapeWorldBounds(EntityHandle handle);

  /**
   * Returns true if the shape's path fill intersects the given point.
   *
   * @param handle Entity handle containing the shape
   * @param point Point to intersect
   * @param fillRule Fill rule to use for the intersection test
   */
  bool pathFillIntersects(EntityHandle handle, const Vector2d& point, FillRule fillRule);

  /**
   * Returns true if the shape's path stroke intersects the given point, using an approximate
   * computation from the stroke-width.
   *
   * @param handle Entity handle containing the shape
   * @param point Point to intersect
   * @param strokeWidth Stroke width to use for the intersection test
   */
  bool pathStrokeIntersects(EntityHandle handle, const Vector2d& point, double strokeWidth);

private:
  using AllShapes = entt::type_list<CircleComponent, EllipseComponent, LineComponent, PathComponent,
                                    PolyComponent, RectComponent>;

  /**
   * Get the tight bounds for the given entity in a specific coordinate space, if it has a shape
   * component.
   *
   * @param handle Entity handle to get the bounds for
   * @param worldFromTarget Transform to the world coordinate system from the target coordinate
   * system
   */
  std::optional<Boxd> getTransformedShapeBounds(EntityHandle handle,
                                                const Transformd& worldFromTarget);

  ComputedPathComponent* createComputedShapeWithStyle(EntityHandle handle,
                                                      const CircleComponent& circle,
                                                      const ComputedStyleComponent& style,
                                                      const FontMetrics& fontMetrics,
                                                      std::vector<parser::ParseError>* outWarnings);

  ComputedPathComponent* createComputedShapeWithStyle(EntityHandle handle,
                                                      const EllipseComponent& ellipse,
                                                      const ComputedStyleComponent& style,
                                                      const FontMetrics& fontMetrics,
                                                      std::vector<parser::ParseError>* outWarnings);

  ComputedPathComponent* createComputedShapeWithStyle(EntityHandle handle,
                                                      const LineComponent& line,
                                                      const ComputedStyleComponent& style,
                                                      const FontMetrics& fontMetrics,
                                                      std::vector<parser::ParseError>* outWarnings);

  ComputedPathComponent* createComputedShapeWithStyle(EntityHandle handle,
                                                      const PathComponent& path,
                                                      const ComputedStyleComponent& style,
                                                      const FontMetrics& fontMetrics,
                                                      std::vector<parser::ParseError>* outWarnings);

  ComputedPathComponent* createComputedShapeWithStyle(EntityHandle handle,
                                                      const PolyComponent& poly,
                                                      const ComputedStyleComponent& style,
                                                      const FontMetrics& fontMetrics,
                                                      std::vector<parser::ParseError>* outWarnings);

  ComputedPathComponent* createComputedShapeWithStyle(EntityHandle handle,
                                                      const RectComponent& rect,
                                                      const ComputedStyleComponent& style,
                                                      const FontMetrics& fontMetrics,
                                                      std::vector<parser::ParseError>* outWarnings);
};

}  // namespace donner::svg::components
