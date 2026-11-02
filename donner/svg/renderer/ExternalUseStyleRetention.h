#pragma once
/// @file
/// Reuse of an external `<use>` target document's derived state across repeated draws.

#include "donner/base/EcsRegistry.h"

namespace donner::svg::components {

/**
 * Keeps a document referenced by external `<use>` elements styled for the occurrences that draw
 * it, instead of restyling the whole document for every occurrence on every frame.
 *
 * An occurrence's inherited style only reaches the subtree of the element it references, so each
 * referenced subtree stays styled for the inputs it was last drawn with: the element, the host's
 * computed style for the `<use>`, and its fill and stroke. A draw with the same inputs reuses the
 * subtree's computed state; changed inputs restyle only that subtree. The state lives in the
 * referenced document's registry context.
 */
class ExternalUseStyleRetention {
public:
  /// Most referenced subtrees kept styled for their occurrence; the least recently drawn beyond
  /// this is restyled for the document itself.
  static constexpr std::size_t kMaximumRetainedSubtrees = 32;

  /**
   * Prepare the referenced document for the occurrence installed in its
   * \ref ExternalUseStyleContext, before its render tree is instantiated. Restyles the referenced
   * subtree only when its retained inputs differ, and schedules an instance rebuild when the render
   * tree was last built for a different occurrence.
   *
   * @param registry Referenced document's registry, with its occurrence context installed.
   */
  static void PrepareOccurrence(Registry& registry);

  /**
   * Restyle every subtree an occurrence styled, so the document renders with its own styles.
   * Called before the document's render tree is instantiated without an occurrence.
   *
   * @param registry Document registry.
   * @return True if any occurrence state was retained, in which case the caller also drops the
   *   occurrence's context paint.
   */
  static bool RestoreStandalone(Registry& registry);
};

}  // namespace donner::svg::components
