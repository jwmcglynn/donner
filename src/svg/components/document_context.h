#pragma once

#include "src/base/rc_string.h"
#include "src/base/vector2.h"
#include "src/svg/components/id_component.h"
#include "src/svg/components/registry.h"

namespace donner::svg {

class SVGDocument;

struct DocumentContext {
  DocumentContext(SVGDocument& document, Registry& registry) : document(document) {
    registry.on_construct<IdComponent>().connect<&DocumentContext::onIdSet>(this);
    registry.on_destroy<IdComponent>().connect<&DocumentContext::onIdDestroy>(this);
  }

  SVGDocument& document;
  std::optional<Vector2d> defaultSize;

  std::unordered_map<RcString, Entity> idToEntity;

private:
  void onIdSet(Registry& registry, Entity entity) {
    auto& idComponent = registry.get<IdComponent>(entity);
    idToEntity.emplace(idComponent.id, entity);
  }

  void onIdDestroy(Registry& registry, Entity entity) {
    auto& idComponent = registry.get<IdComponent>(entity);
    idToEntity.erase(idComponent.id);
  }
};

}  // namespace donner::svg
