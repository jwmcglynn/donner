#include "donner/svg/SVGElement.h"

#include "donner/base/element/ElementTraversalGenerators.h"
#include "donner/base/xml/components/AttributesComponent.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/css/parser/SelectorParser.h"
#include "donner/css/selectors/SelectorMatchOptions.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/ClassComponent.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/ElementTypeComponent.h"
#include "donner/svg/components/IdComponent.h"
#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/components/layout/TransformComponent.h"
#include "donner/svg/components/shadow/ShadowTreeComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/style/StyleComponent.h"
#include "donner/svg/components/style/StyleSystem.h"
#include "donner/svg/properties/PresentationAttributeParsing.h"

namespace donner::svg {

namespace {

static std::optional<SVGElement> querySelectorSearch(const css::Selector& selector,
                                                     const SVGElement& element) {
  css::SelectorMatchOptions<SVGElement> options;
  options.scopeElement = &element;

  ElementTraversalGenerator<SVGElement> elements = allChildrenRecursiveGenerator(element);
  while (elements.next()) {
    SVGElement childElement = elements.getValue();
    if (selector.matches(childElement, options).matched) {
      return childElement;
    }
  }

  return std::nullopt;
}

/// Mark an entity with dirty flags for incremental invalidation.
void markDirty(EntityHandle handle, uint16_t flags) {
  handle.get_or_emplace<components::DirtyFlagsComponent>().mark(flags);
}

void invalidateComputedStyle(EntityHandle handle) {
  components::StyleSystem().invalidateComputed(handle);
}

components::RenderTreeState& getRenderTreeState(EntityHandle handle) {
  auto& registry = *handle.registry();
  if (!registry.ctx().contains<components::RenderTreeState>()) {
    registry.ctx().emplace<components::RenderTreeState>();
  }
  return registry.ctx().get<components::RenderTreeState>();
}

void markNeedsFullStyleRecompute(EntityHandle handle) {
  getRenderTreeState(handle).needsFullStyleRecompute = true;
}

void markNeedsFullRebuild(EntityHandle handle) {
  auto& renderState = getRenderTreeState(handle);
  renderState.needsFullRebuild = true;
  renderState.needsFullStyleRecompute = true;
}

void invalidateComputedStyleForDescendants(EntityHandle handle) {
  auto& tree = handle.get<donner::components::TreeComponent>();
  Registry& registry = *handle.registry();
  for (entt::entity child = tree.firstChild(); child != entt::null;
       child = registry.get<donner::components::TreeComponent>(child).nextSibling()) {
    donner::components::ForAllChildrenRecursive(
        EntityHandle(registry, child), [](EntityHandle desc) { invalidateComputedStyle(desc); });
  }
}

/// Propagate style-related dirty flags to all descendants of the given entity.
void propagateStyleDirtyToDescendants(EntityHandle handle) {
  auto& tree = handle.get<donner::components::TreeComponent>();
  Registry& registry = *handle.registry();
  for (entt::entity child = tree.firstChild(); child != entt::null;
       child = registry.get<donner::components::TreeComponent>(child).nextSibling()) {
    donner::components::ForAllChildrenRecursive(
        EntityHandle(registry, child), [](EntityHandle desc) {
          markDirty(desc,
                    components::DirtyFlagsComponent::Style |
                        components::DirtyFlagsComponent::Paint |
                        components::DirtyFlagsComponent::RenderInstance);
        });
  }
}

/// Propagate world-transform-related dirty flags to all descendants of the given entity.
void propagateWorldTransformDirtyToDescendants(EntityHandle handle) {
  auto& tree = handle.get<donner::components::TreeComponent>();
  Registry& registry = *handle.registry();
  for (entt::entity child = tree.firstChild(); child != entt::null;
       child = registry.get<donner::components::TreeComponent>(child).nextSibling()) {
    donner::components::ForAllChildrenRecursive(
        EntityHandle(registry, child), [](EntityHandle desc) {
          markDirty(desc,
                    components::DirtyFlagsComponent::WorldTransform |
                        components::DirtyFlagsComponent::RenderInstance);
        });
  }
}

/// Mark an entity and all of its descendants with the given dirty flags.
void markDirtySubtree(EntityHandle handle, uint16_t flags) {
  markDirty(handle, flags);

  auto& tree = handle.get<donner::components::TreeComponent>();
  Registry& registry = *handle.registry();
  for (entt::entity child = tree.firstChild(); child != entt::null;
       child = registry.get<donner::components::TreeComponent>(child).nextSibling()) {
    donner::components::ForAllChildrenRecursive(
        EntityHandle(registry, child), [flags](EntityHandle desc) { markDirty(desc, flags); });
  }
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
  return handle_.get<components::ElementTypeComponent>().type();
}

xml::XMLQualifiedNameRef SVGElement::tagName() const {
  return handle_.get<donner::components::TreeComponent>().tagName();
}

bool SVGElement::isKnownType() const {
  return type() != ElementType::Unknown;
}

RcString SVGElement::id() const {
  if (const auto* component = handle_.try_get<components::IdComponent>()) {
    return component->id();
  } else {
    return "";
  }
}

void SVGElement::setId(std::string_view id) {
  // Explicitly remove and re-create, so that SVGDocumentContext can update its
  // id-to-entity map.
  handle_.remove<components::IdComponent>();
  if (!id.empty()) {
    handle_.emplace<components::IdComponent>(RcString(id));
  }

  handle_.get_or_emplace<donner::components::AttributesComponent>().setAttribute(
      *handle_.registry(), xml::XMLQualifiedName("id"), RcString(id));
  markNeedsFullStyleRecompute(handle_);
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

  handle_.get_or_emplace<donner::components::AttributesComponent>().setAttribute(
      *handle_.registry(), xml::XMLQualifiedName("class"), RcString(name));

  // Class changes affect CSS selector matching, which can change any inherited property.
  markNeedsFullStyleRecompute(handle_);
  invalidateComputedStyle(handle_);
  invalidateComputedStyleForDescendants(handle_);
  markDirty(handle_, components::DirtyFlagsComponent::StyleCascade);
  propagateStyleDirtyToDescendants(handle_);
}

void SVGElement::setStyle(std::string_view style) {
  handle_.get_or_emplace<components::StyleComponent>().setStyle(style);

  handle_.get_or_emplace<donner::components::AttributesComponent>().setAttribute(
      *handle_.registry(), xml::XMLQualifiedName("style"), RcString(style));

  components::StyleSystem().invalidateAll(handle_);
  markNeedsFullStyleRecompute(handle_);
  invalidateComputedStyleForDescendants(handle_);

  markDirty(handle_, components::DirtyFlagsComponent::StyleCascade);
  propagateStyleDirtyToDescendants(handle_);
}

void SVGElement::updateStyle(std::string_view style) {
  components::StyleSystem().updateStyle(handle_, style);

  markNeedsFullStyleRecompute(handle_);
  invalidateComputedStyleForDescendants(handle_);

  markDirty(handle_, components::DirtyFlagsComponent::StyleCascade);
  propagateStyleDirtyToDescendants(handle_);
}

ParseResult<bool> SVGElement::trySetPresentationAttribute(std::string_view name,
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

  // Try common CSS properties first (fill, stroke, opacity, transform, etc.).
  auto trySetResult =
      handle_.get_or_emplace<components::StyleComponent>().trySetPresentationAttribute(
          handle_, actualName, value);

  if (trySetResult.hasError()) {
    return trySetResult;
  }

  if (!trySetResult.result()) {
    // Try element-specific presentation attributes (cx, cy, r, rx, ry, d, etc.).
    parser::PropertyParseFnParams params = parser::PropertyParseFnParams::CreateForAttribute(value);
    trySetResult = parser::ParsePresentationAttribute(type(), handle_, actualName, params);
  }

  if (trySetResult.hasResult() && trySetResult.result()) {
    // Set succeeded, so store the attribute value.
    handle_.get_or_emplace<donner::components::AttributesComponent>().setAttribute(
        *handle_.registry(), xml::XMLQualifiedName(RcString(name)), RcString(value));

    // Mark dirty flags based on the attribute type.
    if (actualName == "transform") {
      markDirty(handle_, components::DirtyFlagsComponent::Transform |
                              components::DirtyFlagsComponent::WorldTransform |
                              components::DirtyFlagsComponent::RenderInstance);
      propagateWorldTransformDirtyToDescendants(handle_);
    } else {
      // For CSS properties (fill, stroke, opacity, etc.) and element-specific attributes
      // (cx, cy, r, d, etc.), mark style cascade + shape dirty.
      markNeedsFullStyleRecompute(handle_);
      invalidateComputedStyle(handle_);
      markDirty(handle_, components::DirtyFlagsComponent::StyleCascade |
                              components::DirtyFlagsComponent::Shape);
      if (PropertyRegistry::isPresentationAttributeInherited(actualName)) {
        invalidateComputedStyleForDescendants(handle_);
        propagateStyleDirtyToDescendants(handle_);
      }
    }
    return true;
  }

  return trySetResult;
}

bool SVGElement::hasAttribute(const xml::XMLQualifiedNameRef& name) const {
  return handle_.get_or_emplace<donner::components::AttributesComponent>().hasAttribute(name);
}

std::optional<RcString> SVGElement::getAttribute(const xml::XMLQualifiedNameRef& name) const {
  return handle_.get_or_emplace<donner::components::AttributesComponent>().getAttribute(name);
}

SmallVector<xml::XMLQualifiedNameRef, 1> SVGElement::findMatchingAttributes(
    const xml::XMLQualifiedNameRef& matcher) const {
  return handle_.get_or_emplace<donner::components::AttributesComponent>().findMatchingAttributes(
      matcher);
}

void SVGElement::setAttribute(const xml::XMLQualifiedNameRef& name, std::string_view value) {
  // TODO: Namespace support for these attributes
  // First check some special cases which will never be presentation attributes.
  if (name == xml::XMLQualifiedNameRef("id")) {
    return setId(value);
  } else if (name == xml::XMLQualifiedNameRef("class")) {
    return setClassName(value);
  } else if (name == xml::XMLQualifiedNameRef("style")) {
    return setStyle(value);
  }

  // If it's not in the list above, it may be presentation attribute.
  // TODO(jwmcglynn): Add support for namespace when parsing presentation attributes.
  // Only parse empty namespaces for now.
  if (name.namespacePrefix.empty()) {
    const auto trySetResult = trySetPresentationAttribute(name.name, value);
    if (trySetResult.hasResult() && trySetResult.result()) {
      // Early-return since if this succeeds, the attribute has already been stored.
      return;
    }
  }

  // Otherwise store as a generic attribute.
  markNeedsFullStyleRecompute(handle_);
  return handle_.get_or_emplace<donner::components::AttributesComponent>().setAttribute(
      *handle_.registry(), name, RcString(value));
}

void SVGElement::removeAttribute(const xml::XMLQualifiedNameRef& name) {
  // TODO: Namespace support for these attributes
  // First check some special cases which will never be presentation attributes.
  if (name == xml::XMLQualifiedNameRef("id")) {
    setId("");
  } else if (name == xml::XMLQualifiedNameRef("class")) {
    setClassName("");
  } else if (name == xml::XMLQualifiedNameRef("style")) {
    setStyle("");
  } else {
    // TODO(jwmcglynn): Add support for namespace when parsing presentation attributes.
    // Only parse empty namespaces for now.
    if (name.namespacePrefix.empty()) {
      [[maybe_unused]] auto trySetResult = trySetPresentationAttribute(name.name, "initial");
    }
    markNeedsFullStyleRecompute(handle_);
    // Ignore return result, since it's fine if the attribute doesn't exist.
  }

  // Remove any storage for this attribute.
  handle_.get_or_emplace<donner::components::AttributesComponent>().removeAttribute(
      *handle_.registry(), name);
}

SVGDocument SVGElement::ownerDocument() {
  std::shared_ptr<Registry> sharedRegistry =
      registry().ctx().get<components::SVGDocumentContext>().getSharedRegistry();
  return SVGDocument(std::move(sharedRegistry));
}

std::optional<SVGElement> SVGElement::parentElement() const {
  const auto& tree = handle_.get<donner::components::TreeComponent>();
  const EntityHandle parent = toHandle(tree.parent());
  const bool isSVGElement = (parent && parent.all_of<components::ElementTypeComponent>());

  return isSVGElement ? std::make_optional(SVGElement(parent)) : std::nullopt;
}

std::optional<SVGElement> SVGElement::firstChild() const {
  if (handle_.all_of<components::ShadowTreeComponent>()) {
    // Don't enumerate children for shadow trees.
    return std::nullopt;
  }

  const auto& tree = handle_.get<donner::components::TreeComponent>();
  return tree.firstChild() != entt::null
             ? std::make_optional(SVGElement(toHandle(tree.firstChild())))
             : std::nullopt;
}

std::optional<SVGElement> SVGElement::lastChild() const {
  if (handle_.all_of<components::ShadowTreeComponent>()) {
    // Don't enumerate children for shadow trees.
    return std::nullopt;
  }

  const auto& tree = handle_.get<donner::components::TreeComponent>();
  return tree.lastChild() != entt::null ? std::make_optional(SVGElement(toHandle(tree.lastChild())))
                                        : std::nullopt;
}

std::optional<SVGElement> SVGElement::previousSibling() const {
  const auto& tree = handle_.get<donner::components::TreeComponent>();
  return tree.previousSibling() != entt::null
             ? std::make_optional(SVGElement(toHandle(tree.previousSibling())))
             : std::nullopt;
}

std::optional<SVGElement> SVGElement::nextSibling() const {
  const auto& tree = handle_.get<donner::components::TreeComponent>();
  return tree.nextSibling() != entt::null
             ? std::make_optional(SVGElement(toHandle(tree.nextSibling())))
             : std::nullopt;
}

void SVGElement::insertBefore(const SVGElement& newNode, std::optional<SVGElement> referenceNode) {
  handle_.get<donner::components::TreeComponent>().insertBefore(
      registry(), newNode.handle_.entity(),
      referenceNode ? referenceNode->handle_.entity() : entt::null);
  // Tree structure change: mark inserted subtree and parent for full recomputation.
  markNeedsFullRebuild(handle_);
  markDirty(handle_, components::DirtyFlagsComponent::All);
  markDirtySubtree(newNode.handle_, components::DirtyFlagsComponent::All);
}

void SVGElement::appendChild(const SVGElement& child) {
  handle_.get<donner::components::TreeComponent>().appendChild(registry(),
                                                               child.entityHandle().entity());
  markNeedsFullRebuild(handle_);
  markDirty(handle_, components::DirtyFlagsComponent::All);
  markDirtySubtree(child.handle_, components::DirtyFlagsComponent::All);
}

void SVGElement::replaceChild(const SVGElement& newChild, const SVGElement& oldChild) {
  handle_.get<donner::components::TreeComponent>().replaceChild(
      registry(), newChild.handle_.entity(), oldChild.entityHandle().entity());
  markNeedsFullRebuild(handle_);
  markDirty(handle_, components::DirtyFlagsComponent::All);
  markDirtySubtree(newChild.handle_, components::DirtyFlagsComponent::All);
}

void SVGElement::removeChild(const SVGElement& child) {
  handle_.get<donner::components::TreeComponent>().removeChild(registry(),
                                                               child.entityHandle().entity());
  markNeedsFullRebuild(handle_);
  markDirty(handle_, components::DirtyFlagsComponent::All);
}

void SVGElement::remove() {
  // Mark parent dirty before removing from tree (after remove, parent link is gone).
  if (auto parent = parentElement()) {
    markNeedsFullRebuild(parent->handle_);
    markDirty(parent->handle_, components::DirtyFlagsComponent::All);
  }
  handle_.get<donner::components::TreeComponent>().remove(registry());
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

EntityHandle SVGElement::CreateEmptyEntity(SVGDocument& document) {
  Registry& registry = document.registry();
  Entity entity = document.registry().create();

  return EntityHandle(registry, entity);
}

void SVGElement::CreateEntityOn(EntityHandle handle, const xml::XMLQualifiedNameRef& tagName,
                                ElementType type) {
  if (!handle.all_of<donner::components::TreeComponent>()) {
    handle.emplace<donner::components::TreeComponent>(tagName);
  }
  handle.emplace<components::ElementTypeComponent>(type);
  handle.emplace<components::TransformComponent>();
}

}  // namespace donner::svg
