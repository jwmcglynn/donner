#pragma once
/// @file

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

#include "donner/base/EcsRegistry.h"
#include "donner/base/ParseDiagnostic.h"
#include "donner/base/RcString.h"
#include "donner/base/SmallVector.h"
#include "donner/base/Utils.h"
#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/svg/SVGDocumentHandle.h"
#include "donner/svg/properties/PropertyRegistry.h"

namespace donner {

// Forward declaration, #include "donner/base/ParseResult.h"
template <typename T>
class ParseResult;

}  // namespace donner

namespace donner::svg {

// Forward declaration, #include "donner/svg/SVGDocument.h"
class SVGDocument;

// Forward declaration, #include "donner/svg/DonnerController.h"
class DonnerController;

namespace components {

struct NodeExternalRefState;

}  // namespace components

namespace parser {

// Forward declaration, for parser-only attribute projection.
class AttributeParser;

// Forward declaration, for friend internal data access.
class SVGParserImpl;

}  // namespace parser

/**
 * Lifetime-aware reference to an SVG element entity.
 *
 * `ElementAnchor` retains the owning document storage and records the entity generation observed
 * when the public DOM wrapper was created. Callers resolve it to an \ref EntityHandle only when
 * they need to touch ECS storage.
 */
class ElementAnchor {
public:
  /// Construct an empty anchor.
  ElementAnchor() = default;

  /**
   * Construct an anchor from an entity handle.
   *
   * @param handle Entity handle to retain.
   */
  explicit ElementAnchor(EntityHandle handle);

  /// Copy constructor, retaining another public reference.
  ElementAnchor(const ElementAnchor& other);

  /// Move constructor, transferring the public reference.
  ElementAnchor(ElementAnchor&& other) noexcept;

  /// Destructor, releasing the public reference.
  ~ElementAnchor() noexcept;

  /// Copy assignment, retaining another public reference.
  ElementAnchor& operator=(const ElementAnchor& other);

  /// Move assignment, transferring the public reference.
  ElementAnchor& operator=(ElementAnchor&& other) noexcept;

  /// Resolve this anchor to an \ref EntityHandle.
  EntityHandle resolve() const;

  /// Resolve this anchor without checking for a scoped document access guard.
  EntityHandle unsafeResolve() const;

  /// Resolve this anchor for existing internal APIs that still accept \ref EntityHandle.
  operator EntityHandle() const { return resolve(); }

  /// Returns true if this anchor currently resolves to a live entity.
  bool valid() const { return resolve().valid(); }

  /// Returns true if this anchor is non-empty.
  explicit operator bool() const { return documentHandle_ && entity_ != entt::null; }

  /// Get the retained document registry.
  Registry* registry() const {
    assertScopedEntityHandleAccessAllowed();
    return unsafeRegistry();
  }

  /// Get the retained document registry without checking for a scoped access guard.
  Registry* unsafeRegistry() const {
    return documentHandle_ ? &documentHandle_->registry() : nullptr;
  }

  /// Acquire read access to this anchor's document.
  DocumentReadAccess readAccess() const {
    UTILS_RELEASE_ASSERT_MSG(documentHandle_, "SVGElement has no document state");
    return documentHandle_->read();
  }

  /// Acquire write access to this anchor's document.
  DocumentWriteAccess writeAccess() const {
    UTILS_RELEASE_ASSERT_MSG(documentHandle_, "SVGElement has no document state");
    return documentHandle_->write();
  }

  /// Acquire batched write access to this anchor's document.
  DocumentMutationBatch mutationBatch(bool commitOnScopeExit = true) const {
    UTILS_RELEASE_ASSERT_MSG(documentHandle_, "SVGElement has no document state");
    return DocumentMutationBatch(*documentHandle_, commitOnScopeExit);
  }

  /// Assert that legacy raw ECS access is scoped when required by the document mode.
  void assertScopedEntityHandleAccessAllowed() const;

