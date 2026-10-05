#include "donner/svg/internal/RenderingDiagnostics.h"

#include <array>
#include <utility>

#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/IdComponent.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"

namespace donner::svg::internal {
namespace {

std::string RenderDiagnosticLabel(const Registry& registry, Entity entity) {
  if (!registry.valid(entity)) {
    return "null";
  }
  std::string label = "entity";
  if (const auto* tree = registry.try_get<donner::components::TreeComponent>(entity)) {
    label = std::string(tree->tagName().name);
  }
  if (const auto* id = registry.try_get<components::IdComponent>(entity);
      id != nullptr && !id->id().empty()) {
    label.push_back('#');
    label.append(std::string_view(id->id()));
  }
  label.append(" #");
  label.append(std::to_string(static_cast<std::uint32_t>(entity)));
  return label;
}

std::vector<std::string_view> RenderDiagnosticDirtyNames(std::uint16_t flags) {
  using D = components::DirtyFlagsComponent;
  constexpr std::array<std::pair<D::Flags, std::string_view>, 10> names{{
      {D::Style, "style"},
      {D::Layout, "layout"},
      {D::Transform, "transform"},
      {D::WorldTransform, "world_transform"},
      {D::Shape, "shape"},
      {D::Paint, "paint"},
      {D::Filter, "filter"},
      {D::RenderInstance, "render_instance"},
      {D::ShadowTree, "shadow_tree"},
      {D::TextGeometry, "text_geometry"},
  }};
  std::vector<std::string_view> result;
  for (const auto& [flag, name] : names) {
    if ((flags & flag) != 0) {
      result.push_back(name);
    }
  }
  return result;
}

RenderingDiagnostics::Instance RenderDiagnosticInstance(const Registry& registry, Entity entity) {
  const auto& instance = registry.get<components::RenderingInstanceComponent>(entity);
  RenderingDiagnostics::Instance out;
  out.entity = entity;
  out.dataEntity = instance.dataEntity;
  out.label = RenderDiagnosticLabel(registry, entity);
  out.dataLabel = RenderDiagnosticLabel(registry, instance.dataEntity);
  out.drawOrder = instance.drawOrder;
  out.visible = instance.visible;
  if (const auto* style = registry.try_get<components::ComputedStyleComponent>(entity);
      style != nullptr && style->properties.has_value()) {
    out.style = RenderingDiagnostics::StyleState{
        .displayNone = style->properties->display.get().value() == Display::None,
        .visibility = static_cast<int>(style->properties->visibility.get().value()),
    };
  }
  return out;
}
}  // namespace

RenderingDiagnostics CaptureRenderingDiagnostics(const SVGDocument& document,
                                                 std::size_t maxRecords) {
  DocumentReadAccess access = document.readAccess();
  const Registry& registry = access.registry();
  RenderingDiagnostics result;
  if (const auto* state = registry.ctx().find<components::RenderTreeState>()) {
    result.state = RenderingDiagnostics::TreeState{
        .hasBeenBuilt = state->hasBeenBuilt,
        .needsFullRebuild = state->needsFullRebuild,
        .needsFullStyleRecompute = state->needsFullStyleRecompute,
    };
  }
  for (const Entity entity : registry.view<const components::DirtyFlagsComponent>()) {
    ++result.dirtyCount;
    if (result.dirtyEntities.size() < maxRecords) {
      const auto flags = registry.get<components::DirtyFlagsComponent>(entity).flags;
      result.dirtyEntities.push_back({entity, flags, RenderDiagnosticDirtyNames(flags)});
    }
  }
  for (const Entity entity : registry.view<const components::RenderingInstanceComponent>()) {
    ++result.instanceCount;
    if (result.instances.size() < maxRecords) {
      result.instances.push_back(RenderDiagnosticInstance(registry, entity));
    }
  }
  return result;
}

}  // namespace donner::svg::internal
