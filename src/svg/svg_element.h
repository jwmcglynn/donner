#pragma once
/// @file

#include <functional>
#include <string_view>

#include "src/base/rc_string.h"
#include "src/base/transform.h"
#include "src/base/utils.h"
#include "src/svg/properties/property_registry.h"
#include "src/svg/registry/registry.h"

namespace donner {
struct ParseError;
}

namespace donner::svg {

class SVGDocument;

/**
 * Represents an SVG entity belonging to an \ref SVGDocument.
 *
 * Each \ref SVGElement may only belong to a single document, and each document can have only one
 * root. SVGDocument is responsible for managing the lifetime of all elements in the document, by
 * storing a shared pointer to the internal Registry data-store.
 *
 * Data is stored using the Entity Component System (\ref ECS) pattern, which is a data-oriented
 * design optimized for fast data access and cache locality, particularly during rendering.
 *
 * SVGDocument and \ref SVGElement provide a facade over the \ref ECS, and surface a familiar
 * Document Object Model (DOM) API to traverse and manipulate the document tree, which is internally
 * stored within Components in the ECS.  This makes \ref SVGElement a thin wrapper around an \ref
 * Entity, making the object lightweight and usable on the stack.
 *
 * \see \ref SVGDocument
 * \see \ref ECS
 * \see \ref Component
 */
class SVGElement {
protected:
  /**
   * Internal constructor to create an SVGElement from an \ref EntityHandle.
   *
   * To create an SVGElement, use the static \ref Create methods on the derived class, such as \ref
   * SVGCircleElement::Create.
   *
   * @param handle EntityHandle to wrap.
   */
  explicit SVGElement(EntityHandle handle);

public:
  /// Create another reference to the same SVGElement.
  SVGElement(const SVGElement& other);

  /// Create another reference to the same SVGElement.
  SVGElement& operator=(const SVGElement& other);

  /// Get the ElementType for known XML element types.
  ElementType type() const;

  /// Get the XML tag name string for this element.
  RcString typeString() const;

  /// Get the underlying \ref Entity, for advanced use-cases that require direct access to the ECS.
  Entity entity() const;

  /// Get the element id, the value of the "id" attribute.
  RcString id() const;

  /// Set the element id, the value of the "id" attribute.
  void setId(std::string_view id);

  /// Get the element class name, the value of the "class" attribute.
  RcString className() const;

  /// Set the element class name, the value of the "class" attribute.
  void setClassName(std::string_view name);

  /// Get the element style, the value of the "style" attribute.
  void setStyle(std::string_view style);

  /// Set the value of a presentation attribute, such as "fill" or "stroke".
  ParseResult<bool> trySetPresentationAttribute(std::string_view name, std::string_view value);

  bool hasAttribute(std::string_view name) const;
  std::optional<RcString> getAttribute(std::string_view name) const;

  SVGDocument& ownerDocument();
  std::optional<SVGElement> parentElement() const;
  std::optional<SVGElement> firstChild() const;
  std::optional<SVGElement> lastChild() const;
  std::optional<SVGElement> previousSibling() const;
  std::optional<SVGElement> nextSibling() const;

  /**
   * Insert @a newNode as a child, before @a referenceNode. If @a referenceNode is std::nullopt,
   * append the child.
   *
   * If @a newNode is already in the tree, it is first removed from its parent. However, if
   * inserting the child will create a cycle, the behavior is undefined.
   *
   * @param newNode New node to insert.
   * @param referenceNode A child of this node to insert @a newNode before, or std::nullopt. Must be
   *                      a child of the current node.
   */
  SVGElement insertBefore(SVGElement newNode, std::optional<SVGElement> referenceNode);

  /**
   * Append @a child as a child of the current node.
   *
   * If child is already in the tree, it is first removed from its parent. However, if inserting
   * the child will create a cycle, the behavior is undefined.
   *
   * @param child Node to append.
   */
  SVGElement appendChild(SVGElement child);

  /**
   * Replace @a oldChild with @a newChild in the tree, removing @a oldChild and inserting @a
   * newChild in its place.
   *
   * If @a newChild is already in the tree, it is first removed from its parent. However, if
   * inserting the child will create a cycle, the behavior is undefined.
   *
   * @param newChild New child to insert.
   * @param oldChild Old child to remove, must be a child of the current node.
   */
  SVGElement replaceChild(SVGElement newChild, SVGElement oldChild);

  /**
   * Remove @a child from this node.
   *
   * @param child Child to remove, must be a child of the current node.
   */
  SVGElement removeChild(SVGElement child);

  /**
   * Remove this node from its parent, if it has one. Has no effect if this has no parent.
   */
  void remove();

  bool operator==(const SVGElement& other) const { return handle_ == other.handle_; }

  template <typename Derived>
  Derived cast() {
    UTILS_RELEASE_ASSERT(Derived::Type == type());
    // Derived must inherit from SVGElement, and have no additional members.
    static_assert(std::is_base_of_v<SVGElement, Derived>);
    static_assert(sizeof(SVGElement) == sizeof(Derived));
    return *reinterpret_cast<Derived*>(this);
  }

  std::optional<SVGElement> querySelector(std::string_view selector);

  const PropertyRegistry& getComputedStyle() const;

protected:
  static EntityHandle CreateEntity(Registry& registry, RcString typeString, ElementType Type);

  Registry& registry() const { return *handle_.registry(); }
  EntityHandle toHandle(Entity entity) const { return EntityHandle(registry(), entity); }

  EntityHandle handle_;
};

}  // namespace donner::svg
