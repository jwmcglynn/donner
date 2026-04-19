#pragma once
/// @file
///
/// Helpers for keeping the source pane, command queue, and DOM parse state
/// in sync when editor-owned code writes bytes back into the text buffer.

#include <optional>
#include <string>
#include <string_view>

#include "donner/editor/EditorApp.h"

namespace donner::editor {

/// Outcome from dispatching a source-pane text change.
struct DispatchSourceTextChangeResult {
  /// True when a command was enqueued into `EditorApp`.
  bool dispatchedMutation = false;

  /// True when the change matched the editor's most recent self-initiated
  /// writeback and was intentionally ignored to avoid a feedback loop.
  bool skippedSelfWriteback = false;
};

/// Queue the editor-side sync for source text that was written by the
/// editor itself (drag-end transform writeback, delete-element patch…).
///
/// If structured editing is enabled and the change fits the incremental
/// classifier (single attribute-value edit on a single element — the
/// common case for drag-end transform writebacks), emits a
/// `SetAttributeCommand` that updates the attribute on the live DOM
/// element without destroying the entity space. Falls back to a full
/// `ReplaceDocumentCommand` reparse when the change can't be classified
/// (structural edits, multi-element writebacks).
///
/// Important asymmetry with `DispatchSourceTextChange`: self-generated
/// writebacks fire AFTER the DOM has already been mutated incrementally
/// during the drag. So in many cases the classifier-produced
/// `SetAttributeCommand` is actually redundant (the DOM is already at
/// the target state) — but it's still worth emitting because
/// `SetAttributeCommand`'s apply path is idempotent and the mutation
/// flush bumps `frameVersion`, keeping the render pipeline's version
/// bookkeeping straight. What we CANNOT afford is falling through to
/// `ReplaceDocumentCommand` on every drag release: that tears down the
/// entity space (free + alloc a new `Registry` heap block), which the
/// compositor observes as a structural replacement and spends multiple
/// seconds rebuilding every cached layer bitmap + segment.
///
/// Updates both source baselines so subsequent user edits diff against
/// the new text, and records the exact bytes so the text-change
/// debouncer recognizes the self-initiated `setText()` echo and
/// suppresses a duplicate dispatch.
///
/// @param previousSourcePrePatch Source text BEFORE the patch the
///   caller just applied — used as the "old" side of the classifier's
///   diff. Pass the string `previousSourceText_` held before
///   `applyPatches` updated `source`.
void QueueSourceWritebackReparse(EditorApp& app, std::string_view newSource,
                                 std::string_view previousSourcePrePatch,
                                 std::string* previousSourceText,
                                 std::optional<std::string>* lastWritebackSourceText);

/// Route a source-pane text change through structured editing or full reparse.
///
/// Self-initiated writebacks recorded via `QueueSourceWritebackReparse()` are
/// filtered out here so the source-pane change signal does not enqueue a
/// second `ReplaceDocumentCommand` for the same bytes.
DispatchSourceTextChangeResult DispatchSourceTextChange(
    EditorApp& app, std::string_view newSource, std::string* previousSourceText,
    std::optional<std::string>* lastWritebackSourceText);

}  // namespace donner::editor
