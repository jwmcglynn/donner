#pragma once
/// @file
///
/// `AsyncSVGDocument` is the editor-owned wrapper around `svg::SVGDocument`
/// that gates DOM mutations through the `CommandQueue` and provides the
/// snapshot hand-off to the render thread described in the M1.5 design note
/// in `docs/design_docs/editor.md`.
///
/// The current M2 implementation is **single-threaded**: there is no real
/// render thread yet, so the snapshot hand-off is just a pointer to the
/// live document. The frame version counter is in place so the render
/// thread coordination can be wired up incrementally without changing the
/// public surface.

#include <atomic>
#include <cstdint>
#include <optional>
#include <string_view>

#include "donner/base/ParseDiagnostic.h"
#include "donner/editor/CommandQueue.h"
#include "donner/svg/SVGDocument.h"

namespace donner::editor {

/// Wraps an `svg::SVGDocument` and the editor's per-frame command queue.
///
/// Lifetime: owned by `EditorApp` (one per editor session). All public
/// methods must be called from the UI thread; the only thread-safe
/// operation is `currentFrameVersion()` which the render thread can poll.
class AsyncSVGDocument {
public:
  struct FlushResult {
    bool appliedCommands = false;
    bool replacedDocument = false;
    bool preserveUndoOnReparse = false;
  };

  AsyncSVGDocument();
  ~AsyncSVGDocument() = default;

  AsyncSVGDocument(const AsyncSVGDocument&) = delete;
  AsyncSVGDocument& operator=(const AsyncSVGDocument&) = delete;
  AsyncSVGDocument(AsyncSVGDocument&&) = delete;
  AsyncSVGDocument& operator=(AsyncSVGDocument&&) = delete;

  /// Replace the inner document. Clears any pending commands — they would
  /// reference now-invalid entities. Bumps the frame version.
  void setDocument(svg::SVGDocument document);

  /// Whether a document has been loaded yet.
  [[nodiscard]] bool hasDocument() const { return document_.has_value(); }

  /// Direct access to the inner document. UI thread only. The render thread
  /// should hold the snapshot returned by `acquireRenderSnapshot()` instead.
  [[nodiscard]] svg::SVGDocument& document() { return *document_; }
  [[nodiscard]] const svg::SVGDocument& document() const { return *document_; }

  /// Push a command onto the per-frame queue. UI thread only.
  void applyMutation(EditorCommand command) { queue_.push(std::move(command)); }

  /// Direct access to the command queue. Tools push via `applyMutation`;
  /// this accessor exists for tests and for the main loop's `flushFrame`.
  [[nodiscard]] CommandQueue& queue() { return queue_; }

  /// Drain and apply any pending commands. Called once per frame at the
  /// start of the main loop. Returns true if any commands were applied
  /// (so the caller can decide whether to re-render).
  ///
  /// Bumps the frame version when commands are applied OR when the document
  /// itself was replaced via `setDocument` since the last flush.
  bool flushFrame();

  /// Metadata from the most recent `flushFrame()` call.
  [[nodiscard]] const FlushResult& lastFlushResult() const { return lastFlushResult_; }

  /// Monotonic frame version counter. Bumped on every state change visible
  /// to the renderer (mutation flush or document replacement). The render
  /// thread polls this to detect updates without locking the document.
  [[nodiscard]] std::uint64_t currentFrameVersion() const {
    return frameVersion_.load(std::memory_order_acquire);
  }

  // Test hook: re-parse a string into a fresh document via `SVGParser`.
  // Returns true on success. On failure, the existing document is left
  // intact and `lastParseError()` returns the diagnostic from the parser
  // (so the caller can surface a line + reason in a text editor).
  [[nodiscard]] bool loadFromString(std::string_view svgBytes);

  /// The diagnostic from the most recent failed `loadFromString` /
  /// `ReplaceDocumentCommand`. Cleared on every successful parse — so
  /// `has_value()` is the live "is the source pane currently invalid?"
  /// signal.
  [[nodiscard]] const std::optional<ParseDiagnostic>& lastParseError() const {
    return lastParseError_;
  }

private:
  // Apply a single (already-coalesced) command. SetTransform finds the
  // target element via the document's Registry and calls
  // LayoutSystem::setRawEntityFromParentTransform; ReplaceDocument
  // re-parses the bytes into a fresh SVGDocument and replaces `document_`.
  void applyOne(const EditorCommand& command);

  std::optional<svg::SVGDocument> document_;
  CommandQueue queue_;
  std::atomic<std::uint64_t> frameVersion_{0};
  std::optional<ParseDiagnostic> lastParseError_;
  FlushResult lastFlushResult_;
};

}  // namespace donner::editor
