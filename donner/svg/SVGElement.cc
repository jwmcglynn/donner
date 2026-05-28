#include "donner/svg/SVGElement.h"

#include <atomic>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/xml/components/AttributesComponent.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/css/parser/SelectorParser.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGQuerySelector.h"
#include "donner/svg/components/ClassComponent.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/ElementTypeComponent.h"
#include "donner/svg/components/IdComponent.h"
#include "donner/svg/components/NodeLifetimeCollector.h"
#include "donner/svg/components/NodeLifetimeComponent.h"
#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/components/TreeMutation.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/layout/TransformComponent.h"
#include "donner/svg/components/shadow/ShadowTreeComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/style/StyleComponent.h"
#include "donner/svg/components/style/StyleSystem.h"
#include "donner/svg/properties/PresentationAttributeParsing.h"

namespace donner::svg {

namespace {

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
          markDirty(desc, components::DirtyFlagsComponent::Style |
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
          markDirty(desc, components::DirtyFlagsComponent::WorldTransform |
                              components::DirtyFlagsComponent::RenderInstance);
        });
  }
}

std::optional<EntityHandle> ElementEntityHandle(Registry& registry, Entity entity) {
  if (entity == entt::null) {
    return std::nullopt;
  }

  EntityHandle handle(registry, entity);
  return handle.all_of<components::ElementTypeComponent>() ? std::make_optional(handle)
                                                           : std::nullopt;
}

std::optional<EntityHandle> FirstElementChild(EntityHandle handle) {
  if (handle.all_of<components::ShadowTreeComponent>()) {
    return std::nullopt;
  }

  const auto& tree = handle.get<donner::components::TreeComponent>();
  Registry& registry = *handle.registry();
  for (Entity child = tree.firstChild(); child != entt::null;
       child = registry.get<donner::components::TreeComponent>(child).nextSibling()) {
    if (std::optional<EntityHandle> element = ElementEntityHandle(registry, child)) {
      return element;
    }
  }

  return std::nullopt;
}

std::optional<EntityHandle> LastElementChild(EntityHandle handle) {
  if (handle.all_of<components::ShadowTreeComponent>()) {
    return std::nullopt;
  }

  const auto& tree = handle.get<donner::components::TreeComponent>();
  Registry& registry = *handle.registry();
  for (Entity child = tree.lastChild(); child != entt::null;
       child = registry.get<donner::components::TreeComponent>(child).previousSibling()) {
    if (std::optional<EntityHandle> element = ElementEntityHandle(registry, child)) {
      return element;
    }
  }

  return std::nullopt;
}

std::optional<EntityHandle> PreviousElementSibling(EntityHandle handle) {
  const auto& tree = handle.get<donner::components::TreeComponent>();
  Registry& registry = *handle.registry();
  for (Entity sibling = tree.previousSibling(); sibling != entt::null;
       sibling = registry.get<donner::components::TreeComponent>(sibling).previousSibling()) {
    if (std::optional<EntityHandle> element = ElementEntityHandle(registry, sibling)) {
      return element;
    }
  }

  return std::nullopt;
}

std::optional<EntityHandle> NextElementSibling(EntityHandle handle) {
  const auto& tree = handle.get<donner::components::TreeComponent>();
  Registry& registry = *handle.registry();
  for (Entity sibling = tree.nextSibling(); sibling != entt::null;
       sibling = registry.get<donner::components::TreeComponent>(sibling).nextSibling()) {
    if (std::optional<EntityHandle> element = ElementEntityHandle(registry, sibling)) {
      return element;
    }
  }

  return std::nullopt;
}

}  // namespace

