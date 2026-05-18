#include "donner/svg/SVGGradientElement.h"

#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/paint/GradientComponent.h"
#include "donner/svg/components/shadow/ComputedShadowTreeComponent.h"
#include "donner/svg/renderer/RenderingContext.h"

namespace donner::svg {

SVGGradientElement::SVGGradientElement(EntityHandle handle) : SVGElement(handle) {
  handle_.emplace<components::GradientComponent>();
}

std::optional<RcString> SVGGradientElement::href() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::GradientComponent>();
  const std::optional<Reference> maybeHref = component ? component->href : std::nullopt;
  if (maybeHref) {
    return maybeHref.value().href;
  } else {
    return std::nullopt;
  }
}

GradientUnits SVGGradientElement::gradientUnits() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::GradientComponent>();
  return component ? component->gradientUnits.value_or(GradientUnits::Default)
                   : GradientUnits::Default;
}

Transform2d SVGGradientElement::gradientTransform() const {
  [[maybe_unused]] DocumentWriteAccess access = handle_.writeAccess();
  return components::LayoutSystem().getRawEntityFromParentTransform(handle_);
}

GradientSpreadMethod SVGGradientElement::spreadMethod() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::GradientComponent>();
  return component ? component->spreadMethod.value_or(GradientSpreadMethod::Default)
                   : GradientSpreadMethod::Default;
}

void SVGGradientElement::setHref(const std::optional<RcString>& value) {
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.get_or_emplace<components::GradientComponent>().href = value;
  // Force the shadow tree to be regenerated.
  handle_.remove<components::ComputedShadowTreeComponent>();
  invalidate();
  access.bumpMutationRevision();
}

void SVGGradientElement::setGradientUnits(GradientUnits value) {
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.get_or_emplace<components::GradientComponent>().gradientUnits = value;
  invalidate();
  access.bumpMutationRevision();
}

void SVGGradientElement::setGradientTransform(const Transform2d& value) {
  DocumentWriteAccess access = handle_.writeAccess();
  components::LayoutSystem().setRawEntityFromParentTransform(handle_, value);
  invalidate();
  access.bumpMutationRevision();
}

void SVGGradientElement::setSpreadMethod(GradientSpreadMethod value) {
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.get_or_emplace<components::GradientComponent>().spreadMethod = value;
  invalidate();
  access.bumpMutationRevision();
}

void SVGGradientElement::invalidate() const {
  [[maybe_unused]] DocumentWriteAccess access = handle_.writeAccess();
  components::RenderingContext(*handle_.registry()).invalidateRenderTree();
}

}  // namespace donner::svg
