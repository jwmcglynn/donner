#pragma once

#include <vector>

#include "src/base/parser/parse_error.h"
#include "src/base/rc_string.h"
#include "src/base/vector2.h"
#include "src/svg/components/id_component.h"
#include "src/svg/components/registry.h"

namespace donner::svg {

class SVGDocument;

class DocumentContext {
public:
  DocumentContext(SVGDocument& document, Registry& registry);

  std::optional<Vector2d> defaultSize;

  SVGDocument& document() const { return document_; }

  Entity getEntityById(const RcString& id) const {
    const auto it = idToEntity_.find(id);
    return (it != idToEntity_.end()) ? it->second : entt::null;
  }

  /**
   * Create the render tree for the document, optionally returning parse warnings found when parsing
   * deferred parts of the tree.
   *
   * @param outWarnings If non-null, warnings will be added to this vector.
   */
  void instantiateRenderTree(std::vector<ParseError>* outWarnings);

private:
  void onIdSet(Registry& registry, Entity entity) {
    auto& idComponent = registry.get<IdComponent>(entity);
    idToEntity_.emplace(idComponent.id, entity);
  }

  void onIdDestroy(Registry& registry, Entity entity) {
    auto& idComponent = registry.get<IdComponent>(entity);
    idToEntity_.erase(idComponent.id);
  }

  SVGDocument& document_;
  Registry& registry_;
  std::unordered_map<RcString, Entity> idToEntity_;

  // Rendering signal handlers.
  entt::sigh<void(Registry&, std::vector<ParseError>*)> computePaths_;
};

}  // namespace donner::svg
