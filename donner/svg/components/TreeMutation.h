#pragma once
/// @file

#include "donner/base/EcsRegistry.h"

namespace donner::svg::components {

/**
 * Central mutation service for SVG DOM tree edits.
 *
 * \ref TreeMutation is the public SVG layer's single path for inserting,
 * replacing, and detaching SVG element entities. It wraps the low-level
 * \ref donner::components::TreeComponent link operations with SVG-specific
 * validation, dirty-flag propagation, render-tree invalidation, and
 * node-lifetime state updates.
 */
class TreeMutation {
public:
  /**
   * Insert \p newNode as a child of \p parent before \p referenceNode.
   *
   * If \p referenceNode is invalid, \p newNode is appended.
   *
   * @param parent Parent entity to receive the child.
   * @param newNode Entity to insert.
   * @param referenceNode Existing child to insert before, or an invalid handle to append.
   */
  static void InsertBefore(EntityHandle parent, EntityHandle newNode,
                           EntityHandle referenceNode = EntityHandle());

  /**
   * Append \p child as the last child of \p parent.
   *
   * @param parent Parent entity to receive the child.
   * @param child Entity to append.
   */
  static void AppendChild(EntityHandle parent, EntityHandle child);

  /**
   * Replace \p oldChild with \p newChild under \p parent.
   *
   * @param parent Parent entity whose child is replaced.
   * @param newChild Replacement entity.
   * @param oldChild Existing child to detach.
   */
  static void ReplaceChild(EntityHandle parent, EntityHandle newChild, EntityHandle oldChild);

  /**
   * Remove \p child from \p parent.
   *
   * The child entity and its descendants remain alive as a detached subtree.
   *
   * @param parent Parent entity whose child is removed.
   * @param child Existing child to detach.
   */
  static void RemoveChild(EntityHandle parent, EntityHandle child);

  /**
   * Remove \p entity from its parent, if it has one.
   *
   * @param entity Entity to detach from its parent.
   */
  static void Remove(EntityHandle entity);
};

}  // namespace donner::svg::components
