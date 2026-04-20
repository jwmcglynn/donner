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
#include <unordered_map>

#include "donner/base/EcsRegistry.h"

#include "donner/base/ParseDiagnostic.h"
#include "donner/editor/backend_lib/CommandQueue.h"
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

  /// Outcome of `setDocumentMaybeStructural`. `FullReplace` means the new
  /// doc differs structurally from the current one (or there was no
  /// current one) and every consumer of the old entity space must treat
  /// their state as invalid — identical to `setDocument`'s contract.
  /// `Structural` means the new doc has the same XML shape and element
  /// ids, and the entity remap carried via the next `RenderRequest`
  /// lets downstream consumers (the compositor) preserve their caches.
  enum class ReplaceKind : uint8_t { FullReplace, Structural };

  /// Like `setDocument`, but builds a structural entity remap against
  /// the current document first. If the remap is non-empty (trees match
  /// by tag name + id at every step), the replacement is tagged as
  /// `Structural` — the next `RenderRequest` carries the remap so the
  /// compositor can call `remapAfterStructuralReplace` instead of
  /// `resetAllLayers(documentReplaced=true)`, preserving cached layer
  /// bitmaps and segments across the swap. If the trees differ (user
  /// edited the source pane to change shape, etc.) the remap is empty
  /// and we fall back to the standard `setDocument` path.
  ReplaceKind setDocumentMaybeStructural(svg::SVGDocument newDocument);

  /// Take ownership of the pending structural remap produced by the
  /// most recent `setDocumentMaybeStructural(Structural)` call. Returns
  /// an empty map if the last replacement was a `FullReplace` or nothing
  /// has been replaced since the last consumption. Called by
  /// `RenderCoordinator` when assembling the next `RenderRequest`.
  std::unordered_map<Entity, Entity> consumePendingStructuralRemap() {
    return std::move(pendingStructuralRemap_);
  }

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

  /// Generation counter bumped only when `setDocument` replaces the inner
  /// SVGDocument (e.g. `ReplaceDocumentCommand` on source-pane edits). The
  /// inner document's storage address is stable across a replacement (the
  /// optional lives inside this object), so pointer-identity is NOT a
  /// reliable "is this the same document" check — consumers that cache
  /// per-document state (like the compositor's `activeHints_` backing an
  /// entity space) need this counter to know when to tear that state down.
  ///
  /// Distinct from `frameVersion_`: the frame counter bumps on every
  /// mutation (including every drag frame's `SetTransformCommand`), which
  /// would be catastrophic to treat as a document replacement.
  [[nodiscard]] std::uint64_t documentGeneration() const {
    return documentGeneration_.load(std::memory_order_acquire);
  }

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
  std::atomic<std::uint64_t> documentGeneration_{0};

  /// Remap from the previous document's entity ids to the current
  /// document's entity ids, populated by `setDocumentMaybeStructural`
  /// when the replacement was structurally equivalent. Consumed and
  /// cleared by `consumePendingStructuralRemap`. Empty when the most
  /// recent replacement was a `FullReplace` or there has been no
  /// replacement since the last consumption.
  std::unordered_map<Entity, Entity> pendingStructuralRemap_;
  std::optional<ParseDiagnostic> lastParseError_;
  FlushResult lastFlushResult_;
};

}  // namespace donner::editor
