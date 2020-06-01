#include "src/svg/components/tree_component.h"

#include "src/base/utils.h"

namespace donner {

void TreeComponent::insertBefore(Registry& registry, Entity newNode, Entity referenceNode) {
  assert(newNode != entt::null && "newNode is null");
  // referenceNode may be null.

  auto& newTree = registry.get<TreeComponent>(newNode);
  newTree.remove(registry);

  if (referenceNode != entt::null) {
    auto& referenceTree = registry.get<TreeComponent>(referenceNode);
    UTILS_RELEASE_ASSERT(referenceTree.parent_ == self_);

    newTree.previous_sibling_ = referenceTree.previous_sibling_;
    newTree.next_sibling_ = referenceNode;
    referenceTree.previous_sibling_ = newNode;

    // If there is no previous_sibling_, this was the first node.
    if (newTree.previous_sibling_ == entt::null) {
      first_child_ = newNode;
    } else {
      auto& previousTree = registry.get<TreeComponent>(newTree.previous_sibling_);
      previousTree.next_sibling_ = newNode;
    }

  } else {
    // If no referenceNode is provided, insert at the end.
    if (last_child_ != entt::null) {
      auto& lastTree = registry.get<TreeComponent>(last_child_);
      assert(lastTree.next_sibling_ == entt::null);
      lastTree.next_sibling_ = newNode;
      newTree.previous_sibling_ = last_child_;

      last_child_ = newNode;

    } else {
      // No children.
      assert(first_child_ == entt::null);
      assert(last_child_ == entt::null);

      first_child_ = newNode;
      last_child_ = newNode;
    }
  }

  newTree.parent_ = self_;
}

void TreeComponent::appendChild(Registry& registry, Entity child) {
  assert(child != entt::null && "child is null");
  UTILS_RELEASE_ASSERT(child != self_);

  auto& childTree = registry.get<TreeComponent>(child);
  childTree.remove(registry);
  childTree.parent_ = self_;

  if (last_child_ != entt::null) {
    auto& lastChildTree = registry.get<TreeComponent>(last_child_);
    assert(lastChildTree.next_sibling_ == entt::null);

    childTree.previous_sibling_ = last_child_;
    lastChildTree.next_sibling_ = child;

  } else {
    // No children.
    assert(first_child_ == entt::null);
    first_child_ = child;
  }

  last_child_ = child;
}

void TreeComponent::replaceChild(Registry& registry, Entity newChild, Entity oldChild) {
  assert(newChild != entt::null && "newChild is null");
  assert(oldChild != entt::null && "oldChild is null");
  UTILS_RELEASE_ASSERT(newChild != self_);

  auto& oldChildTree = registry.get<TreeComponent>(oldChild);
  UTILS_RELEASE_ASSERT(oldChildTree.parent_ == self_);

  const Entity oldChildNext = oldChildTree.next_sibling_;
  oldChildTree.remove(registry);
  insertBefore(registry, newChild, oldChildNext);
}

void TreeComponent::removeChild(Registry& registry, Entity child) {
  assert(child != entt::null && "child is null");

  auto& childTree = registry.get<TreeComponent>(child);
  UTILS_RELEASE_ASSERT(childTree.parent_ == self_);
  childTree.remove(registry);
}

void TreeComponent::remove(Registry& registry) {
  if (parent_ != entt::null) {
    auto& parentTree = registry.get<TreeComponent>(parent_);

    // Remove from parent.
    if (parentTree.first_child_ == self_) {
      parentTree.first_child_ = next_sibling_;
    }
    if (parentTree.last_child_ == self_) {
      parentTree.last_child_ = previous_sibling_;
    }

    // Remove from previous sibling.
    if (previous_sibling_ != entt::null) {
      auto& previousTree = registry.get<TreeComponent>(previous_sibling_);
      previousTree.next_sibling_ = next_sibling_;
    }

    // Remove from next sibling.
    if (next_sibling_ != entt::null) {
      auto& nextTree = registry.get<TreeComponent>(next_sibling_);
      nextTree.previous_sibling_ = previous_sibling_;
    }

    // Clear out tree state.
    parent_ = entt::null;
    previous_sibling_ = entt::null;
    next_sibling_ = entt::null;
  }
}

}  // namespace donner