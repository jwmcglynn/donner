#pragma once
/// @file
///
/// Helpers for keeping the source pane, command queue, and DOM parse state
/// in sync when editor-owned code writes bytes back into the text buffer.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "donner/editor/EditorApp.h"
#include "donner/editor/SourceEditIntent.h"

namespace donner::editor {

/// Outcome from dispatching a source-pane text change.
struct DispatchSourceTextChangeResult {
  /// True when the change was applied to the live document or queued as an editor command.
  bool dispatchedMutation = false;

  /// True when the change matched the editor's most recent self-initiated
  /// writeback and was intentionally ignored to avoid a feedback loop.
  bool skippedSelfWriteback = false;
};

/// Queue the editor-side sync for source text that was written by the
/// editor itself (drag-end transform writeback, delete-element patch...).
///
/// Self-generated writebacks happen AFTER the DOM has already been
/// mutated by the editor. Re-classifying the text patch back onto that
/// live DOM would be redundant, and worse, it would leave every
/// `XMLNode` source range at its pre-patch offsets. A later source edit
/// or writeback can then map a changed byte offset onto the next
/// sibling. Always queue a preserving reparse instead: the source ranges
/// refresh, undo remains intact, and the compositor's structural-remap
/// path keeps unchanged layer caches usable when the tree shape is
/// stable.
///
/// Updates both source baselines so subsequent user edits diff against
/// the new text, and records the exact bytes so the text-change
/// debouncer recognizes the self-initiated `setText()` echo and
/// suppresses a duplicate dispatch.
///
void QueueSourceWritebackReparse(EditorApp& app, std::string_view newSource,
                                 std::string* previousSourceText,
                                 std::optional<std::string>* lastWritebackSourceText);

/// Route a source-pane text change through XML structured editing or full reparse.
///
/// Self-initiated writebacks recorded via `QueueSourceWritebackReparse()` are
/// filtered out here so the source-pane change signal does not enqueue a
/// second `ReplaceDocumentCommand` for the same bytes.
DispatchSourceTextChangeResult DispatchSourceTextChange(
    EditorApp& app, std::string_view newSource, std::string* previousSourceText,
    std::optional<std::string>* lastWritebackSourceText);

/// Route source-pane edit intents through XML structured editing or full reparse.
DispatchSourceTextChangeResult DispatchSourceEditIntents(
    EditorApp& app, const std::vector<SourceEditIntent>& intents, std::string_view newSource,
    std::string* previousSourceText, std::optional<std::string>* lastWritebackSourceText);

}  // namespace donner::editor
