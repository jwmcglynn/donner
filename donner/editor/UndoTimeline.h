#pragma once
/// @file

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/Transform.h"
#include "donner/editor/AttributeWriteback.h"
#include "donner/svg/SVGElement.h"

namespace donner::editor {

/**
 * Captured state of an element at a point in time.
 */
struct UndoSnapshot {
  /// The kind of state captured by this snapshot.
  enum class Kind : std::uint8_t {
    /// A graphics element's local transform.
    Transform,

    /// The full SVG source text for structural edits such as element deletion.
    DocumentSource,
  };

  /// Snapshot payload kind.
  Kind kind = Kind::Transform;

  /// The element this snapshot applies to.
  svg::SVGElement element;

  /// The captured transform.
  Transform2d transform;

  /// Stable locator for the element, used to resolve a live handle after a
  /// self-initiated ReplaceDocument swaps the document identity.
  std::optional<AttributeWritebackTarget> writebackTarget;

  /// Exact `transform=` attribute bytes from the source pane, when undo
  /// should restore the user's original text verbatim instead of the
  /// canonical serializer output. `std::nullopt` means the attribute was
  /// absent in the captured source.
  std::optional<RcString> sourceTransformAttributeValue;

  /// Whether `sourceTransformAttributeValue` should be restored verbatim.
  /// When false, source writeback falls back to canonical serialization of
  /// `transform`.
  bool restoreSourceTransformAttributeValue = false;

  /// Full SVG source text for `Kind::DocumentSource` snapshots.
  std::string documentSource;

  /// Selection targets to restore after a `Kind::DocumentSource` snapshot reparses the document.
  std::vector<AttributeWritebackTarget> selectionTargets;

  /// Additional element snapshots that belong to the same user operation.
  /// Multi-selection move/resize/rotate gestures use this so one UI undo
  /// restores the whole manipulation instead of stepping through elements
  /// one at a time.
  std::vector<UndoSnapshot> extras;
};

/// Capture the current transform of an SVGElement as an undo snapshot.
UndoSnapshot captureTransformSnapshot(const svg::SVGElement& element);

/// Capture the full SVG source text for a structural undo snapshot.
UndoSnapshot captureDocumentSourceSnapshot(const svg::SVGElement& anchorElement,
                                           std::string_view source);

/// Apply a previously captured snapshot, including grouped extra element snapshots.
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
  /// from the end of the timeline. Returns the snapshot the caller should apply, or nullopt if
  /// there is nothing to undo.
  std::optional<UndoSnapshot> undo();

  /// Redo the most recently undone entry. Returns the snapshot the caller
  /// should apply, or nullopt if there is nothing to redo.
  std::optional<UndoSnapshot> redo();

  /// Whether there is an entry to undo (either in the current chain or by starting a new one).
  [[nodiscard]] bool canUndo() const;

  /// Whether the most recent undo operation can be redone.
  [[nodiscard]] bool canRedo() const;

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
  std::vector<size_t> redoStack_;
};

}  // namespace donner::editor