  /// Get the referenced entity id.
  Entity entity() const { return entity_; }

  /// Get the entity generation observed when this anchor was created.
  std::uint32_t generation() const { return generation_; }

  /// Test whether the resolved entity has all requested components.
  template <typename... Type>
  bool all_of() const {
    return resolve().all_of<Type...>();
  }

  /// Get a component from the resolved entity.
  template <typename Type>
  const Type& get() const {
    return resolve().get<Type>();
  }

  /// Get a mutable component from the resolved entity.
  template <typename Type>
  Type& get(DocumentWriteAccess& access) const {
    UTILS_RELEASE_ASSERT_MSG(documentHandle_.get() == &access.documentState(),
                             "ElementAnchor write access belongs to a different document");
    return EntityHandle(access.registry(), entity_).get<Type>();
  }

  /// Get a component pointer from the resolved entity, or null if it is absent.
  template <typename Type>
  const Type* try_get() const {
    return resolve().try_get<Type>();
  }

  /// Get a mutable component pointer from the resolved entity, or null if it is absent.
  template <typename Type>
  Type* try_get(DocumentWriteAccess& access) const {
    UTILS_RELEASE_ASSERT_MSG(documentHandle_.get() == &access.documentState(),
                             "ElementAnchor write access belongs to a different document");
    return EntityHandle(access.registry(), entity_).try_get<Type>();
  }

  /// Emplace a component on the resolved entity.
  template <typename Type, typename... Args>
  Type& emplace(DocumentWriteAccess& access, Args&&... args) const {
    UTILS_RELEASE_ASSERT_MSG(documentHandle_.get() == &access.documentState(),
                             "ElementAnchor write access belongs to a different document");
    return EntityHandle(access.registry(), entity_).emplace<Type>(std::forward<Args>(args)...);
  }

  /// Get or emplace a component on the resolved entity.
  template <typename Type, typename... Args>
  Type& get_or_emplace(DocumentWriteAccess& access, Args&&... args) const {
    UTILS_RELEASE_ASSERT_MSG(documentHandle_.get() == &access.documentState(),
                             "ElementAnchor write access belongs to a different document");
    return EntityHandle(access.registry(), entity_)
        .get_or_emplace<Type>(std::forward<Args>(args)...);
  }

  /// Emplace or replace a component on the resolved entity.
  template <typename Type, typename... Args>
  Type& emplace_or_replace(DocumentWriteAccess& access, Args&&... args) const {
    UTILS_RELEASE_ASSERT_MSG(documentHandle_.get() == &access.documentState(),
                             "ElementAnchor write access belongs to a different document");
    return EntityHandle(access.registry(), entity_)
        .emplace_or_replace<Type>(std::forward<Args>(args)...);
  }

  /// Remove components from the resolved entity.
  template <typename... Type>
  decltype(auto) remove(DocumentWriteAccess& access) const {
    UTILS_RELEASE_ASSERT_MSG(documentHandle_.get() == &access.documentState(),
                             "ElementAnchor write access belongs to a different document");
    return EntityHandle(access.registry(), entity_).remove<Type...>();
  }

  /// Compare two anchors by document storage and entity id.
  friend bool operator==(const ElementAnchor& lhs, const ElementAnchor& rhs) {
    return lhs.documentHandle_ == rhs.documentHandle_ && lhs.entity_ == rhs.entity_;
  }

  /// Compare two anchors by document storage and entity id.
  friend bool operator!=(const ElementAnchor& lhs, const ElementAnchor& rhs) {
    return !(lhs == rhs);
  }

private:
  void release() noexcept;

