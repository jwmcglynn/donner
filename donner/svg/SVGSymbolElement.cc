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
  auto& renderingBehavior = handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::ShadowOnlyChildren);
  // The resvg suite targets SVG 1.1, where `<symbol>` cannot carry a `transform` — it is
  // ignored. (SVG 2 allows it; revisit if the suite moves to SVG 2 semantics.)
  renderingBehavior.appliesSelfTransform = false;
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
  const auto* component = handle_.try_get<components::SizedElementComponent>();
  return component ? component->properties.x.get().value()
                   : components::SizedElementComponent().properties.x.get().value();
}

void SVGSymbolElement::setY(Lengthd value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::SizedElementComponent>(access).properties.y.set(
      value, css::Specificity::Override());
  InvalidateSymbol(handle_);
}

Lengthd SVGSymbolElement::y() const {
  const auto* component = handle_.try_get<components::SizedElementComponent>();
  return component ? component->properties.y.get().value()
                   : components::SizedElementComponent().properties.y.get().value();
}

void SVGSymbolElement::setWidth(std::optional<Lengthd> value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::SizedElementComponent>(access).properties.width.set(
      value, css::Specificity::Override());
  InvalidateSymbol(handle_);
}

std::optional<Lengthd> SVGSymbolElement::width() const {
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
  const auto* component = handle_.try_get<components::SymbolComponent>();
  return component ? component->refY : components::SymbolComponent().refY;
}

}  // namespace donner::svg
