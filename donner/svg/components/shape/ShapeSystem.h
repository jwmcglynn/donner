#pragma once
/// @file

#include <span>

#include "donner/base/EcsRegistry.h"
#include "donner/base/ParseWarningSink.h"
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

/// Shared work limit for resolving basic-shape reference geometry during one render preparation.
struct CircleClipGeometryBudget {
  /// Maximum aggregate path/entity work accepted during one preparation.
  static constexpr std::size_t kMaximumWork = Path::kMaximumGeometryQueryWork;

  /// Reserve bounded geometry work. Once rejected, every later reservation also fails.
  /// @param work Work units required by the next operation.
  /// @return True when the reservation was accepted.
  bool reserve(std::size_t work) {
    if (rejected || work > remainingWork) {
      rejected = true;
      return false;
    }
    remainingWork -= work;
    return true;
  }

  /// Reject this preparation and every later reservation.
  void reject() {
    rejected = true;
    remainingWork = 0;
  }

  std::size_t remainingWork = kMaximumWork;  ///< Work still available to this preparation.
  bool rejected = false;                     ///< Whether this budget has rejected any reservation.
};

/**
 * Creates computed path for shapes such as \ref xml_circle, \ref xml_line, and \ref xml_path.
 *
 * @ingroup ecs_systems
 * @see https://www.w3.org/TR/SVG2/shapes.html
 */
class ShapeSystem {
public:
  /// Select path/cache ownership without changing the instance's transform or resource owner.
  /// @param instance Style/tree entity. @return Instance geometry or its light-tree fallback.
  static EntityHandle geometryHandle(EntityHandle instance);

  /// Prepare newly created shadow instances without resampling the rest of the document.
  /// @param registry Document storage. @param entities Admitted shadow entity span.
  /// @param warningSink Diagnostic sink.
  void instantiateShadowComputedPaths(Registry& registry, std::span<const Entity> entities,
                                      ParseWarningSink& warningSink);

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
   * @param warningSink Containing any warnings found
   */
  template <typename T>
  ComputedPathComponent* createComputedPath(EntityHandle handle, const T& component,
                                            const FontMetrics& fontMetrics,
                                            ParseWarningSink& warningSink) {
    const ComputedStyleComponent& style = StyleSystem().computeStyle(handle, warningSink);
    return createComputedShapeWithStyle(handle, component, style, fontMetrics, warningSink);
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
   * @param warningSink Containing any warnings found
   */
  ComputedPathComponent* createComputedPathIfShape(EntityHandle handle,
                                                   const FontMetrics& fontMetrics,
                                                   ParseWarningSink& warningSink);

  /**
   * Create \ref ComputedPathComponent for all entities in the registry that have a shape component.
   *
   * This assumes that \ref StyleSystem::computeAllStyles has already been called.
   *
   * @param registry Registry used to find all shape components
   * @param warningSink Containing any warnings found
   */
  void instantiateAllComputedPaths(Registry& registry, ParseWarningSink& warningSink);

  /**
   * Get the bounds for the given entity in the entity's local space, if it has a shape component.
   *
   * @param handle Entity handle to get the bounds for
   */
  std::optional<Box2d> getShapeBounds(EntityHandle handle);

  /// Return the SVG stroke-shape bounds, including descendant object and stroke geometry.
  /// @param handle Element whose local reference box is required.
  std::optional<Box2d> getStrokeBounds(EntityHandle handle);

  /// Resolve a circle clipping shape in element-local coordinates; invalid geometry is empty.
  /// @param handle Styled element supplying reference geometry and length metrics.
  /// @param circle Parsed circle and reference-box selection.
  /// @param budget Shared admission budget for the render preparation.
  Path circleClipPath(EntityHandle handle, const CircleClipPath& circle,
                      CircleClipGeometryBudget& budget);

  /**
   * Get the bounds for the given entity in world space, if it has a shape component.
   *
   * @param handle Entity handle to get the bounds for
   */
  std::optional<Box2d> getShapeWorldBounds(EntityHandle handle);

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
  /// Prepare one shadow's contextual geometry at most once in the current shape pass.
  /// @param handle Shadow entity. @param warningSink Diagnostic sink.
  void instantiateShadowComputedPath(EntityHandle handle, ParseWarningSink& warningSink);

  using AllShapes = entt::type_list<CircleComponent, EllipseComponent, LineComponent, PathComponent,
                                    PolyComponent, RectComponent>;

  /**
   * Get the tight bounds for the given entity in a specific coordinate space, if it has a shape
   * component.
   *
   * @param handle Entity handle to get the bounds for
   * @param targetFromWorld Transform from world coordinates to the target coordinate system
   */
  std::optional<Box2d> getTransformedShapeBounds(EntityHandle handle,
                                                 const Transform2d& targetFromWorld,
                                                 bool includeStroke = false,
                                                 CircleClipGeometryBudget* budget = nullptr);

  /// Resolve the selected circle reference box in the element's local coordinate space.
  /// @param handle Styled element supplying reference geometry.
  /// @param box Reference-box selection.
  /// @param budget Shared geometry work budget.
  std::optional<Box2d> getCircleReferenceBox(EntityHandle handle, CircleClipPath::Box box,
                                             CircleClipGeometryBudget& budget);

  ComputedPathComponent* createComputedShapeWithStyle(EntityHandle handle,
                                                      const CircleComponent& circle,
                                                      const ComputedStyleComponent& style,
                                                      const FontMetrics& fontMetrics,
                                                      ParseWarningSink& warningSink);

  ComputedPathComponent* createComputedShapeWithStyle(EntityHandle handle,
                                                      const EllipseComponent& ellipse,
                                                      const ComputedStyleComponent& style,
                                                      const FontMetrics& fontMetrics,
                                                      ParseWarningSink& warningSink);

  ComputedPathComponent* createComputedShapeWithStyle(EntityHandle handle,
                                                      const LineComponent& line,
                                                      const ComputedStyleComponent& style,
                                                      const FontMetrics& fontMetrics,
                                                      ParseWarningSink& warningSink);

  ComputedPathComponent* createComputedShapeWithStyle(EntityHandle handle,
                                                      const PathComponent& path,
                                                      const ComputedStyleComponent& style,
                                                      const FontMetrics& fontMetrics,
                                                      ParseWarningSink& warningSink);

  ComputedPathComponent* createComputedShapeWithStyle(EntityHandle handle,
                                                      const PolyComponent& poly,
                                                      const ComputedStyleComponent& style,
                                                      const FontMetrics& fontMetrics,
                                                      ParseWarningSink& warningSink);

  ComputedPathComponent* createComputedShapeWithStyle(EntityHandle handle,
                                                      const RectComponent& rect,
                                                      const ComputedStyleComponent& style,
                                                      const FontMetrics& fontMetrics,
                                                      ParseWarningSink& warningSink);
};

/**
 * Parse presentation attributes for a \ref xml_path element (the "d" attribute).
 */
ParseResult<bool> ParsePathPresentationAttribute(EntityHandle handle, std::string_view name,
                                                 const parser::PropertyParseFnParams& params);

}  // namespace donner::svg::components