ElementAnchor::ElementAnchor(EntityHandle handle) : entity_(handle ? handle.entity() : entt::null) {
  if (handle && handle.valid()) {
    auto* context = handle.registry()->ctx().find<components::SVGDocumentContext>();
    documentHandle_ = context != nullptr ? context->getSharedDocumentState() : nullptr;
    auto* lifetime = handle.try_get<components::NodeLifetimeComponent>();
    if (lifetime == nullptr) {
      UTILS_RELEASE_ASSERT_MSG(
          !documentHandle_ || documentHandle_->threadingMode() != ThreadingMode::ConcurrentDom ||
              documentHandle_->currentThreadHasWriteAccess(),
          "SVGElement anchor construction requires NodeLifetimeComponent in ConcurrentDom");
      lifetime = &handle.get_or_emplace<components::NodeLifetimeComponent>();
    }
    generation_ = lifetime->generation;
    externalRefs_ = lifetime->externalRefs;
  }
}

ElementAnchor::ElementAnchor(const ElementAnchor& other)
    : documentHandle_(other.documentHandle_),
      externalRefs_(other.externalRefs_),
      entity_(other.entity_),
      generation_(other.generation_) {}

ElementAnchor::ElementAnchor(ElementAnchor&& other) noexcept
    : documentHandle_(std::move(other.documentHandle_)),
      externalRefs_(std::move(other.externalRefs_)),
      entity_(other.entity_),
      generation_(other.generation_) {
  other.entity_ = entt::null;
  other.generation_ = 0;
}

ElementAnchor::~ElementAnchor() noexcept {
  release();
}

ElementAnchor& ElementAnchor::operator=(const ElementAnchor& other) {
  if (this == &other) {
    return *this;
  }

  release();
  documentHandle_ = other.documentHandle_;
  externalRefs_ = other.externalRefs_;
  entity_ = other.entity_;
  generation_ = other.generation_;
  return *this;
}

ElementAnchor& ElementAnchor::operator=(ElementAnchor&& other) noexcept {
  if (this == &other) {
    return *this;
  }

  release();
  documentHandle_ = std::move(other.documentHandle_);
  externalRefs_ = std::move(other.externalRefs_);
  entity_ = other.entity_;
  generation_ = other.generation_;
  other.entity_ = entt::null;
  other.generation_ = 0;
  return *this;
}

EntityHandle ElementAnchor::resolve() const {
  assertScopedEntityHandleAccessAllowed();
  return unsafeResolve();
}

EntityHandle ElementAnchor::unsafeResolve() const {
  if (!documentHandle_ || entity_ == entt::null || !documentHandle_->registry().valid(entity_)) {
    return EntityHandle();
  }

  EntityHandle handle(documentHandle_->registry(), entity_);
  if (const auto* lifetime = handle.try_get<components::NodeLifetimeComponent>()) {
    UTILS_RELEASE_ASSERT_MSG(lifetime->generation == generation_,
                             "SVGElement anchor generation mismatch");
  }
  return handle;
}

void ElementAnchor::assertScopedEntityHandleAccessAllowed() const {
  if (!documentHandle_ || documentHandle_->threadingMode() != ThreadingMode::ConcurrentDom) {
    return;
  }

  UTILS_RELEASE_ASSERT_MSG(
      documentHandle_->currentThreadHasAccess(),
      "SVGElement raw ECS access requires withReadAccess() or withWriteAccess() in ConcurrentDom; "
      "use unsafeEntityHandle() for intentionally unguarded ECS access");
}

