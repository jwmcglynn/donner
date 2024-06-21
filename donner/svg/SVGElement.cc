#include "donner/svg/SVGElement.h"

#include "donner/css/parser/SelectorParser.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/AttributesComponent.h"
#include "donner/svg/components/ClassComponent.h"
#include "donner/svg/components/DocumentContext.h"
#include "donner/svg/components/IdComponent.h"
#include "donner/svg/components/TransformComponent.h"
#include "donner/svg/components/TreeComponent.h"
#include "donner/svg/components/shadow/ShadowTreeComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/style/StyleComponent.h"
#include "donner/svg/components/style/StyleSystem.h"

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

XMLQualifiedNameRef SVGElement::xmlTypeName() const {
  return handle_.get<components::TreeComponent>().xmlTypeName();
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
  // Explicitly remove and re-create, so that DocumentContext can update its
  // id-to-entity map.
  handle_.remove<components::IdComponent>();
  if (!id.empty()) {
    handle_.emplace<components::IdComponent>(RcString(id));
  }

  handle_.get_or_emplace<components::AttributesComponent>().setAttribute(XMLQualifiedName("id"),
                                                                         RcString(id));
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

  handle_.get_or_emplace<components::AttributesComponent>().setAttribute(XMLQualifiedName("class"),
                                                                         RcString(name));
}

void SVGElement::setStyle(std::string_view style) {
  handle_.get_or_emplace<components::StyleComponent>().setStyle(style);

  handle_.get_or_emplace<components::AttributesComponent>().setAttribute(XMLQualifiedName("style"),
                                                                         RcString(style));
}

parser::ParseResult<bool> SVGElement::trySetPresentationAttribute(std::string_view name,
                                                                  std::string_view value) {
  std::string_view actualName = name;

  // gradientTransform and patternTransform are special, since they map to the
  // "transform" presentation attribute. When doing this mapping, store the XML
  // attribute with the user-visible attribute name and internally map it to
  // "transform".
  if (((type() == ElementType::LinearGradient || type() == ElementType::RadialGradient) &&
       name == "gradientTransform") ||
      (type() == ElementType::Pattern && name == "patternTransform")) {
    actualName = "transform";
  }

  auto trySetResult =
      handle_.get_or_emplace<components::StyleComponent>().trySetPresentationAttribute(
          handle_, actualName, value);

  if (trySetResult.hasResult() && trySetResult.result()) {
    // Set succeeded, so store the attribute value.
    handle_.get_or_emplace<components::AttributesComponent>().setAttribute(
        XMLQualifiedName(RcString(name)), RcString(value));
    return true;
  }

  return trySetResult;
}

bool SVGElement::hasAttribute(const XMLQualifiedNameRef& name) const {
  return handle_.get_or_emplace<components::AttributesComponent>().hasAttribute(name);
}

std::optional<RcString> SVGElement::getAttribute(const XMLQualifiedNameRef& name) const {
  return handle_.get_or_emplace<components::AttributesComponent>().getAttribute(name);
}

SmallVector<XMLQualifiedNameRef, 1> SVGElement::findMatchingAttributes(
    const XMLQualifiedNameRef& matcher) const {
  return handle_.get_or_emplace<components::AttributesComponent>().findMatchingAttributes(matcher);
}

void SVGElement::setAttribute(const XMLQualifiedNameRef& name, std::string_view value) {
  // First check some special cases which will never be presentation attributes.
  if (name == XMLQualifiedNameRef("id")) {
    return setId(value);
  } else if (name == XMLQualifiedNameRef("class")) {
    return setClassName(value);
  } else if (name == XMLQualifiedNameRef("style")) {
    return setStyle(value);
  }

  // If it's not in the list above, it may be presentation attribute.
  // TODO: Add support for namespace when parsing presentation attributes.
  // Only parse empty namespaces for now.
  if (name.namespacePrefix.empty()) {
    const auto trySetResult = trySetPresentationAttribute(name.name, value);
    if (trySetResult.hasResult() && trySetResult.result()) {
      // Early-return since if this succeeds, the attribute has already been stored.
      return;
    }
  }

  // Otherwise store as a generic attribute.
  return handle_.get_or_emplace<components::AttributesComponent>().setAttribute(name,
                                                                                RcString(value));
}

void SVGElement::removeAttribute(const XMLQualifiedNameRef& name) {
  // First check some special cases which will never be presentation attributes.
  if (name == XMLQualifiedNameRef("id")) {
    setId("");
  } else if (name == XMLQualifiedNameRef("class")) {
    setClassName("");
  } else if (name == XMLQualifiedNameRef("style")) {
    setStyle("");
  } else {
    // TODO: Add support for namespace when parsing presentation attributes.
    // Only parse empty namespaces for now.
    if (name.namespacePrefix.empty()) {
      [[maybe_unused]] auto trySetResult =
          handle_.get_or_emplace<components::StyleComponent>().trySetPresentationAttribute(
              handle_, name.name, "initial");
    }
    // Ignore return result, since it's fine if the attribute doesn't exist.
  }

  // Remove any storage for this attribute.
  handle_.get_or_emplace<components::AttributesComponent>().removeAttribute(name);
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
  const auto selectorResult = css::parser::SelectorParser::Parse(str);
  if (selectorResult.hasError()) {
    return std::nullopt;
  }

  const css::Selector& selector = selectorResult.result();
  return querySelectorSearch(selector, *this);
}

const PropertyRegistry& SVGElement::getComputedStyle() const {
  const components::ComputedStyleComponent& computedStyle =
      components::StyleSystem().computeStyle(handle_, nullptr);
  return computedStyle.properties.value();
}

EntityHandle SVGElement::CreateEntity(Registry& registry, const XMLQualifiedNameRef& xmlTypeName,
                                      ElementType type) {
  Entity entity = registry.create();
  registry.emplace<components::TreeComponent>(entity, type, xmlTypeName);
  registry.emplace<components::TransformComponent>(entity);
  return EntityHandle(registry, entity);
}

}  // namespace donner::svg