  SVGDocumentHandle documentHandle_;
  std::shared_ptr<components::NodeExternalRefState> externalRefs_;
  Entity entity_ = entt::null;
  std::uint32_t generation_ = 0;
};

/**
 * Represents a single SVG element (e.g., `<rect>`, `<circle>`, `<g>`, `<text>`, etc.) within an
 * \ref SVGDocument.
 *
 * SVGElement provides a DOM-like API for traversing the document tree (`parentElement()`,
 * `firstChild()`, `nextSibling()`), querying and setting attributes (`getAttribute()`,
 * `setAttribute()`), and modifying the tree structure (`appendChild()`, `removeChild()`).
 *
 * Elements are lightweight value types — copying an SVGElement creates another handle to the same
 * underlying element, not a deep copy. Use `isa<Derived>()` and `cast<Derived>()` to work with
 * element-specific APIs (e.g., `SVGCircleElement`, `SVGPathElement`).
 *
 * For advanced queries like hit-testing, see `DonnerController`.
 *
 * @see \ref SVGDocument
 */
class SVGElement {
  friend class parser::AttributeParser;
  friend class DonnerController;
  friend class SVGDocument;

protected:
  /**
   * Internal constructor to create an SVGElement from an \ref donner::EntityHandle.
   *
   * To create an SVGElement, use the static \c Create methods on the derived class, such as
   * \ref donner::svg::SVGCircleElement::Create.
   *
   * @param handle EntityHandle to wrap.
   */
  explicit SVGElement(EntityHandle handle);

public:
  /// Create another reference to the same SVGElement.
  SVGElement(const SVGElement& other);

  /// Move constructor.
  SVGElement(SVGElement&& other) noexcept;

  /// Destructor.
  ~SVGElement() noexcept;

  /// Create another reference to the same SVGElement.
  SVGElement& operator=(const SVGElement& other);

  /// Move assignment operator.
  SVGElement& operator=(SVGElement&& other) noexcept;

  /// Get the ElementType for known XML element types.
  ElementType type() const;

  /// Get the ElementType if this handle still has SVG element identity.
  std::optional<ElementType> tryType() const;

  /// Get the XML tag name string for this element.
  xml::XMLQualifiedNameRef tagName() const;

  /// Get the XML tag name if this handle still has XML tree identity.
  std::optional<xml::XMLQualifiedNameRef> tryTagName() const;

  /// Returns true if this is a known element type, returns false if this is an
  /// \ref donner::svg::SVGUnknownElement.
  bool isKnownType() const;

  /**
   * Get the underlying \ref donner::EntityHandle.
   *
   * This is an unsafe advanced escape hatch. In \ref ThreadingMode::ConcurrentDom, callers must
   * hold an explicit document access guard while reading or mutating the returned handle.
   */
  EntityHandle unsafeEntityHandle() const { return handle_.unsafeResolve(); }

  /// Get the underlying \ref donner::EntityHandle, for advanced use-cases that require direct
  /// access to the ECS.
  EntityHandle entityHandle() const {
    handle_.assertScopedEntityHandleAccessAllowed();
    return unsafeEntityHandle();
  }

  /**
   * Run a callback with scoped read access to this element's document and resolved entity handle.
   *
   * In \ref ThreadingMode::ConcurrentDom, use this to batch repeated reads such as sibling
   * traversal or descendant scans under one document read lock.
   *
   * @param callback Callable invoked as `callback(DocumentReadAccess&, EntityHandle)`.
   */
  template <typename Callback>
  decltype(auto) withReadAccess(Callback&& callback) const {
    DocumentReadAccess access = handle_.readAccess();
    EntityHandle handle = handle_.resolve();
    using Result = std::invoke_result_t<Callback, DocumentReadAccess&, EntityHandle>;
    if constexpr (std::is_void_v<Result>) {
      std::forward<Callback>(callback)(access, handle);
    } else {
      return std::forward<Callback>(callback)(access, handle);
    }
  }

