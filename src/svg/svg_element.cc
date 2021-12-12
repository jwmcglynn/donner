#include "src/svg/svg_element.h"

#include <string>

#include "src/svg/components/class_component.h"
#include "src/svg/components/id_component.h"
#include "src/svg/components/style_component.h"
#include "src/svg/components/transform_component.h"
#include "src/svg/components/tree_component.h"

namespace donner {

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

std::string SVGElement::id() const {
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
    registry_.get().remove_if_exists<IdComponent>(entity_);
  }
}

std::string SVGElement::className() const {
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
    registry_.get().remove_if_exists<ClassComponent>(entity_);
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

std::optional<SVGElement> SVGElement::parentElement() {
  auto& tree = registry_.get().get<TreeComponent>(entity_);
  return tree.parent() != entt::null ? std::make_optional(SVGElement(registry_, tree.parent()))
                                     : std::nullopt;
}

std::optional<SVGElement> SVGElement::firstChild() {
  auto& tree = registry_.get().get<TreeComponent>(entity_);
  return tree.firstChild() != entt::null
             ? std::make_optional(SVGElement(registry_, tree.firstChild()))
             : std::nullopt;
}

std::optional<SVGElement> SVGElement::lastChild() {
  auto& tree = registry_.get().get<TreeComponent>(entity_);
  return tree.lastChild() != entt::null
             ? std::make_optional(SVGElement(registry_, tree.lastChild()))
             : std::nullopt;
}

std::optional<SVGElement> SVGElement::previousSibling() {
  auto& tree = registry_.get().get<TreeComponent>(entity_);
  return tree.previousSibling() != entt::null
             ? std::make_optional(SVGElement(registry_, tree.previousSibling()))
             : std::nullopt;
}

std::optional<SVGElement> SVGElement::nextSibling() {
  auto& tree = registry_.get().get<TreeComponent>(entity_);
  return tree.nextSibling() != entt::null
             ? std::make_optional(SVGElement(registry_, tree.nextSibling()))
             : std::nullopt;
}

SVGElement SVGElement::insertBefore(SVGElement newNode, std::optional<SVGElement> referenceNode) {
  registry_.get().get<TreeComponent>(entity_).insertBefore(
      registry_, newNode.entity_, referenceNode ? referenceNode->entity_ : entt::null);
  return newNode;
}

SVGElement SVGElement::appendChild(SVGElement child) {
  registry_.get().get<TreeComponent>(entity_).appendChild(registry_, child.entity_);
  return child;
}

SVGElement SVGElement::replaceChild(SVGElement newChild, SVGElement oldChild) {
  registry_.get().get<TreeComponent>(entity_).replaceChild(registry_, newChild.entity_,
                                                           oldChild.entity_);
  return newChild;
}

SVGElement SVGElement::removeChild(SVGElement child) {
  registry_.get().get<TreeComponent>(entity_).removeChild(registry_, child.entity_);
  return child;
}

void SVGElement::remove() {
  registry_.get().get<TreeComponent>(entity_).remove(registry_);
}

Entity SVGElement::CreateEntity(Registry& registry, RcString typeString, ElementType type) {
  Entity entity = registry.create();
  registry.emplace<TreeComponent>(entity, type, std::move(typeString), entity);
  return entity;
}

}  // namespace donner