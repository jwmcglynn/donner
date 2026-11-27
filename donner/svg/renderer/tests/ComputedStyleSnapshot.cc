#include "donner/svg/renderer/tests/ComputedStyleSnapshot.h"

#include <sstream>
#include <unordered_map>
#include <utility>

#include "donner/base/EcsRegistry.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
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

bool PositionedStyle::operator==(const PositionedStyle& other) const {
  if (position != other.position ||
      static_cast<bool>(properties) != static_cast<bool>(other.properties) ||
      resourceOpacities != other.resourceOpacities || externalFill != other.externalFill ||
      externalStroke != other.externalStroke) {
    return false;
  }
  return !properties || properties->exactlyEquals(*other.properties);
}

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
  const auto positions = PositionedEntities(document);
  styles.reserve(positions.size());
  std::unordered_map<const PropertyRegistry*, std::shared_ptr<const PropertyRegistry>> captured;
  for (const auto& positioned : positions) {
    PositionedStyle entry{.position = positioned.position};
    if (const auto* style = registry.try_get<ComputedStyleComponent>(positioned.entity)) {
      if (const PropertyRegistry* properties = style->properties()) {
        auto [value, inserted] = captured.try_emplace(properties);
        if (inserted) {
          value->second = std::make_shared<const PropertyRegistry>(*properties);
        }
        entry.properties = value->second;
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

// Normalize reference targets by tree position, since the two documents own different entities.
std::vector<PositionedRenderInstance> PositionedRenderInstances(SVGDocument& document) {
  const Registry& registry = document.registry();
  const auto positions = PositionedEntities(document);
  std::unordered_map<Entity, std::string_view> byEntity;
  byEntity.reserve(positions.size());
  for (const auto& item : positions) {
    byEntity.try_emplace(item.entity, item.position);
  }
  const auto position = [&](Entity entity) -> std::string {
    const auto found = byEntity.find(entity);
    return found == byEntity.end() ? std::string() : std::string(found->second);
  };
  const auto paintReference = [&](const ResolvedPaintServer& paint) {
    const auto* reference = std::get_if<PaintResolvedReference>(&paint);
    return reference ? position(reference->reference) : std::string();
  };
  const auto referenceTarget = [&](const auto& reference) {
    return reference ? position(reference->reference) : std::string();
  };
  std::vector<PositionedRenderInstance> result;
  for (const auto& entity : positions) {
    const auto* instance = registry.try_get<RenderingInstanceComponent>(entity.entity);
    if (!instance) {
      continue;
    }
    const auto* filter = instance->resolvedFilter
                             ? std::get_if<ResolvedReference>(&*instance->resolvedFilter)
                             : nullptr;
    result.emplace_back(
        entity.position, instance->drawOrder, instance->visible, instance->isolatedLayer,
        position(instance->subtreeLastEntity), instance->worldFromEntityTransform,
        std::array{paintReference(instance->resolvedFill), paintReference(instance->resolvedStroke),
                   referenceTarget(instance->clipPath), referenceTarget(instance->mask),
                   referenceTarget(instance->markerStart), referenceTarget(instance->markerMid),
                   referenceTarget(instance->markerEnd),
                   filter ? position(*filter) : std::string()});
  }
  return result;
}

}  // namespace donner::svg::components