  /**
   * Run a callback with scoped write access to this element's document and resolved entity handle.
   *
   * Nested DOM setters called by the callback reuse this write access and coalesce their mutation
   * revision bumps. Raw ECS mutations made through the handle should call
   * `DocumentWriteAccess::bumpMutationRevision()`.
   *
   * @param callback Callable invoked as `callback(DocumentWriteAccess&, EntityHandle)`.
   */
  template <typename Callback>
  decltype(auto) withWriteAccess(Callback&& callback) const {
    DocumentMutationBatch batch = handle_.mutationBatch(false);
    EntityHandle handle = handle_.resolve();
    using Result = std::invoke_result_t<Callback, DocumentWriteAccess&, EntityHandle>;
    if constexpr (std::is_void_v<Result>) {
      std::forward<Callback>(callback)(batch.access(), handle);
    } else {
      return std::forward<Callback>(callback)(batch.access(), handle);
    }
  }

  /// Get the element id, the value of the "id" attribute.
  RcString id() const;

  /**
   * Set the element id, the value of the "id" attribute.
   *
   * @param id New id to set.
   */
  void setId(std::string_view id);

  /// Get the element class name, the value of the "class" attribute.
  RcString className() const;

  /**
   * Set the element class name, the value of the "class" attribute.
   *
   * @param name New class name to set.
   */
  void setClassName(std::string_view name);

  /**
   * Set the element style, the value of the "style" attribute.
   *
   * @param style New style to set.
   */
  void setStyle(std::string_view style);

  /**
   * Update the element style, adding new attributes or overridding existing ones (without removing
   * them).
   *
   * @param style Style updates to apply, as a CSS style string (e.g. "fill:red;").
   */
  void updateStyle(std::string_view style);

  /**
   * Set the value of a presentation attribute, such as "fill" or "stroke". Note that this accepts
   * the CSS value, not the XML attribute value.
   *
   * For example, for the following XML attributes they need to be mapped as follows before calling:
   * - `gradientTransform` -> `transform`
   * - `patternTransform` -> `transform`
   *
   * @param name Name of the attribute to set.
   * @param value New value to set.
   * @return true if the attribute was set, false if the attribute is not a valid presentation
   * attribute for this element, or a \ref ParseDiagnostic if the value is invalid.
   */
  ParseResult<bool> trySetPresentationAttribute(std::string_view name, std::string_view value);

  /**
   * Returns true if the element has an attribute with the given name.
   *
   * @param name Name of the attribute to check.
   * @return true if the attribute exists, false otherwise.
   */
  bool hasAttribute(const xml::XMLQualifiedNameRef& name) const;

  /**
   * Get the value of an attribute, if it exists.
   *
   * @param name Name of the attribute to get.
   * @return The value of the attribute, or \c std::nullopt if the attribute does not exist.
   */
  std::optional<RcString> getAttribute(const xml::XMLQualifiedNameRef& name) const;

  /**
   * Find attributes matching the given name matcher.
   *
   * @param matcher Matcher to use to find attributes. If
   * \ref donner::xml::XMLQualifiedNameRef::namespacePrefix is `*`, the matcher will match any
   * namespace with the given attribute name.
   * @return A vector of attributes matching the given name matcher.
   */
  SmallVector<xml::XMLQualifiedNameRef, 1> findMatchingAttributes(
      const xml::XMLQualifiedNameRef& matcher) const;

  /**
   * Get the list of attributes for this element.
   *
   * @return The qualified names of attributes present on this element.
   */
  SmallVector<xml::XMLQualifiedNameRef, 10> attributes() const;

  /**
   * Set the value of a generic XML attribute, which may be either a presentation attribute or
   * custom user-provided attribute.
   *
   * This API supports a superset of \ref trySetPresentationAttribute, however its parse errors are
   * ignored. If the attribute is not a presentation attribute, or there are parse errors the
   * attribute will be stored as a custom attribute instead.
   *
   * @param name Name of the attribute to set.
   * @param value New value to set.
   */
  void setAttribute(const xml::XMLQualifiedNameRef& name, std::string_view value);

