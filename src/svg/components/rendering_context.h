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
   * @param outWarnings If non-null, warnings will be added to this vector.
   */
  void instantiateRenderTree(std::vector<ParseError>* outWarnings);

private:
  Registry& registry_;

  // Rendering signal handlers.
  entt::sigh<void(Registry&, std::vector<ParseError>*)> computePaths_;
};

}  // namespace donner::svg
