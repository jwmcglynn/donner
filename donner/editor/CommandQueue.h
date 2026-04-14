#pragma once
/// @file
///
/// `CommandQueue` is the per-frame `EditorCommand` queue described in the
/// "AsyncSVGDocument: single-threaded command queue" section of
/// `docs/design_docs/editor.md`. It accumulates editor-initiated DOM
/// mutations on the UI thread and coalesces them at flush time.
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
/// 3. No reordering across commands targeting different entities.
///    Coalescing only collapses redundant writes.
///
/// The queue is **single-threaded** — it must only be touched from the UI
/// thread. The render thread reads document state via the snapshot hand-off
/// in `AsyncSVGDocument`, never via the queue directly.

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
