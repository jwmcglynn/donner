#include "src/svg/svg_element.h"

#include "src/css/parser/selector_parser.h"
#include "src/svg/components/class_component.h"
#include "src/svg/components/document_context.h"
#include "src/svg/components/id_component.h"
#include "src/svg/components/style_component.h"
#include "src/svg/components/transform_component.h"
#include "src/svg/components/tree_component.h"
#include "src/svg/svg_document.h"

namespace donner {

namespace {

static std::optional<SVGElement> querySelectorSearch(const css::Selector& selector,
                                                     SVGElement element) {
  // TODO: Add a proper iterator for all children.
  if (selector.matches(element)) {
    return element;
  }

  for (std::optional<SVGElement> child = element.firstChild(); child;
       child = child->nextSibling()) {
    if (auto result = querySelectorSearch(selector, *child)) {
      return *result;
    }
  }

  return std::nullopt;
}

}  // namespace

SVGElement::SVGElement(Registry& registry, Entity entity) : registry_(registry), entity_(entity) {}

SVGElement::SVGElement(const SVGElement& other) = default;
SVGElement& SVGElement::operator=(const SVGElement& other) = default;

ElementType SVGElement::type() const {
  return registry_.get().get<TreeComponent>(entity_).type();
}

RcString SVGElement::typeString() const {
  return registry_.get().get<TreeComponent>(entity_).typeString();
}

Entity SVGElement::entity() const {
  return entity_;
}

RcString SVGElement::id() const {
  if (const auto* component = registry_.get().try_get<IdComponent>(entity_)) {
    return component->id;
  } else {
    return "";
  }
}

void SVGElement::setId(std::string_view id) {
  if (!id.empty()) {
    auto& component = registry_.get().get_or_emplace<IdComponent>(entity_);
    component.id = id;
  } else {
    registry_.get().remove<IdComponent>(entity_);
  }
}

RcString SVGElement::className() const {
  if (const auto* component = registry_.get().try_get<ClassComponent>(entity_)) {
    return component->className;
  } else {
    return "";
  }
}

void SVGElement::setClassName(std::string_view name) {
  if (!name.empty()) {
    auto& component = registry_.get().get_or_emplace<ClassComponent>(entity_);
    component.className = name;
  } else {
    registry_.get().remove<ClassComponent>(entity_);
  }
}

Transformd SVGElement::transform() const {
  if (const auto* component = registry_.get().try_get<TransformComponent>(entity_)) {
    return component->transform;
  } else {
    return Transformd();
  }
}

void SVGElement::setTransform(Transformd transform) {
  auto& component = registry_.get().get_or_emplace<TransformComponent>(entity_);
  component.transform = transform;
}

void SVGElement::setStyle(std::string_view style) {
  registry_.get().get_or_emplace<StyleComponent>(entity_).setStyle(style);
}

bool SVGElement::trySetPresentationAttribute(std::string_view name, std::string_view value) {
  return registry_.get().get_or_emplace<StyleComponent>(entity_).trySetPresentationAttribute(name,
                                                                                             value);
}

bool SVGElement::hasAttribute(std::string_view name) const {
  // TODO
  return false;
}

std::optional<RcString> SVGElement::getAttribute(std::string_view name) const {
  // TODO
  return std::nullopt;
}

SVGDocument& SVGElement::ownerDocument() {
  return registry_.get().ctx<DocumentContext>().document;
}

std::optional<SVGElement> SVGElement::parentElement() const {
  Registry& registry = registry_.get();
  const auto& tree = registry.get<TreeComponent>(entity_);
  return tree.parent()
             ? std::make_optional(SVGElement(registry, entt::to_entity(registry, *tree.parent())))
             : std::nullopt;
}

std::optional<SVGElement> SVGElement::firstChild() const {
  Registry& registry = registry_.get();
  const auto& tree = registry.get<TreeComponent>(entity_);
  return tree.firstChild() ? std::make_optional(SVGElement(
                                 registry, entt::to_entity(registry, *tree.firstChild())))
                           : std::nullopt;
}

std::optional<SVGElement> SVGElement::lastChild() const {
  Registry& registry = registry_.get();
  const auto& tree = registry.get<TreeComponent>(entity_);
  return tree.lastChild() ? std::make_optional(
                                SVGElement(registry, entt::to_entity(registry, *tree.lastChild())))
                          : std::nullopt;
}

std::optional<SVGElement> SVGElement::previousSibling() const {
  Registry& registry = registry_.get();
  const auto& tree = registry.get<TreeComponent>(entity_);
  return tree.previousSibling() ? std::make_optional(SVGElement(
                                      registry, entt::to_entity(registry, *tree.previousSibling())))
                                : std::nullopt;
}

std::optional<SVGElement> SVGElement::nextSibling() const {
  Registry& registry = registry_.get();
  const auto& tree = registry.get<TreeComponent>(entity_);
  return tree.nextSibling() ? std::make_optional(SVGElement(
                                  registry, entt::to_entity(registry, *tree.nextSibling())))
                            : std::nullopt;
}

SVGElement SVGElement::insertBefore(SVGElement newNode, std::optional<SVGElement> referenceNode) {
  Registry& registry = registry_.get();
  registry.get<TreeComponent>(entity_).insertBefore(
      &registry.get<TreeComponent>(newNode.entity_),
      referenceNode ? &registry.get<TreeComponent>(referenceNode->entity_) : nullptr);
  return newNode;
}

SVGElement SVGElement::appendChild(SVGElement child) {
  Registry& registry = registry_.get();
  registry.get<TreeComponent>(entity_).appendChild(&registry.get<TreeComponent>(child.entity_));
  return child;
}

SVGElement SVGElement::replaceChild(SVGElement newChild, SVGElement oldChild) {
  Registry& registry = registry_.get();
  registry.get<TreeComponent>(entity_).replaceChild(&registry.get<TreeComponent>(newChild.entity_),
                                                    &registry.get<TreeComponent>(oldChild.entity_));
  return newChild;
}

SVGElement SVGElement::removeChild(SVGElement child) {
  Registry& registry = registry_.get();
  registry.get<TreeComponent>(entity_).removeChild(&registry.get<TreeComponent>(child.entity_));
  return child;
}

void SVGElement::remove() {
  registry_.get().get<TreeComponent>(entity_).remove();
}

std::optional<SVGElement> SVGElement::querySelector(std::string_view str) {
  const ParseResult<css::Selector> selectorResult = css::SelectorParser::Parse(str);
  if (selectorResult.hasError()) {
    return std::nullopt;
  }

  const css::Selector selector = std::move(selectorResult.result());
  return querySelectorSearch(selector, *this);
}

Entity SVGElement::CreateEntity(Registry& registry, RcString typeString, ElementType type) {
  Entity entity = registry.create();
  registry.emplace<TreeComponent>(entity, type, std::move(typeString));
  return entity;
}

}  // namespace donner
