#pragma once
/// @file

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/Transform.h"
#include "donner/svg/SVGElement.h"

namespace donner::editor {

/**
 * Captured state of an element at a point in time.
 *
 * In M2 the editor only supports transform-level snapshots — `PathSpline`
 * and element create/delete snapshots return when path tools land. See
 * docs/design_docs/editor.md "Non-Goals" for the scoping decision.
 */
struct UndoSnapshot {
  /// The element this snapshot applies to.
  svg::SVGElement element;

  /// The captured transform.
  Transform2d transform;
};

/// Capture the current transform of an SVGElement as an undo snapshot.
UndoSnapshot captureTransformSnapshot(const svg::SVGElement& element);

/// Apply a previously captured snapshot back to its element.
void applySnapshot(const UndoSnapshot& snapshot);

/// A single entry in the non-destructive undo timeline.
struct UndoEntry {
  /// Human-readable label (e.g. "Move element").
  std::string label;

  /// Element state before the operation.
  UndoSnapshot before;

  /// Element state after the operation.
  UndoSnapshot after;

  /// If this entry undoes another entry, the index of that entry.
  std::optional<size_t> undoOf;
};

/**
 * Non-destructive chronological undo timeline.
 *
 * Uses an Emacs-style undo model: consecutive undos walk backward through the entry list,
 * reversing each entry. Any non-undo action breaks the chain. After a break, subsequent undos
 * traverse the entire list (including previous undo entries), allowing undo-of-undo to re-apply
 * a previously reversed action.
 *
 * No entries are ever removed. The full editing history is preserved indefinitely.
 */
class UndoTimeline {
public:
  /// Begin a drag transaction. The before snapshot is captured now; the after snapshot is captured
  /// on commit. Nested calls are ignored (outermost transaction wins).
  void beginTransaction(std::string_view label, UndoSnapshot before);

  /// Commit the current transaction with the given after snapshot.
  void commitTransaction(UndoSnapshot after);

  /// Discard the current transaction without recording an entry.
  void abortTransaction();

  /// Whether a transaction is currently open.
  [[nodiscard]] bool inTransaction() const { return pendingTransaction_.has_value(); }

  /// Record a complete single-step action (breaks any active undo chain).
  void record(std::string_view label, UndoSnapshot before, UndoSnapshot after);

  /// Undo the next entry in the current undo chain. If no chain is active, starts a new chain
  /// from the end of the timeline. Returns the snapshot that was applied, or nullopt if
  /// there is nothing to undo.
  std::optional<UndoSnapshot> undo();

  /// Whether there is an entry to undo (either in the current chain or by starting a new one).
  [[nodiscard]] bool canUndo() const;

  /// The label of the entry that would be undone next.
  [[nodiscard]] std::optional<std::string_view> nextUndoLabel() const;

  /// Number of entries in the timeline.
  [[nodiscard]] size_t entryCount() const { return entries_.size(); }

  /// Break the current undo chain (called on any non-undo user action).
  void breakUndoChain();

  /// Clear all history.
  void clear();

private:
  static constexpr size_t kCursorExhausted = static_cast<size_t>(-1);

  std::vector<UndoEntry> entries_;

  struct PendingTransaction {
    std::string label;
    UndoSnapshot before;
  };
  std::optional<PendingTransaction> pendingTransaction_;

  /// Undo chain state.
  bool inUndoChain_ = false;
  size_t undoCursor_ = 0;  ///< Entry index being reversed during the chain.
  size_t chainStart_ = 0;  ///< entries_.size() when the chain started.
};

}  // namespace donner::editor
