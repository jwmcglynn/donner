#pragma once
/// @file

#include <vector>

#include "src/base/rc_string.h"
#include "src/base/vector2.h"
#include "src/svg/components/id_component.h"
#include "src/svg/registry/registry.h"

namespace donner::svg {

class SVGDocument;

class DocumentContext {
public:
  DocumentContext(SVGDocument& document, Registry& registry);

  std::optional<Vector2i> canvasSize;
  Entity rootEntity = entt::null;

  SVGDocument& document() const { return document_; }

  Entity getEntityById(const RcString& id) const {
    const auto it = idToEntity_.find(id);
    return (it != idToEntity_.end()) ? it->second : entt::null;
  }

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
  std::unordered_map<RcString, Entity> idToEntity_;
};

}  // namespace donner::svg