void ElementAnchor::release() noexcept {
  if (!externalRefs_) {
    return;
  }

  const bool lastPublicHandle = externalRefs_.use_count() == 2;
  const bool potentiallyDetached = externalRefs_->isDetached.load(std::memory_order_acquire);
  externalRefs_.reset();
  if (!lastPublicHandle || !potentiallyDetached || !documentHandle_ || entity_ == entt::null) {
    return;
  }

  // §concurrent-dom: if the calling thread already holds a read access (and not write), taking
  // write here would self-deadlock — DocumentWriteAccess drains all readers without supporting a
  // read→write upgrade, so the writer would wait on its own held read. (Write access is reentrant,
  // so write-while-write is fine and proceeds.) The detached-node Collect call below is purely
  // opportunistic eager cleanup; the next periodic NodeLifetimeCollector::Collect pass (e.g. on the
  // next source edit, mutation batch commit, or end-of-frame) will pick this entity up, so it is
  // safe to bail without leaking. Without this guard a perfectly-normal API pattern — a detached
  // SVGElement local going out of scope inside a withReadAccess callback — would hang the thread.
  if (documentHandle_->currentThreadHasAccess() &&
      !documentHandle_->currentThreadHasWriteAccess()) {
    return;
  }

  DocumentWriteAccess access = documentHandle_->write();
  Registry& registry = access.registry();
  if (!registry.valid(entity_)) {
    return;
  }

  EntityHandle handle(registry, entity_);
  if (auto* lifetime = handle.try_get<components::NodeLifetimeComponent>()) {
    if (lifetime->generation == generation_ && !lifetime->isAttached() &&
        lifetime->externalRefCount() == 0) {
      components::NodeLifetimeCollector::Collect(registry);
    }
  }
}

SVGElement::SVGElement(EntityHandle handle) : handle_(handle) {}

SVGElement::SVGElement(const SVGElement& other) = default;
SVGElement::SVGElement(SVGElement&& other) noexcept = default;
SVGElement::~SVGElement() noexcept = default;
SVGElement& SVGElement::operator=(const SVGElement& other) = default;
SVGElement& SVGElement::operator=(SVGElement&& other) noexcept = default;

ElementType SVGElement::type() const {
  return handle_.get<components::ElementTypeComponent>().type();
}

std::optional<ElementType> SVGElement::tryType() const {
  if (!handle_ || !handle_.all_of<components::ElementTypeComponent>()) {
    return std::nullopt;
  }

  return handle_.get<components::ElementTypeComponent>().type();
}

xml::XMLQualifiedNameRef SVGElement::tagName() const {
  return handle_.get<donner::components::TreeComponent>().tagName();
}

std::optional<xml::XMLQualifiedNameRef> SVGElement::tryTagName() const {
  if (!handle_ || !handle_.all_of<donner::components::TreeComponent>()) {
    return std::nullopt;
  }

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
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  // Explicitly remove and re-create, so that SVGDocumentContext can update its
  // id-to-entity map.
  handle_.remove<components::IdComponent>(access);
  if (!id.empty()) {
    handle_.emplace<components::IdComponent>(access, RcString(id));
  }

  handle_.get_or_emplace<donner::components::AttributesComponent>(access).setAttribute(
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
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  if (!name.empty()) {
    auto& component = handle_.get_or_emplace<components::ClassComponent>(access);
    component.className = name;
  } else {
    handle_.remove<components::ClassComponent>(access);
  }

  handle_.get_or_emplace<donner::components::AttributesComponent>(access).setAttribute(
      *handle_.registry(), xml::XMLQualifiedName("class"), RcString(name));

  // Class changes affect CSS selector matching, which can change any inherited property.
  markNeedsFullStyleRecompute(handle_);
  invalidateComputedStyle(handle_);
  invalidateComputedStyleForDescendants(handle_);
  markDirty(handle_, components::DirtyFlagsComponent::StyleCascade);
  propagateStyleDirtyToDescendants(handle_);
}

void SVGElement::setStyle(std::string_view style) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::StyleComponent>(access).setStyle(style);

  handle_.get_or_emplace<donner::components::AttributesComponent>(access).setAttribute(
      *handle_.registry(), xml::XMLQualifiedName("style"), RcString(style));

  components::StyleSystem().invalidateAll(handle_);
  markNeedsFullStyleRecompute(handle_);
  invalidateComputedStyleForDescendants(handle_);

  markDirty(handle_, components::DirtyFlagsComponent::StyleCascade);
  propagateStyleDirtyToDescendants(handle_);
}

