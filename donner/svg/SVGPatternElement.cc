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
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::ViewBoxComponent>(access).viewBox = viewBox;
  InvalidatePattern(handle_);
}

void SVGPatternElement::setPreserveAspectRatio(PreserveAspectRatio preserveAspectRatio) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::PreserveAspectRatioComponent>(access).preserveAspectRatio =
      preserveAspectRatio;
  InvalidatePattern(handle_);
}

void SVGPatternElement::setX(Lengthd value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::PatternComponent>(access).sizeProperties.x.set(
      value, css::Specificity::Override());
  InvalidatePattern(handle_);
}

void SVGPatternElement::setY(Lengthd value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::PatternComponent>(access).sizeProperties.y.set(
      value, css::Specificity::Override());
  InvalidatePattern(handle_);
}

void SVGPatternElement::setWidth(OptionalRef<Lengthd> value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::PatternComponent>(access).sizeProperties.width.set(
      value, css::Specificity::Override());
  InvalidatePattern(handle_);
}

void SVGPatternElement::setHeight(OptionalRef<Lengthd> value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::PatternComponent>(access).sizeProperties.height.set(
      value, css::Specificity::Override());
  InvalidatePattern(handle_);
}

void SVGPatternElement::setPatternUnits(PatternUnits value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::PatternComponent>(access).patternUnits = value;
  InvalidatePattern(handle_);
}

void SVGPatternElement::setPatternContentUnits(PatternContentUnits value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::PatternComponent>(access).patternContentUnits = value;
  InvalidatePattern(handle_);
}

void SVGPatternElement::setPatternTransform(Transform2d transform) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  components::LayoutSystem().setRawEntityFromParentTransform(handle_, transform);
  InvalidatePattern(handle_);
}

void SVGPatternElement::setHref(OptionalRef<RcStringOrRef> value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  if (value) {
    handle_.get_or_emplace<components::PatternComponent>(access).href = RcString(value.value());
  } else {
    handle_.get_or_emplace<components::PatternComponent>(access).href = std::nullopt;
  }

  InvalidatePattern(handle_);
}

}  // namespace donner::svg
