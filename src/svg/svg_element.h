#pragma once

#include <functional>
#include <string_view>

#include "src/base/rc_string.h"
#include "src/base/transform.h"
#include "src/base/utils.h"
#include "src/svg/components/registry.h"
#include "src/svg/properties/property_registry.h"

namespace donner {
struct ParseError;
}

namespace donner::svg {

class SVGDocument;

class SVGElement {
protected:
  explicit SVGElement(EntityHandle handle);

public:
  SVGElement(const SVGElement& other);
  SVGElement& operator=(const SVGElement& other);

  ElementType type() const;
  RcString typeString() const;

  Entity entity() const;

  RcString id() const;
  void setId(std::string_view id);

  RcString className() const;
  void setClassName(std::string_view name);

  Transformd transform() const;
  void setTransform(Transformd transform);

  void setStyle(std::string_view style);
  bool trySetPresentationAttribute(std::string_view name, std::string_view value);

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

class SVGGraphicsElement : public SVGElement {
protected:
  explicit SVGGraphicsElement(EntityHandle handle) : SVGElement(handle) {}
};

class SVGGeometryElement : public SVGGraphicsElement {};

}  // namespace donner::svg
