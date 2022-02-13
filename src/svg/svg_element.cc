#include "src/svg/svg_element.h"

#include "src/css/parser/selector_parser.h"
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
SVGElement& SVGElement::operator=(const SVGElement& other) = default;

ElementType SVGElement::type() const {
  return handle_.get<TreeComponent>().type();
}

RcString SVGElement::typeString() const {
  return handle_.get<TreeComponent>().typeString();
}

Entity SVGElement::entity() const {
  return handle_.entity();
}

RcString SVGElement::id() const {
  if (const auto* component = handle_.try_get<IdComponent>()) {
    return component->id;
  } else {
    return "";
  }
}

void SVGElement::setId(std::string_view id) {
  // Explicitly remove and re-create, so that DocumentContext can update its id-to-entity map.
  handle_.remove<IdComponent>();
  if (!id.empty()) {
    handle_.emplace<IdComponent>(IdComponent{RcString(id)});
  }
}

RcString SVGElement::className() const {
  if (const auto* component = handle_.try_get<ClassComponent>()) {
    return component->className;
  } else {
    return "";
  }
}

void SVGElement::setClassName(std::string_view name) {
  if (!name.empty()) {
    auto& component = handle_.get_or_emplace<ClassComponent>();
    component.className = name;
  } else {
    handle_.remove<ClassComponent>();
  }
}

Transformd SVGElement::transform() const {
  computeTransform();
  return handle_.get<ComputedTransformComponent>().transform;
}

void SVGElement::setTransform(Transformd transform) {
  auto& component = handle_.get_or_emplace<TransformComponent>();
  component.transform.set(CssTransform(transform), css::Specificity::Override());
}

void SVGElement::setStyle(std::string_view style) {
  handle_.get_or_emplace<StyleComponent>().setStyle(style);
}

bool SVGElement::trySetPresentationAttribute(std::string_view name, std::string_view value) {
  return handle_.get_or_emplace<StyleComponent>().trySetPresentationAttribute(handle_, name, value);
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
  return registry().ctx<DocumentContext>().document;
}

std::optional<SVGElement> SVGElement::parentElement() const {
  const auto& tree = handle_.get<TreeComponent>();
  return tree.parent() != entt::null ? std::make_optional(SVGElement(toHandle(tree.parent())))
                                     : std::nullopt;
}

std::optional<SVGElement> SVGElement::firstChild() const {
  if (handle_.all_of<ShadowTreeComponent>()) {
    // Don't enumerate children for shadow trees.
    return std::nullopt;
  }

  const auto& tree = handle_.get<TreeComponent>();
  return tree.firstChild() != entt::null
             ? std::make_optional(SVGElement(toHandle(tree.firstChild())))
             : std::nullopt;
}

std::optional<SVGElement> SVGElement::lastChild() const {
  if (handle_.all_of<ShadowTreeComponent>()) {
    // Don't enumerate children for shadow trees.
    return std::nullopt;
  }

  const auto& tree = handle_.get<TreeComponent>();
  return tree.lastChild() != entt::null ? std::make_optional(SVGElement(toHandle(tree.lastChild())))
                                        : std::nullopt;
}

std::optional<SVGElement> SVGElement::previousSibling() const {
  const auto& tree = handle_.get<TreeComponent>();
  return tree.previousSibling() != entt::null
             ? std::make_optional(SVGElement(toHandle(tree.previousSibling())))
             : std::nullopt;
}

std::optional<SVGElement> SVGElement::nextSibling() const {
  const auto& tree = handle_.get<TreeComponent>();
  return tree.nextSibling() != entt::null
             ? std::make_optional(SVGElement(toHandle(tree.nextSibling())))
             : std::nullopt;
}

SVGElement SVGElement::insertBefore(SVGElement newNode, std::optional<SVGElement> referenceNode) {
  handle_.get<TreeComponent>().insertBefore(
      registry(), newNode.handle_.entity(),
      referenceNode ? referenceNode->handle_.entity() : entt::null);
  return newNode;
}

SVGElement SVGElement::appendChild(SVGElement child) {
  handle_.get<TreeComponent>().appendChild(registry(), child.handle_.entity());
  return child;
}

SVGElement SVGElement::replaceChild(SVGElement newChild, SVGElement oldChild) {
  handle_.get<TreeComponent>().replaceChild(registry(), newChild.handle_.entity(),
                                            oldChild.handle_.entity());
  return newChild;
}

SVGElement SVGElement::removeChild(SVGElement child) {
  handle_.get<TreeComponent>().removeChild(registry(), child.entity());
  return child;
}

void SVGElement::remove() {
  handle_.get<TreeComponent>().remove(registry());
}

std::optional<SVGElement> SVGElement::querySelector(std::string_view str) {
  const ParseResult<css::Selector> selectorResult = css::SelectorParser::Parse(str);
  if (selectorResult.hasError()) {
    return std::nullopt;
  }

  const css::Selector selector = std::move(selectorResult.result());
  return querySelectorSearch(selector, *this);
}

const PropertyRegistry& SVGElement::getComputedStyle() const {
  auto& computedStyle = handle_.get_or_emplace<ComputedStyleComponent>();
  computedStyle.computeProperties(handle_);

  return computedStyle.properties();
}

EntityHandle SVGElement::CreateEntity(Registry& registry, RcString typeString, ElementType type) {
  Entity entity = registry.create();
  registry.emplace<TreeComponent>(entity, type, std::move(typeString));
  return EntityHandle(registry, entity);
}

void SVGElement::invalidateTransform() {
  handle_.remove<ComputedTransformComponent>();
}

void SVGElement::computeTransform() const {
  auto& transform = handle_.get_or_emplace<TransformComponent>();
  transform.compute(handle_, FontMetrics());
}

}  // namespace donner::svg
