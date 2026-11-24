#include "donner/svg/renderer/tests/ComputedStyleSnapshot.h"

#include <sstream>
#include <utility>

#include "donner/base/EcsRegistry.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/components/shadow/ComputedShadowTreeComponent.h"

namespace donner::svg::components {
namespace {

void CollectPositionedEntities(const Registry& registry, Entity entity, const std::string& position,
                               std::vector<PositionedEntity>& entities) {
  entities.push_back(PositionedEntity{.position = position, .entity = entity});

  int childIndex = 0;
  for (Entity child = registry.get<donner::components::TreeComponent>(entity).firstChild();
       child != entt::null;
       child = registry.get<donner::components::TreeComponent>(child).nextSibling()) {
    std::ostringstream childPosition;
    childPosition << position << "/"
                  << registry.get<donner::components::TreeComponent>(child).tagName() << "["
                  << childIndex++ << "]";
    CollectPositionedEntities(registry, child, childPosition.str(), entities);
  }

  // A `<use>` main shadow tree hangs off its host as a child; offscreen trees (paint servers,
  // markers, masks) are unattached, so visit them through the host's branches.
  if (const auto* shadow = registry.try_get<ComputedShadowTreeComponent>(entity)) {
    for (size_t branch = 0; branch < shadow->offscreenShadowCount(); ++branch) {
      if (const Entity root = shadow->offscreenShadowRoot(branch); root != entt::null) {
        CollectPositionedEntities(registry, root, position + "~branch " + std::to_string(branch),
                                  entities);
      }
    }
  }
}

}  // namespace

std::ostream& operator<<(std::ostream& os, const PositionedStyle& value) {
  os << value.position << ": ";
  if (value.properties) {
    os << *value.properties;
  } else {
    os << "(no computed style)";
  }
  if (const auto& opacities = value.resourceOpacities) {
    os << " stopOpacity=" << opacities->stopOpacity << " (" << opacities->stopSpecificity
       << ") floodOpacity=" << opacities->floodOpacity << " (" << opacities->floodSpecificity
       << ")";
  }
  return os << (value.externalFill ? " externalFill" : "")
            << (value.externalStroke ? " externalStroke" : "");
}

std::vector<PositionedEntity> PositionedEntities(SVGDocument& document) {
  const Registry& registry = document.registry();
  std::vector<PositionedEntity> entities;
  CollectPositionedEntities(registry, registry.ctx().get<SVGDocumentContext>().rootEntity, "svg",
                            entities);
  return entities;
}

std::vector<PositionedStyle> PositionedStyles(SVGDocument& document) {
  const Registry& registry = document.registry();
  std::vector<PositionedStyle> styles;
  for (const PositionedEntity& positioned : PositionedEntities(document)) {
    PositionedStyle entry{.position = positioned.position};
    if (const auto* style = registry.try_get<ComputedStyleComponent>(positioned.entity)) {
      if (const PropertyRegistry* properties = style->properties()) {
        entry.properties = *properties;
      }
      if (const ComputedResourceOpacities* opacities = style->resourceOpacities()) {
        entry.resourceOpacities = *opacities;
      }
      entry.externalFill = style->externalFill;
      entry.externalStroke = style->externalStroke;
    }
    styles.push_back(std::move(entry));
  }
  return styles;
}

}  // namespace donner::svg::components
