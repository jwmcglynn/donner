#pragma once
/// @file

#include <any>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/base/ParseError.h"
#include "donner/base/Vector2.h"

namespace donner::svg::components {

/**
 * Rendering controller, which instantiates and and manages the rendering tree.
 *
 * Used during the rendering phase in combination with the rendering backend.
 */
class RenderingContext {
public:
  /// Constructor.
  explicit RenderingContext(Registry& registry);

  /**
   * Create the render tree for the document, optionally returning parse warnings found when parsing
   * deferred parts of the tree.
   *
   * @param verbose If true, enable verbose logging.
   * @param outWarnings If non-null, warnings will be added to this vector.
   */
  void instantiateRenderTree(bool verbose, std::vector<ParseError>* outWarnings);

  /**
   * Find the first entity that intersects the given point.
   *
   * @param point Point to find the intersecting entity for
   */
  Entity findIntersecting(const Vector2d& point);

  /**
   * Invalidate the rendering tree, forcing it to be recreated on the next render.
   */
  void invalidateRenderTree();

  /**
   * Set initial context-fill and context-stroke values for sub-document rendering.
   * When a `<use>` element references an external SVG, the `<use>` element's resolved fill
   * and stroke become the initial context-fill/context-stroke in the sub-document.
   *
   * Values are type-erased `ResolvedPaintServer` stored as `std::any` to avoid header
   * dependencies on component types.
   *
   * @param fill Initial context-fill value (std::any containing ResolvedPaintServer).
   * @param stroke Initial context-stroke value (std::any containing ResolvedPaintServer).
   */
  void setInitialContextPaint(std::any fill, std::any stroke);

private:
  /**
   * Create all computed parts of the tree, evaluating styles and creating shadow trees.
   *
   * @param outWarnings If non-null, warnings will be added to this vector.
   */
  void createComputedComponents(std::vector<ParseError>* outWarnings);

  /**
   * Creates all rendering instances for the document, the final step before it can be rendered.
   *
   * @param verbose If true, enable verbose logging.
   */
  void instantiateRenderTreeWithPrecomputedTree(bool verbose);

private:
  /// Reference to the registry containing the render tree.
  Registry& registry_;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)

  /// Initial context-fill value for sub-document rendering (type-erased ResolvedPaintServer).
  std::any initialContextFill_;
  /// Initial context-stroke value for sub-document rendering (type-erased ResolvedPaintServer).
  std::any initialContextStroke_;
};

}  // namespace donner::svg::components
