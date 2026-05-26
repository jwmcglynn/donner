#include "donner/svg/SVGSymbolElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/PreserveAspectRatioComponent.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/layout/SizedElementComponent.h"
#include "donner/svg/components/layout/SymbolComponent.h"
#include "donner/svg/components/layout/ViewBoxComponent.h"
#include "donner/svg/renderer/RenderingContext.h"

namespace donner::svg {
namespace {

void InvalidateSymbol(EntityHandle handle) {
  components::LayoutSystem().invalidate(handle);
  components::RenderingContext(*handle.registry()).invalidateRenderTree();
}

}  // namespace

SVGSymbolElement SVGSymbolElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  // Set the rendering behavior for a symbol, which is not rendered directly.
  handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::ShadowOnlyChildren);
  handle.emplace<components::ViewBoxComponent>();
  handle.emplace<components::PreserveAspectRatioComponent>();
  handle.emplace<components::SymbolComponent>();
  handle.emplace<components::SizedElementComponent>().canOverrideWidthHeightForSymbol = true;
  return SVGSymbolElement(handle);
}

void SVGSymbolElement::setViewBox(OptionalRef<Box2d> viewBox) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::ViewBoxComponent>(access).viewBox = viewBox;
  InvalidateSymbol(handle_);
}

std::optional<Box2d> SVGSymbolElement::viewBox() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::ViewBoxComponent>();
  return component ? component->viewBox : std::nullopt;
}

void SVGSymbolElement::setPreserveAspectRatio(PreserveAspectRatio preserveAspectRatio) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::PreserveAspectRatioComponent>(access).preserveAspectRatio =
      preserveAspectRatio;
  InvalidateSymbol(handle_);
}

PreserveAspectRatio SVGSymbolElement::preserveAspectRatio() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::PreserveAspectRatioComponent>();
  return component ? component->preserveAspectRatio : PreserveAspectRatio();
}

void SVGSymbolElement::setX(Lengthd value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::SizedElementComponent>(access).properties.x.set(
      value, css::Specificity::Override());
  InvalidateSymbol(handle_);
}

Lengthd SVGSymbolElement::x() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::SizedElementComponent>();
  return component ? component->properties.x.getRequired()
                   : components::SizedElementComponent().properties.x.getRequired();
}

void SVGSymbolElement::setY(Lengthd value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::SizedElementComponent>(access).properties.y.set(
      value, css::Specificity::Override());
  InvalidateSymbol(handle_);
}

Lengthd SVGSymbolElement::y() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::SizedElementComponent>();
  return component ? component->properties.y.getRequired()
                   : components::SizedElementComponent().properties.y.getRequired();
}

void SVGSymbolElement::setWidth(std::optional<Lengthd> value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::SizedElementComponent>(access).properties.width.set(
      value, css::Specificity::Override());
  InvalidateSymbol(handle_);
}

std::optional<Lengthd> SVGSymbolElement::width() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::SizedElementComponent>();
  return component ? component->properties.width.get() : std::nullopt;
}

void SVGSymbolElement::setHeight(std::optional<Lengthd> value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::SizedElementComponent>(access).properties.height.set(
      value, css::Specificity::Override());
  InvalidateSymbol(handle_);
}

std::optional<Lengthd> SVGSymbolElement::height() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::SizedElementComponent>();
  return component ? component->properties.height.get() : std::nullopt;
}

void SVGSymbolElement::setRefX(double value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::SymbolComponent>(access).refX = value;
  InvalidateSymbol(handle_);
}

double SVGSymbolElement::refX() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::SymbolComponent>();
  return component ? component->refX : components::SymbolComponent().refX;
}

void SVGSymbolElement::setRefY(double value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::SymbolComponent>(access).refY = value;
  InvalidateSymbol(handle_);
}

double SVGSymbolElement::refY() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::SymbolComponent>();
  return component ? component->refY : components::SymbolComponent().refY;
}

}  // namespace donner::svg
