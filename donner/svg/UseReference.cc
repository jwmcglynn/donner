#include "donner/svg/UseReference.h"

#include <optional>

#include "donner/base/RcString.h"
#include "donner/base/xml/components/AttributesComponent.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/resources/ImageComponent.h"
#include "donner/svg/components/shadow/ComputedShadowTreeComponent.h"
#include "donner/svg/components/shadow/ShadowTreeComponent.h"
#include "donner/svg/components/shadow/ShadowTreeSystem.h"
#include "donner/svg/graph/Reference.h"
#include "donner/svg/renderer/RenderingContext.h"

namespace donner::svg {

void InvalidateUseInstance(EntityHandle handle) {
  components::LayoutSystem().invalidate(handle);
  components::ShadowTreeSystem().teardownInstances(handle);
  handle.remove<components::ComputedShadowTreeComponent, components::ExternalUseComponent>();
  components::RenderingContext(*handle.registry()).invalidateRenderTree();
}

namespace {

bool IsExternalHref(const std::optional<RcString>& href) {
  return href.has_value() && !href->empty() && Reference(*href).isExternal();
}

/**
 * Whether retargeting \p handle to \p newHref can leave the rest of the render tree in place: the
 * use is presented, draws an external document instance before and after, and owns no shadow tree,
 * so nothing in this document's tree is created or destroyed.
 */
bool CanRetargetInPlace(EntityHandle handle, const std::optional<RcString>& newHref) {
  if (!IsExternalHref(newHref) || handle.all_of<components::ComputedShadowTreeComponent>() ||
      !handle.all_of<components::RenderingInstanceComponent>()) {
    return false;
  }
  const auto* shadowTree = handle.try_get<components::ShadowTreeComponent>();
  return shadowTree == nullptr || IsExternalHref(shadowTree->mainHref());
}

/// Whether \p handle is a presented use that draws an external instance and owns no shadow tree.
bool IsPresentedExternalOnlyUse(EntityHandle handle) {
  const auto* shadowTree = handle.try_get<components::ShadowTreeComponent>();
  return shadowTree != nullptr && IsExternalHref(shadowTree->mainHref()) &&
         !handle.all_of<components::ComputedShadowTreeComponent>() &&
         handle.all_of<components::RenderingInstanceComponent>();
}

}  // namespace

void InvalidateUseGeometry(EntityHandle handle) {
  // An external instance takes the viewport the use establishes when it is drawn, so only the
  // use's own layout is stale. A same-document shadow tree copied that viewport when it was
  // instantiated, so it is rebuilt.
  if (IsPresentedExternalOnlyUse(handle)) {
    components::LayoutSystem().invalidate(handle);
    return;
  }
  InvalidateUseInstance(handle);
}

void RetargetUseFromAttributes(EntityHandle handle) {
  const auto* attributes = handle.try_get<donner::components::AttributesComponent>();
  std::optional<RcString> href;
  if (attributes != nullptr) {
    href = attributes->getAttribute(xml::XMLQualifiedNameRef("href"));
    if (!href) {
      href = attributes->getAttribute(xml::XMLQualifiedNameRef("xlink", "href"));
    }
  }
  const bool inPlace = CanRetargetInPlace(handle, href);
  if (href.value_or(RcString()).empty()) {
    handle.remove<components::ShadowTreeComponent>();
  } else {
    auto& shadowTree = handle.emplace_or_replace<components::ShadowTreeComponent>();
    shadowTree.setMainHref(*href);
    shadowTree.setsContextColors = true;
  }
  if (!inPlace) {
    InvalidateUseInstance(handle);
    return;
  }

  // The next render attaches the new external instance to this use alone.
  components::LayoutSystem().invalidate(handle);
  handle.remove<components::ExternalUseComponent>();
  handle.get_or_emplace<components::DirtyFlagsComponent>().mark(
      components::DirtyFlagsComponent::ShadowTree |
      components::DirtyFlagsComponent::RenderInstance);
}

}  // namespace donner::svg
