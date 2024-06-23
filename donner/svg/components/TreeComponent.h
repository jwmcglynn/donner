#pragma once
/// @file

#include "donner/base/RcString.h"
#include "donner/base/SmallVector.h"
#include "donner/svg/registry/Registry.h"
#include "donner/svg/xml/XMLQualifiedName.h"

namespace donner::svg::components {

class TreeComponent {
public:
  TreeComponent(ElementType type, const XMLQualifiedNameRef& xmlTypeName)
      : type_(type),
        xmlTypeName_(RcString(xmlTypeName.namespacePrefix), RcString(xmlTypeName.name)) {}

  /**
   * Insert \a newNode as a child, before \a referenceNode. If \a referenceNode is entt::null,
   * append the child.
   *
   * If \a newNode is already in the tree, it is first removed from its parent. However, if
   * inserting the child will create a cycle, the behavior is undefined.
   *
   * @param registry Entity registry.
   * @param newNode New node to insert.
   * @param referenceNode Nullable, a child of this node to insert \a newNode before. Must be a
   *                      child of the current node.
   */
  void insertBefore(Registry& registry, Entity newNode, Entity referenceNode);

  /**
   * Append \a child as a child of the current node.
   *
   * If \a child is already in the tree, it is first removed from its parent. However, if inserting
   * the \a child will create a cycle, the behavior is undefined.
   *
   * @param registry Entity registry.
   * @param child Node to append.
   */
  void appendChild(Registry& registry, Entity child);

  /**
   * Replace \a oldChild with \a newChild in the tree, removing \a oldChild and inserting \a
   * newChild in its place.
   *
   * If \a newChild is already in the tree, it is first removed from its parent. However, if
   * inserting the child will create a cycle, the behavior is undefined.
   *
   * @param registry Entity registry.
   * @param newChild New child to insert.
   * @param oldChild Old child to remove, must be a child of the current node.
   */
  void replaceChild(Registry& registry, Entity newChild, Entity oldChild);

  /**
   * Remove \a child from this node.
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

  XMLQualifiedNameRef xmlTypeName() const { return xmlTypeName_; }

  Entity parent() const { return parent_; }
  Entity firstChild() const { return firstChild_; }
  Entity lastChild() const { return lastChild_; }
  Entity previousSibling() const { return previousSibling_; }
  Entity nextSibling() const { return nextSibling_; }

private:
  ElementType type_;
  XMLQualifiedName xmlTypeName_;

  Entity parent_{entt::null};
  Entity firstChild_{entt::null};
  Entity lastChild_{entt::null};
  Entity previousSibling_{entt::null};
  Entity nextSibling_{entt::null};
};

// TODO: Find a better place for this helper
template <typename Func>
void ForAllChildren(EntityHandle handle, const Func& func) {
  assert(handle.valid());
  Registry& registry = *handle.registry();

  SmallVector<entt::entity, 4> stack;
  stack.push_back(handle.entity());

  while (!stack.empty()) {
    EntityHandle currentHandle = EntityHandle(registry, stack[stack.size() - 1]);
    stack.pop_back();

    // Call the functor for the current entity
    func(currentHandle);

    // Add all children to the stack
    auto& treeComponent = currentHandle.get<components::TreeComponent>();
    for (entt::entity child = treeComponent.firstChild(); child != entt::null;
         child = registry.get<components::TreeComponent>(child).nextSibling()) {
      stack.push_back(child);
    }
  }
}

}  // namespace donner::svg::components
