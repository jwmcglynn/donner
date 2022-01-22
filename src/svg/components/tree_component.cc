#include "src/svg/components/tree_component.h"

#include "src/base/utils.h"

namespace donner {

void TreeComponent::insertBefore(TreeComponent* newNode, TreeComponent* referenceNode) {
  assert(newNode && "newNode is null");
  // referenceNode may be null.

  newNode->remove();

  if (referenceNode) {
    UTILS_RELEASE_ASSERT(referenceNode->parent_ == this);

    newNode->previousSibling_ = referenceNode->previousSibling_;
    newNode->nextSibling_ = referenceNode;
    referenceNode->previousSibling_ = newNode;

    // If there is no previousSibling_, this was the first node.
    if (newNode->previousSibling_ == nullptr) {
      firstChild_ = newNode;
    } else {
      newNode->previousSibling_->nextSibling_ = newNode;
    }

  } else {
    // If no referenceNode is provided, insert at the end.
    if (lastChild_) {
      assert(lastChild_->nextSibling_ == nullptr);
      lastChild_->nextSibling_ = newNode;
      newNode->previousSibling_ = lastChild_;

      lastChild_ = newNode;

    } else {
      // No children.
      assert(firstChild_ == nullptr);
      assert(lastChild_ == nullptr);

      firstChild_ = newNode;
      lastChild_ = newNode;
    }
  }

  newNode->parent_ = this;
}

void TreeComponent::appendChild(TreeComponent* child) {
  assert(child && "child is null");
  UTILS_RELEASE_ASSERT(child != this);

  child->remove();
  child->parent_ = this;

  if (lastChild_) {
    assert(lastChild_->nextSibling_ == nullptr);

    child->previousSibling_ = lastChild_;
    lastChild_->nextSibling_ = child;

  } else {
    // No children.
    assert(firstChild_ == nullptr);
    firstChild_ = child;
  }

  lastChild_ = child;
}

void TreeComponent::replaceChild(TreeComponent* newChild, TreeComponent* oldChild) {
  assert(newChild && "newChild is null");
  assert(oldChild && "oldChild is null");
  UTILS_RELEASE_ASSERT(newChild != this);
  UTILS_RELEASE_ASSERT(oldChild->parent_ == this);

  TreeComponent* oldChildNext = oldChild->nextSibling_;
  oldChild->remove();
  insertBefore(newChild, oldChildNext);
}

void TreeComponent::removeChild(TreeComponent* child) {
  assert(child && "child is null");
  UTILS_RELEASE_ASSERT(child->parent_ == this);
  child->remove();
}

void TreeComponent::remove() {
  if (parent_) {
    // Remove from parent.
    if (parent_->firstChild_ == this) {
      parent_->firstChild_ = nextSibling_;
    }
    if (parent_->lastChild_ == this) {
      parent_->lastChild_ = previousSibling_;
    }

    // Remove from previous sibling.
    if (previousSibling_) {
      previousSibling_->nextSibling_ = nextSibling_;
    }

    // Remove from next sibling.
    if (nextSibling_) {
      nextSibling_->previousSibling_ = previousSibling_;
    }

    // Clear out tree state.
    parent_ = nullptr;
    previousSibling_ = nullptr;
    nextSibling_ = nullptr;
  }
}

}  // namespace donner