  /**
   * Remove an attribute, which may be either a presentation attribute or custom user-provided
   * attribute.
   *
   * If this is a presentation attribute, the presentation attributes value will be removed
   * (internally by setting the value to 'inherit').
   *
   * @param name Name of the attribute to remove.
   */
  void removeAttribute(const xml::XMLQualifiedNameRef& name);

  /**
   * Get an owning reference to the \ref SVGDocument containing this element.
   */
  SVGDocument ownerDocument();

  /**
   * Get this element's parent, if it exists.
   *
   * @return The parent element, or \c std::nullopt if this is the root element or the element has
   * not been inserted into a document tree.
   */
  std::optional<SVGElement> parentElement() const;

  /**
   * Get the first child of this element, if it exists.
   *
   * @return The first child element, or \c std::nullopt if the element has no children.
   */
  std::optional<SVGElement> firstChild() const;

  /**
   * Get the last child of this element, if it exists.
   *
   * @return The last child element, or \c std::nullopt if the element has no children.
   */
  std::optional<SVGElement> lastChild() const;

  /**
   * Get the previous sibling of this element, if it exists.
   *
   * @return The previous sibling element, or \c std::nullopt if the element has no previous
   * sibling.
   */
  std::optional<SVGElement> previousSibling() const;

  /**
   * Get the next sibling of this element, if it exists.
   *
   * For tight traversal loops in \ref ThreadingMode::ConcurrentDom, wrap the loop in
   * \ref withReadAccess so each step can reuse the same read access.
   *
   * @return The next sibling element, or \c std::nullopt if the element has no next sibling.
   */
  std::optional<SVGElement> nextSibling() const;

  /**
   * Insert \p newNode as a child, before \p referenceNode. If \p referenceNode is std::nullopt,
   * append the child.
   *
   * If \p newNode is already in the tree, it is first removed from its parent before reinsertion.
   *
   * @pre \p newNode must not be an ancestor of this element (inserting a parent as a child of its
   * descendant would create a cycle, which is undefined behavior).
   *
   * @param newNode New node to insert.
   * @param referenceNode A child of this node to insert \p newNode before, or \c std::nullopt. Must
   * be a child of the current node.
   */
  void insertBefore(const SVGElement& newNode, std::optional<SVGElement> referenceNode);

  /**
   * Append \p child as a child of the current node.
   *
   * If child is already in the tree, it is first removed from its parent before reinsertion.
   *
   * @pre \p child must not be an ancestor of this element (would create a cycle).
   *
   * @param child Node to append.
   */
  void appendChild(const SVGElement& child);

  /**
   * Replace \p oldChild with \p newChild in the tree, removing \p oldChild and inserting \p
   * newChild in its place.
   *
   * If \p newChild is already in the tree, it is first removed from its parent before reinsertion.
   *
   * @pre \p newChild must not be an ancestor of this element (would create a cycle).
   *
   * @param newChild New child to insert.
   * @param oldChild Old child to remove, must be a child of the current node.
   */
  void replaceChild(const SVGElement& newChild, const SVGElement& oldChild);

  /**
   * Remove \p child from this node.
   *
   * @param child Child to remove, must be a child of the current node.
   */
  void removeChild(const SVGElement& child);

  /**
   * Remove this node from its parent, if it has one. Has no effect if this has no parent.
   */
  void remove();

  /**
   * Returns true if the two SVGElement handles reference the same underlying document.
   */
  bool operator==(const SVGElement& other) const { return handle_ == other.handle_; }

  /**
   * Returns true if the two SVGElement handles reference the same underlying document.
   */
  bool operator!=(const SVGElement& other) const { return handle_ != other.handle_; }

