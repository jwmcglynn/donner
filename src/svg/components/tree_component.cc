#include "src/svg/components/tree_component.h"

#include "src/base/utils.h"

namespace donner::svg::components {

void TreeComponent::insertBefore(Registry& registry, Entity newNode, Entity referenceNode) {
  assert(newNode != entt::null && "newNode is null");
  // referenceNode may be null.

  const Entity self = entt::to_entity(registry.storage<TreeComponent>(), *this);

  auto& newTree = registry.get<TreeComponent>(newNode);
  newTree.remove(registry);

  if (referenceNode != entt::null) {
    auto& referenceTree = registry.get<TreeComponent>(referenceNode);
    UTILS_RELEASE_ASSERT(referenceTree.parent_ == self);

    newTree.previousSibling_ = referenceTree.previousSibling_;
    newTree.nextSibling_ = referenceNode;
    referenceTree.previousSibling_ = newNode;

    // If there is no previousSibling_, this was the first node.
    if (newTree.previousSibling_ == entt::null) {
      firstChild_ = newNode;
    } else {
      auto& previousTree = registry.get<TreeComponent>(newTree.previousSibling_);
      previousTree.nextSibling_ = newNode;
    }

  } else {
    // If no referenceNode is provided, insert at the end.
    if (lastChild_ != entt::null) {
      auto& lastTree = registry.get<TreeComponent>(lastChild_);
      assert(lastTree.nextSibling_ == entt::null);
      lastTree.nextSibling_ = newNode;
      newTree.previousSibling_ = lastChild_;

      lastChild_ = newNode;

    } else {
      // No children.
      assert(firstChild_ == entt::null);
      assert(lastChild_ == entt::null);

      firstChild_ = newNode;
      lastChild_ = newNode;
    }
  }

  newTree.parent_ = self;
}

void TreeComponent::appendChild(Registry& registry, Entity child) {
  assert(child != entt::null && "child is null");

  const Entity self = entt::to_entity(registry.storage<TreeComponent>(), *this);
  UTILS_RELEASE_ASSERT(child != self);

  auto& childTree = registry.get<TreeComponent>(child);
  childTree.remove(registry);
  childTree.parent_ = self;

  if (lastChild_ != entt::null) {
    auto& lastChildTree = registry.get<TreeComponent>(lastChild_);
    assert(lastChildTree.nextSibling_ == entt::null);

    childTree.previousSibling_ = lastChild_;
    lastChildTree.nextSibling_ = child;

  } else {
    // No children.
    assert(firstChild_ == entt::null);
    firstChild_ = child;
  }

  lastChild_ = child;
}

void TreeComponent::replaceChild(Registry& registry, Entity newChild, Entity oldChild) {
  assert(newChild != entt::null && "newChild is null");
  assert(oldChild != entt::null && "oldChild is null");

  const Entity self = entt::to_entity(registry.storage<TreeComponent>(), *this);
  UTILS_RELEASE_ASSERT(newChild != self);

  auto& oldChildTree = registry.get<TreeComponent>(oldChild);
  UTILS_RELEASE_ASSERT(oldChildTree.parent_ == self);

  const Entity oldChildNext = oldChildTree.nextSibling_;
  oldChildTree.remove(registry);
  insertBefore(registry, newChild, oldChildNext);
}

void TreeComponent::removeChild(Registry& registry, Entity child) {
  assert(child != entt::null && "child is null");

  auto& childTree = registry.get<TreeComponent>(child);
  const Entity self = entt::to_entity(registry.storage<TreeComponent>(), *this);
  UTILS_RELEASE_ASSERT(childTree.parent_ == self);
  childTree.remove(registry);
}

void TreeComponent::remove(Registry& registry) {
  if (parent_ != entt::null) {
    auto& parentTree = registry.get<TreeComponent>(parent_);
    const Entity self = entt::to_entity(registry.storage<TreeComponent>(), *this);

    // Remove from parent.
    if (parentTree.firstChild_ == self) {
      parentTree.firstChild_ = nextSibling_;
    }
    if (parentTree.lastChild_ == self) {
      parentTree.lastChild_ = previousSibling_;
    }

    // Remove from previous sibling.
    if (previousSibling_ != entt::null) {
      auto& previousTree = registry.get<TreeComponent>(previousSibling_);
      previousTree.nextSibling_ = nextSibling_;
    }

    // Remove from next sibling.
    if (nextSibling_ != entt::null) {
      auto& nextTree = registry.get<TreeComponent>(nextSibling_);
      nextTree.previousSibling_ = previousSibling_;
    }

    // Clear out tree state.
    parent_ = entt::null;
    previousSibling_ = entt::null;
    nextSibling_ = entt::null;
  }
}

}  // namespace donner::svg::components
