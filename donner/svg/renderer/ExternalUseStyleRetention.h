#pragma once
/// @file
/// Reuse of an external `<use>` target document's derived state across repeated draws.

#include <cstddef>
#include <cstdint>
#include <optional>

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
 *
 * Only occurrences whose fill and stroke are none or a solid color are retained; one painted with
 * a gradient or pattern reference is restyled for every draw and restored afterwards, so no state
 * outliving a draw refers to the host document. Public reads of the document's own styles and
 * rendering restore it with \ref RestoreStandalone first.
 */
class ExternalUseStyleRetention {
public:
  /// Most referenced subtrees kept styled for their occurrence; the least recently drawn beyond
  /// this is restyled for the document itself.
  static constexpr std::size_t kMaximumRetainedSubtrees = 32;

  /**
   * Prepare the referenced document's styles for the occurrence installed in its
   * \ref ExternalUseStyleContext. Restyles the referenced subtree only when its retained inputs
   * differ.
   *
   * @param registry Referenced document's registry, with its occurrence context installed.
   */
  static void PrepareOccurrence(Registry& registry);

  /**
   * Once the occurrence's instance layout is in place, schedule an instance rebuild when the render
   * tree was last built for a different occurrence: another root, paint, or viewport the `<use>`
   * establishes for a `<symbol>` or `<svg>`. A full rebuild is scheduled instead when the subtree
   * holds a `<use>` whose shadow tree a rebuild for another root left uninstantiated.
   *
   * @param registry Referenced document's registry, with its occurrence context installed.
   */
  static void PrepareInstance(Registry& registry);

  /**
   * After the render tree is instantiated for the installed occurrence, restore every other
   * retained subtree to the document's own styles if drawing the occurrence may read any element
   * outside its own subtree: a paint server, clip path, mask, marker, filter, instantiated element,
   * inherited paint server, or a feImage, textPath or mpath target. What such an element pulls in
   * can reach any other subtree, so a self-contained subtree is the only one drawn next to
   * retained subtrees.
   *
   * @param registry Referenced document's registry, with its occurrence context installed.
   * @return True if a subtree was restored, in which case the caller instantiates the render tree
   *   again before drawing.
   */
  static bool RestoreOtherSubtreesIfReadingOutside(Registry& registry);

  /**
   * End the installed occurrence's draw, before its context is removed. Restores the occurrence's
   * subtree and drops its rendering instances when it must not be retained: its paint refers to
   * the host, or its restyle hit a style resource limit.
   *
   * @param registry Referenced document's registry, with its occurrence context installed.
   */
  static void FinishOccurrence(Registry& registry);

  /**
   * Restyle every subtree an occurrence styled and drop the occurrence's rendering instances and
   * context paint, so the document renders and reads with its own styles.
   *
   * Render-tree instantiation calls this before preparing a document without an occurrence.
   *
   * @param registry Document registry.
   */
  static void RestoreStandalone(Registry& registry);

  /**
   * Before a public read of computed or derived state (styles, stops, text metrics, layout
   * transforms, shape geometry and bounds, rendering instances): if an occurrence styled the
   * document, restore it with \ref RestoreStandalone and prepare its render tree again, since
   * restoring drops layout state that reads expect the preparation to have computed. Call under
   * write access. A read-only accessor that cannot restore reports no value for occurrence-derived
   * state instead.
   *
   * @param registry Document registry.
   */
  static void RestoreStandaloneForRead(Registry& registry);

  /**
   * Whether \p entity's computed style currently reflects an occurrence rather than its document.
   *
   * @param registry Document registry.
   * @param entity Element to test.
   */
  static bool IsOccurrenceStyled(const Registry& registry, Entity entity);

  /**
   * Identifies the style the installed occurrence's subtree currently has, while that subtree is
   * retained for the occurrence's inputs.
   *
   * Every snapshot of occurrence inputs gets an epoch no other snapshot in the process has had, and
   * a subtree keeps its epoch only while it stays styled for those inputs: any restyle that reaches
   * it (changed inputs, a style-dirty element, eviction, or a restore) ends the epoch. Equal epochs
   * on two draws therefore mean the subtree's computed styles were not recomputed in between.
   * Changes that do not restyle, such as a new path `d`, are not covered; callers combine the epoch
   * with the document's mutation revision.
   *
   * @param registry Referenced document's registry, with its occurrence context installed.
   * @return The epoch, or `std::nullopt` when the occurrence is not retained (its paint refers to
   *   the host, or its inputs were not snapshotted).
   */
  static std::optional<std::uint64_t> RetainedStyleEpoch(const Registry& registry);

  /**
   * Whether any occurrence state is retained, so the document's rendering instances may reflect an
   * occurrence.
   *
   * @param registry Document registry.
   */
  static bool HasOccurrenceState(const Registry& registry);
};

}  // namespace donner::svg::components
