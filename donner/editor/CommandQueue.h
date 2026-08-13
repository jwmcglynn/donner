#pragma once
/// @file
///
/// `CommandQueue` is the per-frame queued-command seam described in
/// \ref EditorArchitecture. It accumulates canvas, tool, application, and
/// full-document-replacement commands on the UI thread and coalesces them at
/// flush time. Incremental source-pane edits use
/// `AsyncSVGDocument::applySourceEdit()` instead of this queue.
///
/// Coalescing rules (applied in `flush()`):
///
/// 1. `ReplaceDocument` is exclusive: it drops every command queued before
///    it (their entity references would be invalidated by the re-parse).
///    Commands queued *after* a `ReplaceDocument` apply against the new
///    document.
/// 2. `SetTransform` collapses by entity: multiple `SetTransform` commands
///    targeting the same `Entity` flush as a single command carrying the
///    most recent transform. A drag that produces 60 mouse-move
///    `SetTransform` commands per second flushes as a single
///    `setTransform()` call.
/// 3. `InsertElement` and `DeleteElement` are structural and are never
///    coalesced.
/// 4. No reordering across commands targeting different entities.
///    Coalescing only collapses redundant writes.
///
/// The queue is **UI-thread-only**. The render thread reads the shared live
/// document under ConcurrentDom access guards, never through this queue.

#include <deque>
#include <vector>

#include "donner/editor/EditorCommand.h"

namespace donner::editor {

class CommandQueue {
public:
  struct FlushResult {
    std::vector<EditorCommand> effectiveCommands;

    /// True when any ReplaceDocument was drained from the raw pending batch.
    bool hadReplaceDocument = false;

    /// True only when the drained batch contained at least one
    /// ReplaceDocument and every drained ReplaceDocument carried the
    /// preserve-undo marker.
    bool preserveUndoOnReparse = false;
  };

  /// Push a command onto the queue. UI thread only.
  void push(EditorCommand command) { pending_.push_back(std::move(command)); }

  /// Drain and coalesce the pending commands. Returns the effective set of
  /// commands to apply, in the order the application should issue them.
  /// After `flush()` returns, the queue is empty.
  ///
  /// `flush()` is called once per frame at the start of the main loop.
  [[nodiscard]] FlushResult flush();

  /// Whether the queue currently holds any pending commands. Useful for
  /// frame-skip optimizations (no need to re-render if nothing changed).
  [[nodiscard]] bool empty() const { return pending_.empty(); }

  /// Number of un-coalesced commands currently pending. Coalescing happens
  /// at `flush()` time, so this is the *raw* count, not the effective count.
  [[nodiscard]] std::size_t size() const { return pending_.size(); }

  /// Drop everything pending without applying. Useful for tests and for the
  /// "abort drag" cancel path.
  void clear() { pending_.clear(); }

private:
  std::deque<EditorCommand> pending_;
};

}  // namespace donner::editor
