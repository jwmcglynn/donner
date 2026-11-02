#include "donner/svg/UseReference.h"

#include <optional>

#include "donner/base/RcString.h"
#include "donner/base/xml/components/AttributesComponent.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/resources/ImageComponent.h"
#include "donner/svg/components/shadow/ComputedShadowTreeComponent.h"
#include "donner/svg/components/shadow/ShadowTreeComponent.h"
#include "donner/svg/components/shadow/ShadowTreeSystem.h"
#include "donner/svg/renderer/RenderingContext.h"

namespace donner::svg {

void InvalidateUseInstance(EntityHandle handle) {
  components::LayoutSystem().invalidate(handle);
  components::ShadowTreeSystem().teardownInstances(handle);
  handle.remove<components::ComputedShadowTreeComponent, components::ExternalUseComponent>();
  components::RenderingContext(*handle.registry()).invalidateRenderTree();
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
  if (href.value_or(RcString()).empty()) {
    handle.remove<components::ShadowTreeComponent>();
  } else {
    auto& shadowTree = handle.emplace_or_replace<components::ShadowTreeComponent>();
    shadowTree.setMainHref(*href);
    shadowTree.setsContextColors = true;
  }
  InvalidateUseInstance(handle);
}

}  // namespace donner::svg
