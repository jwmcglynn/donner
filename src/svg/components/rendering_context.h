#pragma once

#include <vector>

#include "src/base/parser/parse_error.h"
#include "src/base/rc_string.h"
#include "src/base/vector2.h"
#include "src/svg/components/id_component.h"
#include "src/svg/registry/registry.h"

namespace donner::svg {

class SVGDocument;

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
  void instantiateRenderTree(bool verbose, std::vector<ParseError>* outWarnings);

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

  Registry& registry_;

  // Rendering signal handlers.
  entt::sigh<void(Registry&)> evaluateConditionalComponents_;
  entt::sigh<void(Registry&, std::vector<ParseError>*)> instantiateComputedComponents_;
};

}  // namespace donner::svg
