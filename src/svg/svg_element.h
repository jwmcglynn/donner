#pragma once

#include <functional>
#include <string_view>

#include "src/base/rc_string.h"
#include "src/base/transform.h"
#include "src/base/utils.h"
#include "src/svg/components/registry.h"

namespace donner {

class SVGDocument;
struct ParseError;

class SVGElement {
protected:
  SVGElement(Registry& registry, Entity entity);

public:
  SVGElement(const SVGElement& other);
  SVGElement& operator=(const SVGElement& other);

  ElementType type() const;
  RcString typeString() const;

  Entity entity() const;

  std::string id() const;
  void setId(std::string_view id);

  std::string className() const;
  void setClassName(std::string_view name);

  Transformd transform() const;
  void setTransform(Transformd transform);

  void setStyle(std::string_view style);
  bool trySetPresentationAttribute(std::string_view name, std::string_view value);

  std::optional<SVGElement> parentElement();
  std::optional<SVGElement> firstChild();
  std::optional<SVGElement> lastChild();
  std::optional<SVGElement> previousSibling();
  std::optional<SVGElement> nextSibling();

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

  bool operator==(const SVGElement& other) const { return entity_ == other.entity_; }

  template <typename Derived>
  Derived cast() {
    UTILS_RELEASE_ASSERT(Derived::Type == type());
    static_assert(sizeof(SVGElement) == sizeof(Derived));
    return *reinterpret_cast<Derived*>(this);
  }

protected:
  static Entity CreateEntity(Registry& registry, RcString typeString, ElementType Type);

  std::reference_wrapper<Registry> registry_;
  Entity entity_;
};

class SVGGraphicsElement : public SVGElement {
protected:
  SVGGraphicsElement(Registry& registry, Entity entity) : SVGElement(registry, entity) {}
};

class SVGGeometryElement : public SVGGraphicsElement {};

}  // namespace donner