void SVGElement::updateStyle(std::string_view style) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  components::StyleSystem().updateStyle(handle_, style);

  // TODO(jwmcglynn): Update the style attribute too
  // handle_.get_or_emplace<donner::components::AttributesComponent>(access).setAttribute(
  //     *handle_.registry(), xml::XMLQualifiedName("style"), RcString(style));

  markNeedsFullStyleRecompute(handle_);
  invalidateComputedStyle(handle_);
  invalidateComputedStyleForDescendants(handle_);

  markDirty(handle_, components::DirtyFlagsComponent::StyleCascade);
  propagateStyleDirtyToDescendants(handle_);
}

ParseResult<bool> SVGElement::trySetPresentationAttribute(std::string_view name,
                                                          std::string_view value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
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
      handle_.get_or_emplace<components::StyleComponent>(access).trySetPresentationAttribute(
          handle_, actualName, value);

  if (trySetResult.hasError()) {
    mutation.cancel();
    return trySetResult;
  }

  if (!trySetResult.result()) {
    // Try element-specific presentation attributes (cx, cy, r, rx, ry, d, etc.).
    parser::PropertyParseFnParams params = parser::PropertyParseFnParams::CreateForAttribute(value);
    trySetResult = parser::ParsePresentationAttribute(type(), handle_, actualName, params);
  }

  if (trySetResult.hasResult() && trySetResult.result()) {
    // Set succeeded, so store the attribute value.
    handle_.get_or_emplace<donner::components::AttributesComponent>(access).setAttribute(
        *handle_.registry(), xml::XMLQualifiedName(RcString(name)), RcString(value));

    // Mark dirty flags based on the attribute type.
    if (actualName == "transform") {
      components::LayoutSystem().invalidate(handle_);
      markDirty(handle_, components::DirtyFlagsComponent::RenderInstance);
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

  mutation.cancel();
  return trySetResult;
}

bool SVGElement::hasAttribute(const xml::XMLQualifiedNameRef& name) const {
  const auto* attributes = handle_.try_get<donner::components::AttributesComponent>();
  return attributes != nullptr && attributes->hasAttribute(name);
}

std::optional<RcString> SVGElement::getAttribute(const xml::XMLQualifiedNameRef& name) const {
  const auto* attributes = handle_.try_get<donner::components::AttributesComponent>();
  return attributes != nullptr ? attributes->getAttribute(name) : std::nullopt;
}

SmallVector<xml::XMLQualifiedNameRef, 1> SVGElement::findMatchingAttributes(
    const xml::XMLQualifiedNameRef& matcher) const {
  const auto* attributes = handle_.try_get<donner::components::AttributesComponent>();
  return attributes != nullptr ? attributes->findMatchingAttributes(matcher)
                               : SmallVector<xml::XMLQualifiedNameRef, 1>();
}

void SVGElement::setAttribute(const xml::XMLQualifiedNameRef& name, std::string_view value) {
  SVGDocument document = ownerDocument();
  (void)document.setElementAttribute(*this, name, value);
}

std::optional<ParseDiagnostic> SVGElement::setAttributeFromXMLMutation(
    const xml::XMLQualifiedNameRef& name, std::string_view value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  // TODO: Namespace support for these attributes
  // First check some special cases which will never be presentation attributes.
  if (name == xml::XMLQualifiedNameRef("id")) {
    setId(value);
    return std::nullopt;
  } else if (name == xml::XMLQualifiedNameRef("class")) {
    setClassName(value);
    return std::nullopt;
  } else if (name == xml::XMLQualifiedNameRef("style")) {
    setStyle(value);
    return std::nullopt;
  }

  // If it's not in the list above, it may be presentation attribute.
  // TODO(jwmcglynn): Add support for namespace when parsing presentation attributes.
  // Only parse empty namespaces for now.
  if (name.namespacePrefix.empty()) {
    auto trySetResult = trySetPresentationAttribute(name.name, value);
    const bool attributeWasSet = trySetResult.hasResult() && trySetResult.result();
    if (attributeWasSet) {
      if (name.name == "transform") {
        // Source/XML transform edits are settled document changes, not active drag frames.
        // Keep canvas drag mutations on the transform-only fast path, but force XML-sourced
        // transform changes through the normal dirty reraster path so filtered layer pixels
        // settle exactly.
        markDirty(handle_, components::DirtyFlagsComponent::Paint);
      }
      // Early-return since if this succeeds, the attribute has already been stored.
      return std::nullopt;
    }

    if (trySetResult.hasError()) {
      markNeedsFullStyleRecompute(handle_);
      handle_.get_or_emplace<donner::components::AttributesComponent>(access).setAttribute(
          *handle_.registry(), name, RcString(value));
      return std::move(trySetResult).error();
    }
  }

  // Otherwise store as a generic attribute.
  markNeedsFullStyleRecompute(handle_);
  handle_.get_or_emplace<donner::components::AttributesComponent>(access).setAttribute(
      *handle_.registry(), name, RcString(value));
  return std::nullopt;
}

void SVGElement::removeAttribute(const xml::XMLQualifiedNameRef& name) {
  SVGDocument document = ownerDocument();
  (void)document.removeElementAttribute(*this, name);
}

void SVGElement::removeAttributeFromXMLMutation(const xml::XMLQualifiedNameRef& name) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
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
  handle_.get_or_emplace<donner::components::AttributesComponent>(access).removeAttribute(
      *handle_.registry(), name);
}

SVGDocument SVGElement::ownerDocument() {
  DocumentReadAccess access = handle_.readAccess();
  SVGDocumentHandle documentState =
      access.registry().ctx().get<components::SVGDocumentContext>().getSharedDocumentState();
  return SVGDocument(std::move(documentState));
}

std::optional<SVGElement> SVGElement::parentElement() const {
  DocumentReadAccess access = handle_.readAccess();
  EntityHandle handle = handle_.resolve();
  const auto& tree = handle.get<donner::components::TreeComponent>();
  const EntityHandle parent(access.registry(), tree.parent());
  const bool isSVGElement = (parent && parent.all_of<components::ElementTypeComponent>());

  return isSVGElement ? std::make_optional(SVGElement(parent)) : std::nullopt;
}

std::optional<SVGElement> SVGElement::firstChild() const {
  if (std::optional<EntityHandle> handle = FirstElementChild(handle_)) {
    return SVGElement(*handle);
  }

  return std::nullopt;
}

std::optional<SVGElement> SVGElement::lastChild() const {
  if (std::optional<EntityHandle> handle = LastElementChild(handle_)) {
    return SVGElement(*handle);
  }

  return std::nullopt;
}

std::optional<SVGElement> SVGElement::previousSibling() const {
  if (std::optional<EntityHandle> handle = PreviousElementSibling(handle_)) {
    return SVGElement(*handle);
  }

  return std::nullopt;
}

std::optional<SVGElement> SVGElement::nextSibling() const {
  if (std::optional<EntityHandle> handle = NextElementSibling(handle_)) {
    return SVGElement(*handle);
  }

  return std::nullopt;
}

void SVGElement::insertBefore(const SVGElement& newNode, std::optional<SVGElement> referenceNode) {
  [[maybe_unused]] DocumentWriteAccess access = handle_.writeAccess();
  SVGDocument document = ownerDocument();
  if (document.hasSourceStore() && xml::XMLNode::TryCast(handle_).has_value()) {
    (void)document.insertElement(*this, newNode, referenceNode);
    return;
  }

  components::TreeMutation::InsertBefore(handle_, newNode.handle_,
                                         referenceNode ? referenceNode->handle_ : EntityHandle());
}

void SVGElement::appendChild(const SVGElement& child) {
  [[maybe_unused]] DocumentWriteAccess access = handle_.writeAccess();
  SVGDocument document = ownerDocument();
  if (document.hasSourceStore() && xml::XMLNode::TryCast(handle_).has_value()) {
    (void)document.insertElement(*this, child, std::nullopt);
    return;
  }

  components::TreeMutation::AppendChild(handle_, child.handle_);
}

void SVGElement::replaceChild(const SVGElement& newChild, const SVGElement& oldChild) {
  [[maybe_unused]] DocumentWriteAccess access = handle_.writeAccess();
  SVGDocument document = ownerDocument();
  if (document.hasSourceStore() && xml::XMLNode::TryCast(handle_).has_value()) {
    xml::ApplySourceEditResult insertResult = document.insertElement(*this, newChild, oldChild);
    if (!insertResult.diagnostic.has_value()) {
      (void)document.removeElement(oldChild);
    }
    return;
  }

  components::TreeMutation::ReplaceChild(handle_, newChild.handle_, oldChild.handle_);
}

void SVGElement::removeChild(const SVGElement& child) {
  [[maybe_unused]] DocumentWriteAccess access = handle_.writeAccess();
  const std::optional<SVGElement> childParent = child.parentElement();
  if (childParent.has_value() && *childParent == *this) {
    SVGDocument document = ownerDocument();
    if (document.hasSourceStore()) {
      (void)document.removeElement(child);
      return;
    }
  }

  components::TreeMutation::RemoveChild(handle_, child.handle_);
}

void SVGElement::remove() {
  [[maybe_unused]] DocumentWriteAccess access = handle_.writeAccess();
  SVGDocument document = ownerDocument();
  if (document.hasSourceStore() && xml::XMLNode::TryCast(handle_).has_value()) {
    (void)document.removeElement(*this);
    return;
  }

  components::TreeMutation::Remove(handle_);
}

std::optional<SVGElement> SVGElement::querySelector(std::string_view str) {
  const auto selectorResult = css::parser::SelectorParser::Parse(str);
  if (selectorResult.hasError()) {
    return std::nullopt;
  }

  const css::Selector& selector = selectorResult.result();
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  return details::QuerySelectorSearch(selector, handle_.resolve());
}

const PropertyRegistry& SVGElement::getComputedStyle() const {
  [[maybe_unused]] DocumentWriteAccess access = handle_.writeAccess();
  ParseWarningSink disabledSink = ParseWarningSink::Disabled();
  const components::ComputedStyleComponent& computedStyle =
      components::StyleSystem().computeStyle(handle_, disabledSink);
  return computedStyle.properties.value();
}

DocumentWriteAccess SVGElement::CreateElementWriteAccess(SVGDocument& document) {
  return document.writeAccess();
}

DocumentMutationBatch SVGElement::CreateElementMutationBatch(SVGDocument& document) {
  return DocumentMutationBatch(*document.handle(), true);
}

EntityHandle SVGElement::CreateEmptyEntity(SVGDocument& document) {
  DocumentMutationBatch mutation = CreateElementMutationBatch(document);
  DocumentWriteAccess& access = mutation.access();
  EntityHandle handle = CreateEmptyEntity(access);
  return handle;
}

EntityHandle SVGElement::CreateEmptyEntity(DocumentWriteAccess& access) {
  Registry& registry = access.registry();
  Entity entity = registry.create();

  return EntityHandle(registry, entity);
}

void SVGElement::CreateEntityOn(EntityHandle handle, const xml::XMLQualifiedNameRef& tagName,
                                ElementType type) {
  if (!handle.all_of<donner::components::TreeComponent>()) {
    handle.emplace<donner::components::TreeComponent>(tagName);
  }
  const auto& tree = handle.get<donner::components::TreeComponent>();
  handle.emplace<components::ElementTypeComponent>(type);
  handle.emplace<components::TransformComponent>();
  auto& lifetime = handle.get_or_emplace<components::NodeLifetimeComponent>();
  if (tree.parent() != entt::null) {
    lifetime.markAttached();
  } else {
    lifetime.markDetached(handle.entity());
  }
}

}  // namespace donner::svg
