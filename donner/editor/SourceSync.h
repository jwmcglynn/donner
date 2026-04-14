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

/// Queue a reparse for source text that was written by the editor itself.
///
/// This updates both source baselines so subsequent user edits diff against
/// the new text, and it records the exact bytes so the text-change debouncer
/// can recognize the self-initiated `setText()` echo and suppress a duplicate
/// `ReplaceDocumentCommand`.
void QueueSourceWritebackReparse(EditorApp& app, std::string_view newSource,
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
