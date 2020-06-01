#pragma once

#include "src/svg/components/registry.h"

namespace donner {

class TreeComponent {
public:
  TreeComponent(ElementType type, Entity self) : type_(type), self_(self) {}

  /**
   * Insert newNode as a child, before referenceNode. If referenceNode is entt::null, append the
   * child.
   *
   * If newNode is already in the tree, it is first removed from its parent. However, if inserting
   * the child will create a cycle, the behavior is undefined.
   *
   * @param registry Entity registry.
   * @param newNode New node to insert.
   * @param referenceNode Nullable, a child of this node to insert newNode before. Must be a child
   *                      of the current node.
   */
  void insertBefore(Registry& registry, Entity newNode, Entity referenceNode);

  /**
   * Append child as a child of the current node.
   *
   * If child is already in the tree, it is first removed from its parent. However, if inserting
   * the child will create a cycle, the behavior is undefined.
   *
   * @param registry Entity registry.
   * @param child Node to append.
   */
  void appendChild(Registry& registry, Entity child);

  /**
   * Replace oldChild with newChild in the tree, removing oldChild and inserting newChild in its
   * place.
   *
   * If newChild is already in the tree, it is first removed from its parent. However, if inserting
   * the child will create a cycle, the behavior is undefined.
   *
   * @param registry Entity registry.
   * @param newChild New child to insert.
   * @param oldChild Old child to remove, must be a child of the current node.
   */
  void replaceChild(Registry& registry, Entity newChild, Entity oldChild);

  /**
   * Remove child from this node.
   *
   * @param registry Entity registry.
   * @param child Child to remove, must be a child of the current node.
   */
  void removeChild(Registry& registry, Entity child);

  /**
   * Remove this node from its parent, if it has one. Has no effect if this has no parent.
   *
   * @param registry Entity registry.
   */
  void remove(Registry& registry);

  ElementType type() const { return type_; }

  Entity parent() const { return parent_; }
  Entity firstChild() const { return first_child_; }
  Entity lastChild() const { return last_child_; }
  Entity previousSibling() const { return previous_sibling_; }
  Entity nextSibling() const { return next_sibling_; }

private:
  ElementType type_;
  Entity self_{entt::null};

  Entity parent_{entt::null};
  Entity first_child_{entt::null};
  Entity last_child_{entt::null};
  Entity previous_sibling_{entt::null};
  Entity next_sibling_{entt::null};
};

}  // namespace donner