  /**
   * Return true if this element "is a" instance of type, if it be cast to a specific type with \ref
   * cast.
   *
   * @tparam Derived Type to check.
   */
  template <typename Derived>
  bool isa() const {
    if constexpr (std::is_same_v<Derived, SVGElement>) {
      return true;
    } else if constexpr (requires { Derived::Type; }) {
      static_assert(std::is_same_v<std::remove_cvref_t<decltype(Derived::Type)>, ElementType>,
                    "Derived::Type must be an ElementType");
      return Derived::Type == type();

      // If there is a Derived::IsBaseOf(ElementType) method, we're casting to a base class. Call it
      // to ensure we can.
    } else if constexpr (requires { Derived::IsBaseOf(ElementType{}); }) {
      return Derived::IsBaseOf(type());
    } else {
      static_assert(false, "isa<>() called on a type that is not a valid SVGElement");
    }
  }

  /**
   * Cast this element to its derived type.
   *
   * @pre Requires this element to be of type \p Derived::Type.
   * @return A reference to this element, cast to the derived type.
   */
  template <typename Derived>
  Derived cast() {
    UTILS_RELEASE_ASSERT(isa<Derived>());
    // Derived must inherit from SVGElement, and have no additional members.
    static_assert(std::is_base_of_v<SVGElement, Derived>);
    static_assert(sizeof(SVGElement) == sizeof(Derived));
    return *reinterpret_cast<Derived*>(this);  // NOLINT: Reinterpret cast validated by assert.
  }

  /**
   * Cast this element to its derived type (const version).
   *
   * @pre Requires this element to be of type \p Derived::Type.
   * @return A reference to this element, cast to the derived type.
   */
  template <typename Derived>
  Derived cast() const {
    UTILS_RELEASE_ASSERT(isa<Derived>());
    // Derived must inherit from SVGElement, and have no additional members.
    static_assert(std::is_base_of_v<SVGElement, Derived>);
    static_assert(sizeof(SVGElement) == sizeof(Derived));
    // NOLINTNEXTLINE: Reinterpret cast validated by assert.
    return *reinterpret_cast<const Derived*>(this);
  }

  /**
   * Cast this element to its derived type, if possible. Return \c std::nullopt otherwise.
   *
   * @return A reference to this element, cast to the derived type, or \c std::nullopt if the
   * element is not of the correct type.
   */
  template <typename Derived>
  std::optional<Derived> tryCast() {
    if (isa<Derived>()) {
      return cast<Derived>();
    }

    return std::nullopt;
  }

  /**
   * Cast this element to its derived type, if possible. Return \c std::nullopt otherwise (const
   * version).
   *
   * @return A reference to this element, cast to the derived type, or \c std::nullopt if the
   * element is not of the correct type.
   */
  template <typename Derived>
  std::optional<const Derived> tryCast() const {
    if (isa<Derived>()) {
      return cast<Derived>();
    }

    return std::nullopt;
  }

  /**
   * Find the first element in the tree that matches the given CSS selector.
   *
   * This method performs its own scoped read. For repeated DOM reads in
   * \ref ThreadingMode::ConcurrentDom, wrap the surrounding scan in \ref withReadAccess so nested
   * reads reuse the same document access.
   *
   * ```
   * auto element = document.svgElement().querySelector("#elementId");
   * ```
   *
   * To find things relative to the current element, use `:scope`:
   * ```
   * auto rectInElement = element.querySelector(":scope > rect");
   * ```
   *
   * @param selector CSS selector to match. If the selector string is invalid, returns
   * \c std::nullopt (no error is reported).
   * @return The first matching element, or `std::nullopt` if no element matches.
   */
  std::optional<SVGElement> querySelector(std::string_view selector);

  /**
   * Get the computed CSS style of this element, after the CSS cascade has been applied. The
   * returned \ref donner::svg::PropertyRegistry contains resolved values for all CSS properties
   * (fill, stroke, font-size, etc.).
   */
  const PropertyRegistry& getComputedStyle() const;

protected:
  /**
   * Scoped mutation helper that runs cleanup before committing the mutation revision.
   *
   * This is intended for element setters that need to invalidate cached computed state after the
   * raw SVG attribute component is changed. The callback runs while the write access from the
   * mutation batch is still active, and before the coalesced mutation revision is committed.
   */
  template <typename InvalidateCallback>
  class ScopedMutation {
  public:
    ScopedMutation(const ElementAnchor& handle, InvalidateCallback invalidateOnScopeExit)
        : mutation_(handle.mutationBatch()),
          invalidateOnScopeExit_(std::move(invalidateOnScopeExit)) {}

