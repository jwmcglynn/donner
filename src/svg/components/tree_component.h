#pragma once

#include "src/base/rc_string.h"
#include "src/svg/components/registry.h"

namespace donner {

class TreeComponent {
public:
  TreeComponent(ElementType type, RcString typeString) : type_(type), typeString_(typeString) {}

  /**
   * Insert \a newNode as a child, before \a referenceNode. If \a referenceNode is entt::null,
   * append the child.
   *
   * If \a newNode is already in the tree, it is first removed from its parent. However, if
   * inserting the child will create a cycle, the behavior is undefined.
   *
   * @param newNode New node to insert.
   * @param referenceNode Nullable, a child of this node to insert \a newNode before. Must be a
   *                      child of the current node.
   */
  void insertBefore(TreeComponent* newNode, TreeComponent* referenceNode);

  /**
   * Append \a child as a child of the current node.
   *
   * If \a child is already in the tree, it is first removed from its parent. However, if inserting
   * the \a child will create a cycle, the behavior is undefined.
   *
   * @param child Node to append.
   */
  void appendChild(TreeComponent* child);

  /**
   * Replace \a oldChild with \a newChild in the tree, removing \a oldChild and inserting \a
   * newChild in its place.
   *
   * If \a newChild is already in the tree, it is first removed from its parent. However, if
   * inserting the child will create a cycle, the behavior is undefined.
   *
   * @param newChild New child to insert.
   * @param oldChild Old child to remove, must be a child of the current node.
   */
  void replaceChild(TreeComponent* newChild, TreeComponent* oldChild);

  /**
   * Remove \a child from this node.
   *
   * @param child Child to remove, must be a child of the current node.
   */
  void removeChild(TreeComponent* child);

  /**
   * Remove this node from its parent, if it has one. Has no effect if this has no parent.
   */
  void remove();

  ElementType type() const { return type_; }

  RcString typeString() const { return typeString_; }

  TreeComponent* parent() const { return parent_; }
  TreeComponent* firstChild() const { return firstChild_; }
  TreeComponent* lastChild() const { return lastChild_; }
  TreeComponent* previousSibling() const { return previousSibling_; }
  TreeComponent* nextSibling() const { return nextSibling_; }

private:
  ElementType type_;
  RcString typeString_;

  TreeComponent* parent_ = nullptr;
  TreeComponent* firstChild_ = nullptr;
  TreeComponent* lastChild_ = nullptr;
  TreeComponent* previousSibling_ = nullptr;
  TreeComponent* nextSibling_ = nullptr;
};

}  // namespace donner

// Ensure pointer stability, so that we can directly reference TreeComponents without invalidating
// them when entities are deleted.
template <>
struct entt::component_traits<donner::TreeComponent> : entt::basic_component_traits {
  static constexpr auto in_place_delete = true;
};
