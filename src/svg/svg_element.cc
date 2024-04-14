#include "src/svg/svg_element.h"

#include "src/css/parser/selector_parser.h"
#include "src/svg/components/attributes_component.h"
#include "src/svg/components/class_component.h"
#include "src/svg/components/computed_style_component.h"
#include "src/svg/components/document_context.h"
#include "src/svg/components/id_component.h"
#include "src/svg/components/shadow_tree_component.h"
#include "src/svg/components/style_component.h"
#include "src/svg/components/transform_component.h"
#include "src/svg/components/tree_component.h"
#include "src/svg/svg_document.h"

namespace donner::svg {

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

SVGElement::SVGElement(EntityHandle handle) : handle_(handle) {}

SVGElement::SVGElement(const SVGElement& other) = default;
SVGElement::SVGElement(SVGElement&& other) noexcept {
  *this = std::move(other);
}

SVGElement& SVGElement::operator=(const SVGElement& other) = default;
SVGElement& SVGElement::operator=(SVGElement&& other) noexcept {
  handle_ = other.handle_;
  other.handle_ = EntityHandle();
  return *this;
}

ElementType SVGElement::type() const {
  return handle_.get<components::TreeComponent>().type();
}

RcString SVGElement::typeString() const {
  return handle_.get<components::TreeComponent>().typeString();
}

Entity SVGElement::entity() const {
  return handle_.entity();
}

RcString SVGElement::id() const {
  if (const auto* component = handle_.try_get<components::IdComponent>()) {
    return component->id;
  } else {
    return "";
  }
}

void SVGElement::setId(std::string_view id) {
  // Explicitly remove and re-create, so that DocumentContext can update its id-to-entity map.
  handle_.remove<components::IdComponent>();
  if (!id.empty()) {
    handle_.emplace<components::IdComponent>(RcString(id));
  }

  handle_.get_or_emplace<components::AttributesComponent>().setAttribute("id", RcString(id));
}

RcString SVGElement::className() const {
  if (const auto* component = handle_.try_get<components::ClassComponent>()) {
    return component->className;
  } else {
    return "";
  }
}

void SVGElement::setClassName(std::string_view name) {
  if (!name.empty()) {
    auto& component = handle_.get_or_emplace<components::ClassComponent>();
    component.className = name;
  } else {
    handle_.remove<components::ClassComponent>();
  }

  handle_.get_or_emplace<components::AttributesComponent>().setAttribute("class", RcString(name));
}

void SVGElement::setStyle(std::string_view style) {
  handle_.get_or_emplace<components::StyleComponent>().setStyle(style);

  handle_.get_or_emplace<components::AttributesComponent>().setAttribute("style", RcString(style));
}

ParseResult<bool> SVGElement::trySetPresentationAttribute(std::string_view name,
                                                          std::string_view value) {
  ParseResult<bool> trySetResult =
      handle_.get_or_emplace<components::StyleComponent>().trySetPresentationAttribute(handle_,
                                                                                       name, value);

  if (trySetResult.hasResult() && trySetResult.result()) {
    // Set succeeded, so store the attribute value.
    handle_.get_or_emplace<components::AttributesComponent>().setAttribute(RcString(name),
                                                                           RcString(value));
    return true;
  }

  return trySetResult;
}

bool SVGElement::hasAttribute(std::string_view name) const {
  return handle_.get_or_emplace<components::AttributesComponent>().hasAttribute(RcString(name));
}

std::optional<RcString> SVGElement::getAttribute(std::string_view name) const {
  return handle_.get_or_emplace<components::AttributesComponent>().getAttribute(RcString(name));
}

void SVGElement::setAttribute(std::string_view name, std::string_view value) {
  // First check some special cases which will never be presentation attributes.
  if (name == "id") {
    return setId(value);
  } else if (name == "class") {
    return setClassName(value);
  } else if (name == "style") {
    return setStyle(value);
  }

  // If it's not in the list above, it may be presentation attribute.
  const ParseResult<bool> trySetResult = trySetPresentationAttribute(name, value);
  if (trySetResult.hasResult() && trySetResult.result()) {
    // Early-return since if this succeed, the attribute has already been stored.
    return;
  }

  // Otherwise store as a generic attribute.
  return handle_.get_or_emplace<components::AttributesComponent>().setAttribute(RcString(name),
                                                                                RcString(value));
}

void SVGElement::removeAttribute(std::string_view name) {
  // First check some special cases which will never be presentation attributes.
  if (name == "id") {
    setId("");
  } else if (name == "class") {
    setClassName("");
  } else if (name == "style") {
    setStyle("");
  } else {
    [[maybe_unused]] ParseResult<bool> trySetResult =
        handle_.get_or_emplace<components::StyleComponent>().trySetPresentationAttribute(
            handle_, name, "initial");
    // Ignore return result, since it's fine if the attribute doesn't exist.
  }

  // Remove any storage for this attribute.
  handle_.get_or_emplace<components::AttributesComponent>().removeAttribute(RcString(name));
}

SVGDocument& SVGElement::ownerDocument() {
  return registry().ctx().get<components::DocumentContext>().document();
}

std::optional<SVGElement> SVGElement::parentElement() const {
  const auto& tree = handle_.get<components::TreeComponent>();
  return tree.parent() != entt::null ? std::make_optional(SVGElement(toHandle(tree.parent())))
                                     : std::nullopt;
}

std::optional<SVGElement> SVGElement::firstChild() const {
  if (handle_.all_of<components::ShadowTreeComponent>()) {
    // Don't enumerate children for shadow trees.
    return std::nullopt;
  }

  const auto& tree = handle_.get<components::TreeComponent>();
  return tree.firstChild() != entt::null
             ? std::make_optional(SVGElement(toHandle(tree.firstChild())))
             : std::nullopt;
}

std::optional<SVGElement> SVGElement::lastChild() const {
  if (handle_.all_of<components::ShadowTreeComponent>()) {
    // Don't enumerate children for shadow trees.
    return std::nullopt;
  }

  const auto& tree = handle_.get<components::TreeComponent>();
  return tree.lastChild() != entt::null ? std::make_optional(SVGElement(toHandle(tree.lastChild())))
                                        : std::nullopt;
}

std::optional<SVGElement> SVGElement::previousSibling() const {
  const auto& tree = handle_.get<components::TreeComponent>();
  return tree.previousSibling() != entt::null
             ? std::make_optional(SVGElement(toHandle(tree.previousSibling())))
             : std::nullopt;
}

std::optional<SVGElement> SVGElement::nextSibling() const {
  const auto& tree = handle_.get<components::TreeComponent>();
  return tree.nextSibling() != entt::null
             ? std::make_optional(SVGElement(toHandle(tree.nextSibling())))
             : std::nullopt;
}

SVGElement SVGElement::insertBefore(SVGElement newNode, std::optional<SVGElement> referenceNode) {
  handle_.get<components::TreeComponent>().insertBefore(
      registry(), newNode.handle_.entity(),
      referenceNode ? referenceNode->handle_.entity() : entt::null);
  return newNode;
}

SVGElement SVGElement::appendChild(SVGElement child) {
  handle_.get<components::TreeComponent>().appendChild(registry(), child.handle_.entity());
  return child;
}

SVGElement SVGElement::replaceChild(SVGElement newChild, SVGElement oldChild) {
  handle_.get<components::TreeComponent>().replaceChild(registry(), newChild.handle_.entity(),
                                                        oldChild.handle_.entity());
  return newChild;
}

SVGElement SVGElement::removeChild(SVGElement child) {
  handle_.get<components::TreeComponent>().removeChild(registry(), child.entity());
  return child;
}

void SVGElement::remove() {
  handle_.get<components::TreeComponent>().remove(registry());
}

std::optional<SVGElement> SVGElement::querySelector(std::string_view str) {
  const ParseResult<css::Selector> selectorResult = css::SelectorParser::Parse(str);
  if (selectorResult.hasError()) {
    return std::nullopt;
  }

  const css::Selector& selector = selectorResult.result();
  return querySelectorSearch(selector, *this);
}

const PropertyRegistry& SVGElement::getComputedStyle() const {
  auto& computedStyle = handle_.get_or_emplace<components::ComputedStyleComponent>();
  computedStyle.computeProperties(handle_);

  return computedStyle.properties();
}

EntityHandle SVGElement::CreateEntity(Registry& registry, RcString typeString, ElementType type) {
  Entity entity = registry.create();
  registry.emplace<components::TreeComponent>(entity, type, std::move(typeString));
  registry.emplace<components::TransformComponent>(entity);
  return EntityHandle(registry, entity);
}

}  // namespace donner::svg