    ScopedMutation(const ScopedMutation& other) = delete;
    ScopedMutation(ScopedMutation&& other) = delete;
    ScopedMutation& operator=(const ScopedMutation& other) = delete;
    ScopedMutation& operator=(ScopedMutation&& other) = delete;

    ~ScopedMutation() { invalidateOnScopeExit_(); }

    /// Get the active write access for this mutation.
    DocumentWriteAccess& access() { return mutation_.access(); }

  private:
    DocumentMutationBatch mutation_;
    InvalidateCallback invalidateOnScopeExit_;
  };

  /**
   * Create a scoped mutation that runs \p invalidateOnScopeExit before the revision commit.
   *
   * @param invalidateOnScopeExit Callable invoked while write access is still held.
   */
  template <typename InvalidateCallback>
  ScopedMutation<std::decay_t<InvalidateCallback>> mutationScope(
      InvalidateCallback&& invalidateOnScopeExit) const {
    return ScopedMutation<std::decay_t<InvalidateCallback>>(
        handle_, std::forward<InvalidateCallback>(invalidateOnScopeExit));
  }

  /**
   * Set an attribute from an XML mutation and return any SVG semantic parse diagnostic.
   *
   * Invalid presentation-attribute values are still stored in the XML attribute projection, but
   * leave the previous valid SVG semantic component in place.
   *
   * @param name Name of the attribute to set.
   * @param value New value to set.
   */
  std::optional<ParseDiagnostic> setAttributeFromXMLMutation(const xml::XMLQualifiedNameRef& name,
                                                             std::string_view value);

  /**
   * Remove an attribute from an XML mutation without writing back to XML source.
   *
   * @param name Name of the attribute to remove.
   */
  void removeAttributeFromXMLMutation(const xml::XMLQualifiedNameRef& name);

  /**
   * Acquire write access for creating an element in a document.
   *
   * @param document Containing document.
   */
  static DocumentWriteAccess CreateElementWriteAccess(SVGDocument& document);

  /**
   * Acquire a mutation scope for creating an element in a document.
   *
   * @param document Containing document.
   */
  static DocumentMutationBatch CreateElementMutationBatch(SVGDocument& document);

  /**
   * Create a new Entity within the document ECS, and return a handle to it.
   *
   * @param document Containing document.
   */
  static EntityHandle CreateEmptyEntity(SVGDocument& document);

  /**
   * Create a new Entity within a guarded document ECS, and return a handle to it.
   *
   * @param access Active write access for the containing document.
   */
  static EntityHandle CreateEmptyEntity(DocumentWriteAccess& access);

  /**
   * Create a new SVG element instance on a given \ref donner::Entity.
   *
   * @param handle Entity to create the element on.
   * @param tagName XML element type, e.g. "svg" or "rect", which an optional namespace.
   * @param Type Type of the entity.
   */
  static void CreateEntityOn(EntityHandle handle, const xml::XMLQualifiedNameRef& tagName,
                             ElementType Type);

  /// Get the underlying ECS Registry, which holds all data for the document, for advanced use.
  Registry& unsafeRegistry() const { return *handle_.unsafeRegistry(); }

  /// Get the underlying ECS Registry, which holds all data for the document, for advanced use.
  Registry& registry() const { return *handle_.registry(); }

  /**
   * Convert an Entity to an EntityHandle, for advanced use.
   *
   * @param entity Entity to convert.
   */
  EntityHandle toHandle(Entity entity) const { return EntityHandle(registry(), entity); }

  /// The lifetime-aware ECS entity anchor for this element.
  ElementAnchor handle_;
};

}  // namespace donner::svg
