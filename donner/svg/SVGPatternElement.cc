#include "donner/svg/SVGPatternElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/PreserveAspectRatioComponent.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/layout/ViewBoxComponent.h"
#include "donner/svg/components/paint/PatternComponent.h"
#include "donner/svg/components/shadow/ComputedShadowTreeComponent.h"
#include "donner/svg/components/style/DoNotInheritFillOrStrokeTag.h"
#include "donner/svg/core/PreserveAspectRatio.h"
#include "donner/svg/renderer/RenderingContext.h"

namespace donner::svg {
namespace {

void InvalidatePattern(EntityHandle handle) {
  components::LayoutSystem().invalidate(handle);
  handle.remove<components::ComputedPatternComponent>();
  handle.remove<components::ComputedShadowTreeComponent>();
  components::RenderingContext(*handle.registry()).invalidateRenderTree();
}

}  // namespace

SVGPatternElement SVGPatternElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);

  auto& renderingBehavior = handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::ShadowOnlyChildren);
  renderingBehavior.inheritsParentTransform = false;
  renderingBehavior.appliesSelfTransform = false;

  handle.emplace<components::PatternComponent>();
  handle.emplace<components::DoNotInheritFillOrStrokeTag>();
  handle.emplace<components::ViewBoxComponent>();
  handle.emplace<components::PreserveAspectRatioComponent>();
  return SVGPatternElement(handle);
}

std::optional<Box2d> SVGPatternElement::viewBox() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::ViewBoxComponent>();
  return component ? component->viewBox : std::nullopt;
}

PreserveAspectRatio SVGPatternElement::preserveAspectRatio() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::PreserveAspectRatioComponent>();
  return component ? component->preserveAspectRatio : PreserveAspectRatio();
}

Lengthd SVGPatternElement::x() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::PatternComponent>();
  return component ? component->sizeProperties.x.getRequired()
                   : components::PatternComponent().sizeProperties.x.getRequired();
}

Lengthd SVGPatternElement::y() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::PatternComponent>();
  return component ? component->sizeProperties.y.getRequired()
                   : components::PatternComponent().sizeProperties.y.getRequired();
}

std::optional<Lengthd> SVGPatternElement::width() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::PatternComponent>();
  return component ? component->sizeProperties.width.get() : std::nullopt;
}

std::optional<Lengthd> SVGPatternElement::height() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::PatternComponent>();
  return component ? component->sizeProperties.height.get() : std::nullopt;
}

PatternUnits SVGPatternElement::patternUnits() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::PatternComponent>();
  return component ? component->patternUnits.value_or(PatternUnits::Default)
                   : PatternUnits::Default;
}

PatternContentUnits SVGPatternElement::patternContentUnits() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::PatternComponent>();
  return component ? component->patternContentUnits.value_or(PatternContentUnits::Default)
                   : PatternContentUnits::Default;
}

Transform2d SVGPatternElement::patternTransform() const {
  [[maybe_unused]] DocumentWriteAccess access = handle_.writeAccess();
  return components::LayoutSystem().getRawEntityFromParentTransform(handle_);
}

std::optional<RcString> SVGPatternElement::href() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::PatternComponent>();
  const std::optional<Reference> maybeHref = component ? component->href : std::nullopt;
  if (maybeHref) {
    return maybeHref.value().href;
  } else {
    return std::nullopt;
  }
}

void SVGPatternElement::setViewBox(OptionalRef<Box2d> viewBox) {
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.get_or_emplace<components::ViewBoxComponent>().viewBox = viewBox;
  InvalidatePattern(handle_);
  access.bumpMutationRevision();
}

void SVGPatternElement::setPreserveAspectRatio(PreserveAspectRatio preserveAspectRatio) {
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.get_or_emplace<components::PreserveAspectRatioComponent>().preserveAspectRatio =
      preserveAspectRatio;
  InvalidatePattern(handle_);
  access.bumpMutationRevision();
}

void SVGPatternElement::setX(Lengthd value) {
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.get_or_emplace<components::PatternComponent>().sizeProperties.x.set(
      value, css::Specificity::Override());
  InvalidatePattern(handle_);
  access.bumpMutationRevision();
}

void SVGPatternElement::setY(Lengthd value) {
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.get_or_emplace<components::PatternComponent>().sizeProperties.y.set(
      value, css::Specificity::Override());
  InvalidatePattern(handle_);
  access.bumpMutationRevision();
}

void SVGPatternElement::setWidth(OptionalRef<Lengthd> value) {
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.get_or_emplace<components::PatternComponent>().sizeProperties.width.set(
      value, css::Specificity::Override());
  InvalidatePattern(handle_);
  access.bumpMutationRevision();
}

void SVGPatternElement::setHeight(OptionalRef<Lengthd> value) {
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.get_or_emplace<components::PatternComponent>().sizeProperties.height.set(
      value, css::Specificity::Override());
  InvalidatePattern(handle_);
  access.bumpMutationRevision();
}

void SVGPatternElement::setPatternUnits(PatternUnits value) {
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.get_or_emplace<components::PatternComponent>().patternUnits = value;
  InvalidatePattern(handle_);
  access.bumpMutationRevision();
}

void SVGPatternElement::setPatternContentUnits(PatternContentUnits value) {
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.get_or_emplace<components::PatternComponent>().patternContentUnits = value;
  InvalidatePattern(handle_);
  access.bumpMutationRevision();
}

void SVGPatternElement::setPatternTransform(Transform2d transform) {
  DocumentWriteAccess access = handle_.writeAccess();
  components::LayoutSystem().setRawEntityFromParentTransform(handle_, transform);
  InvalidatePattern(handle_);
  access.bumpMutationRevision();
}

void SVGPatternElement::setHref(OptionalRef<RcStringOrRef> value) {
  DocumentWriteAccess access = handle_.writeAccess();
  if (value) {
    handle_.get_or_emplace<components::PatternComponent>().href = RcString(value.value());
  } else {
    handle_.get_or_emplace<components::PatternComponent>().href = std::nullopt;
  }

  InvalidatePattern(handle_);
  access.bumpMutationRevision();
}

}  // namespace donner::svg
