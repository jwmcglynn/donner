#pragma once
/// @file

#include <vector>

#include "donner/base/RcString.h"
#include "donner/base/Vector2.h"
#include "donner/base/parser/ParseError.h"
#include "donner/svg/components/IdComponent.h"
#include "donner/svg/registry/Registry.h"

namespace donner::svg::components {

class RenderingContext {
public:
  explicit RenderingContext(Registry& registry);

  /**
   * Create the render tree for the document, optionally returning parse warnings found when parsing
   * deferred parts of the tree.
   *
   * @param verbose If true, enable verbose logging.
   * @param outWarnings If non-null, warnings will be added to this vector.
   */
  void instantiateRenderTree(bool verbose, std::vector<parser::ParseError>* outWarnings);

  /**
   * Find the first entity that intersects the given point.
   *
   * @param registry Registry used to find all shape components
   * @param point Point to find the intersecting entity for
   */
  Entity findIntersecting(const Vector2d& point);

private:
  /**
   * Create all computed parts of the tree, evaluating styles and creating shadow trees.
   *
   * @param outWarnings If non-null, warnings will be added to this vector.
   */
  void createComputedComponents(std::vector<parser::ParseError>* outWarnings);

  /**
   * Creates all rendering instances for the document, the final step before it can be rendered.
   *
   * @param verbose If true, enable verbose logging.
   */
  void instantiateRenderTreeWithPrecomputedTree(bool verbose);

  Registry& registry_;
};

}  // namespace donner::svg::components
