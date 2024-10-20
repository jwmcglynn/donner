#pragma once
/// @file

#include "donner/base/EcsRegistry.h"
#include "donner/base/RcString.h"
#include "donner/base/SmallVector.h"
#include "donner/base/xml/XMLQualifiedName.h"

namespace donner::components {

/**
 * Stores the tree structure for an XML element, such as the parent, children, and siblings.
 *
 * This component is added to all entities that are part of the SVG tree, and is used to navigate
 * the tree structure.
 */
class TreeComponent {
public:
  /**
   * Construct a new tree component with the given \p type and \p tagName.
   *
   * @param tagName The qualified tag name of the element, which may include a namespace. (e.g.
   * "svg")
   */
  explicit TreeComponent(const XMLQualifiedNameRef& tagName)
      : tagName_(RcString(tagName.namespacePrefix), RcString(tagName.name)) {}

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

  /// Get the qualified tag name of the element, e.g. "svg".
  XMLQualifiedNameRef tagName() const { return tagName_; }

  /// Get the parent of this node, if it has one. Returns \c entt::null if this is the root.
  Entity parent() const { return parent_; }

  /// Get the first child of this node, if it has one. Returns \c entt::null if this has no
  /// children.
  Entity firstChild() const { return firstChild_; }

  /// Get the last child of this node, if it has one. Returns \c entt::null if this has no children.
  Entity lastChild() const { return lastChild_; }

  /// Get the previous sibling of this node, if it has one. Returns \c entt::null if this is the
  /// first child.
  Entity previousSibling() const { return previousSibling_; }

  /// Get the next sibling of this node, if it has one. Returns \c entt::null if this is the last
  /// child.
  Entity nextSibling() const { return nextSibling_; }

private:
  XMLQualifiedName tagName_;  //!< Qualified tag name of the element, e.g. "svg"

  Entity parent_{entt::null};      //!< Parent of this node, or \c entt::null if this is the root.
  Entity firstChild_{entt::null};  //!< First child of this node, or \c entt::null if this has no
                                   //!< children.
  Entity lastChild_{entt::null};   //!< Last child of this node, or \c entt::null if this has no
                                   //!< children.
  Entity previousSibling_{entt::null};  //!< Previous sibling of this node, or \c entt::null if this
                                        //!< is the first child.
  Entity nextSibling_{entt::null};      //!< Next sibling of this node, or \c entt::null if this is
                                        //!< the last child.
};

// TODO(jwmcglynn): Find a better place for this helper
/**
 * Iterate over all children of the given entity and call the given functor for each child.
 * Iterates in pre-order traversal order.
 *
 * @param handle Entity handle to iterate over.
 * @param func Functor to call for each child.
 */
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
    for (entt::entity child = treeComponent.lastChild(); child != entt::null;
         child = registry.get<components::TreeComponent>(child).previousSibling()) {
      stack.push_back(child);
    }
  }
}

}  // namespace donner::components
