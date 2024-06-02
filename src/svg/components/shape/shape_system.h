#pragma once
/// @file

#include "src/svg/components/shape/circle_component.h"
#include "src/svg/components/shape/computed_path_component.h"
#include "src/svg/components/shape/ellipse_component.h"
#include "src/svg/components/shape/line_component.h"
#include "src/svg/components/shape/path_component.h"
#include "src/svg/components/shape/poly_component.h"
#include "src/svg/components/shape/rect_component.h"
#include "src/svg/components/style/computed_style_component.h"
#include "src/svg/components/style/style_system.h"
#include "src/svg/registry/registry.h"

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
   * @param component Shape component attached to \ref handle
   * @param fontMetrics Font metrics, used to scale lengths
   * @param outWarnings Containing any warnings found
   */
  template <typename T>
  const ComputedPathComponent* createComputedPath(EntityHandle handle, const T& component,
                                                  const FontMetrics& fontMetrics,
                                                  std::vector<ParseError>* outWarnings) {
    const ComputedStyleComponent& style = StyleSystem().computeStyle(handle, outWarnings);
    return createComputedShapeWithStyle(handle, component, style, fontMetrics, outWarnings);
  }

  /**
   * Create \ref ComputedPathComponent for all entities in the registry that have a shape component.
   *
   * This assumes that \ref StyleSystem::computeAllStyles has already been called.
   *
   * @param registry Registry used to find all shape components
   * @param outWarnings Containing any warnings found
   */
  void instantiateAllComputedPaths(Registry& registry, std::vector<ParseError>* outWarnings);

private:
  using AllShapes = entt::type_list<CircleComponent, EllipseComponent, LineComponent, PathComponent,
                                    PolyComponent, RectComponent>;

  const ComputedPathComponent* createComputedShapeWithStyle(EntityHandle handle,
                                                            const CircleComponent& circle,
                                                            const ComputedStyleComponent& style,
                                                            const FontMetrics& fontMetrics,
                                                            std::vector<ParseError>* outWarnings);

  const ComputedPathComponent* createComputedShapeWithStyle(EntityHandle handle,
                                                            const EllipseComponent& ellipse,
                                                            const ComputedStyleComponent& style,
                                                            const FontMetrics& fontMetrics,
                                                            std::vector<ParseError>* outWarnings);

  const ComputedPathComponent* createComputedShapeWithStyle(EntityHandle handle,
                                                            const LineComponent& line,
                                                            const ComputedStyleComponent& style,
                                                            const FontMetrics& fontMetrics,
                                                            std::vector<ParseError>* outWarnings);

  const ComputedPathComponent* createComputedShapeWithStyle(EntityHandle handle,
                                                            const PathComponent& path,
                                                            const ComputedStyleComponent& style,
                                                            const FontMetrics& fontMetrics,
                                                            std::vector<ParseError>* outWarnings);

  const ComputedPathComponent* createComputedShapeWithStyle(EntityHandle handle,
                                                            const PolyComponent& poly,
                                                            const ComputedStyleComponent& style,
                                                            const FontMetrics& fontMetrics,
                                                            std::vector<ParseError>* outWarnings);

  const ComputedPathComponent* createComputedShapeWithStyle(EntityHandle handle,
                                                            const RectComponent& rect,
                                                            const ComputedStyleComponent& style,
                                                            const FontMetrics& fontMetrics,
                                                            std::vector<ParseError>* outWarnings);
};

}  // namespace donner::svg::components
