#pragma once
/// @file

#include <string_view>

#include "donner/base/EcsRegistry.h"
#include "donner/base/RcString.h"
#include "donner/base/SmallVector.h"
#include "donner/base/Utils.h"
#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/svg/properties/PropertyRegistry.h"

namespace donner::base::parser {

// Forward declaration, #include "donner/base/parser/ParseResult.h"
template <typename T>
class ParseResult;

}  // namespace donner::base::parser

namespace donner::svg {

// Forward declaration, #include "donner/svg/SVGDocument.h"
class SVGDocument;

// Forward declaration, #include "donner/svg/DonnerController.h"
class DonnerController;

/**
 * Represents an SVG entity belonging to an \ref SVGDocument.
 *
 * Each \ref SVGElement may only belong to a single document, and each document can have only one
 * root. SVGDocument is responsible for managing the lifetime of all elements in the document, by
 * storing a shared pointer to the internal Registry data-store.
 *
 * Data is stored using the Entity Component System (\ref EcsArchitecture) pattern, which is a
 * data-oriented design optimized for fast data access and cache locality, particularly during
 * rendering.
 *
 * SVGDocument and \ref SVGElement provide a facade over the ECS, and surface a familiar Document
 * Object Model (DOM) API to traverse and manipulate the document tree, which is internally stored
 * within Components in the ECS.  This makes \ref SVGElement a thin wrapper around an \ref Entity,
 * making the object lightweight and usable on the stack.
 *
 * @see \ref SVGDocument
 * @see \ref EcsArchitecture
 */
class SVGElement {
  friend class DonnerController;

protected:
  /**
   * Internal constructor to create an SVGElement from an \ref EntityHandle.
   *
   * To create an SVGElement, use the static \c Create methods on the derived class, such as \ref
   * SVGCircleElement::Create.
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
  ~SVGElement() noexcept = default;

  /// Create another reference to the same SVGElement.
  SVGElement& operator=(const SVGElement& other);

  /// Move assignment operator.
  SVGElement& operator=(SVGElement&& other) noexcept;

  /// Get the ElementType for known XML element types.
  ElementType type() const;

  /// Get the XML tag name string for this element.
  XMLQualifiedNameRef tagName() const;

  /// Returns true if this is a known element type, returns false if this is an \ref
  /// SVGUnknownElement.
  bool isKnownType() const;

  /// Get the underlying \ref Entity, for advanced use-cases that require direct access to the ECS.
  Entity entity() const;

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
   * attribute for this element, or a \ref donner::base::parser::ParseError if the value is invalid.
   */
  base::parser::ParseResult<bool> trySetPresentationAttribute(std::string_view name,
                                                              std::string_view value);

  /**
   * Returns true if the element has an attribute with the given name.
   *
   * @param name Name of the attribute to check.
   * @return true if the attribute exists, false otherwise.
   */
  bool hasAttribute(const XMLQualifiedNameRef& name) const;

  /**
   * Get the value of an attribute, if it exists.
   *
   * @param name Name of the attribute to get.
   * @return The value of the attribute, or \c std::nullopt if the attribute does not exist.
   */
  std::optional<RcString> getAttribute(const XMLQualifiedNameRef& name) const;

  /**
   * Find attributes matching the given name matcher.
   *
   * @param matcher Matcher to use to find attributes. If \ref XMLQualifiedNameRef::namespacePrefix
   * is "*", the matcher will match any namespace with the given attribute name.
   * @return A vector of attributes matching the given name matcher.
   */
  SmallVector<XMLQualifiedNameRef, 1> findMatchingAttributes(
      const XMLQualifiedNameRef& matcher) const;

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
  void setAttribute(const XMLQualifiedNameRef& name, std::string_view value);

  /**
   * Remove an attribute, which may be either a presentation attribute or custom user-provided
   * attribute.
   *
   * If this is a presentation attribute, the presentation attributes value will be removed
   * (internally by setting the value to 'inherit').
   *
   * @param name Name of the attribute to remove.
   */
  void removeAttribute(const XMLQualifiedNameRef& name);

  /**
   * Get the \ref SVGDocument that holds this element.
   */
  SVGDocument& ownerDocument();

  /**
   * Get this element's parent, if it exists. If the parent is not set, this document is either the
   * root element or has not been inserted into the document tree.
   *
   * @return The parent element, or \c std::nullopt if the parent is not set.
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
   * @return The next sibling element, or \c std::nullopt if the element has no next sibling.
   */
  std::optional<SVGElement> nextSibling() const;

  /**
   * Insert \p newNode as a child, before \p referenceNode. If \p referenceNode is std::nullopt,
   * append the child.
   *
   * If \p newNode is already in the tree, it is first removed from its parent. However, if
   * inserting the child will create a cycle, the behavior is undefined.
   *
   * @param newNode New node to insert.
   * @param referenceNode A child of this node to insert \p newNode before, or \c std::nullopt. Must
   * be a child of the current node.
   */
  SVGElement insertBefore(SVGElement newNode, std::optional<SVGElement> referenceNode);

  /**
   * Append \p child as a child of the current node.
   *
   * If child is already in the tree, it is first removed from its parent. However, if inserting
   * the child will create a cycle, the behavior is undefined.
   *
   * @param child Node to append.
   */
  SVGElement appendChild(SVGElement child);

  /**
   * Replace \p oldChild with \p newChild in the tree, removing \p oldChild and inserting \p
   * newChild in its place.
   *
   * If \p newChild is already in the tree, it is first removed from its parent. However, if
   * inserting the child will create a cycle, the behavior is undefined.
   *
   * @param newChild New child to insert.
   * @param oldChild Old child to remove, must be a child of the current node.
   */
  SVGElement replaceChild(SVGElement newChild, SVGElement oldChild);

  /**
   * Remove \p child from this node.
   *
   * @param child Child to remove, must be a child of the current node.
   */
  SVGElement removeChild(SVGElement child);

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
    // If there is a Derived::IsBaseOf(ElementType) method, we're casting to a base class. Call it
    // to ensure we can.
    if constexpr (requires { Derived::IsBaseOf(ElementType{}); }) {
      return Derived::IsBaseOf(type());
    } else {
      return Derived::Type == type();
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
    UTILS_RELEASE_ASSERT(isa<Derived>());
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
    UTILS_RELEASE_ASSERT(isa<Derived>());
    if (isa<Derived>()) {
      return cast<Derived>();
    }

    return std::nullopt;
  }

  /**
   * Find the first element in the tree that matches the given CSS selector.
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
   * @param selector CSS selector to match.
   * @return The first matching element, or `std::nullopt` if no element matches.
   */
  std::optional<SVGElement> querySelector(std::string_view selector);

  /**
   * Get the computed CSS style of this element, after the CSS cascade.
   */
  const PropertyRegistry& getComputedStyle() const;

protected:
  /**
   * Create a new Entity of the given type.
   *
   * @param registry Registry to create the entity in.
   * @param tagName XML element type, e.g. "svg" or "rect", which an optional namespace.
   * @param Type Type of the entity.
   */
  static EntityHandle CreateEntity(Registry& registry, const XMLQualifiedNameRef& tagName,
                                   ElementType Type);

  /// Get the underlying ECS Registry, which holds all data for the document, for advanced use.
  Registry& registry() const { return *handle_.registry(); }

  /**
   * Convert an Entity to an EntityHandle, for advanced use.
   *
   * @param entity Entity to convert.
   */
  EntityHandle toHandle(Entity entity) const { return EntityHandle(registry(), entity); }

  /// The underlying ECS Entity for this element, which holds all data.
  EntityHandle handle_;
};

}  // namespace donner::svg
