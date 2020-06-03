#include "src/svg/svg_element.h"

#include <string>

#include "src/svg/components/id_component.h"
#include "src/svg/components/path_component.h"
#include "src/svg/components/tree_component.h"

namespace donner {

SVGElement::SVGElement(Registry& registry, Entity entity) : registry_(registry), entity_(entity) {}

SVGElement::SVGElement(const SVGElement& other) = default;
SVGElement& SVGElement::operator=(const SVGElement& other) = default;

ElementType SVGElement::type() const {
  return registry_.get().get<TreeComponent>(entity_).type();
}

Entity SVGElement::entity() const {
  return entity_;
}

std::string SVGElement::id() const {
  if (const auto* idComponent = registry_.get().try_get<IdComponent>(entity_)) {
    return idComponent->id;
  } else {
    return "";
  }
}

void SVGElement::setId(std::string_view id) {
  if (!id.empty()) {
    auto& idComponent = registry_.get().get_or_emplace<IdComponent>(entity_);
    idComponent.id = id;
  } else {
    registry_.get().remove_if_exists<IdComponent>(entity_);
  }
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

std::string_view SVGPathElement::d() const {
  if (const auto* pathComponent = registry_.get().try_get<PathComponent>(entity_)) {
    return pathComponent->d();
  } else {
    return "";
  }
}

std::optional<ParseError> SVGPathElement::setD(std::string_view d) {
  auto& pathComponent = registry_.get().get_or_emplace<PathComponent>(entity_);
  return pathComponent.setD(d);
}

}  // namespace donner