#include "donner/editor/backend_lib/AsyncSVGDocument.h"

#include "donner/base/ParseWarningSink.h"
#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/compositor/CompositorController.h"
#include "donner/svg/parser/SVGParser.h"

namespace donner::editor {

AsyncSVGDocument::AsyncSVGDocument() = default;

void AsyncSVGDocument::setDocument(svg::SVGDocument document) {
  document_ = std::move(document);
  queue_.clear();
  lastParseError_.reset();
  pendingStructuralRemap_.clear();
  frameVersion_.fetch_add(1, std::memory_order_release);
  documentGeneration_.fetch_add(1, std::memory_order_release);
}

AsyncSVGDocument::ReplaceKind AsyncSVGDocument::setDocumentMaybeStructural(
    svg::SVGDocument newDocument) {
  // If there's no current document, a structural remap isn't meaningful —
  // just install the new one like a regular `setDocument`.
  if (!document_.has_value()) {
    setDocument(std::move(newDocument));
    return ReplaceKind::FullReplace;
  }

  // Build the remap BEFORE the swap. The walker needs both documents'
  // XML trees live; once we move `newDocument` into `document_`, the
  // old document is gone. Note this runs on the UI thread with no
  // locking — consistent with the existing `setDocument` contract that
  // only the UI thread touches the document outside a render.
  auto remap = svg::compositor::BuildStructuralEntityRemap(*document_, newDocument);
  const bool structural = !remap.empty();

  // Swap the document. We can't call `setDocument` directly because it
  // clears `pendingStructuralRemap_` — we want to populate it right
  // after.
  document_ = std::move(newDocument);
  queue_.clear();
  lastParseError_.reset();
  frameVersion_.fetch_add(1, std::memory_order_release);
  documentGeneration_.fetch_add(1, std::memory_order_release);

  if (structural) {
    pendingStructuralRemap_ = std::move(remap);
    return ReplaceKind::Structural;
  }
  pendingStructuralRemap_.clear();
  return ReplaceKind::FullReplace;
}

bool AsyncSVGDocument::flushFrame() {
  lastFlushResult_ = {};
  if (queue_.empty()) {
    return false;
  }

  const auto queueFlush = queue_.flush();
  if (queueFlush.effectiveCommands.empty()) {
    return false;
  }

  for (const auto& cmd : queueFlush.effectiveCommands) {
    applyOne(cmd);
  }

  lastFlushResult_ = FlushResult{
      .appliedCommands = true,
      .replacedDocument = queueFlush.hadReplaceDocument,
      .preserveUndoOnReparse = queueFlush.preserveUndoOnReparse,
  };
  frameVersion_.fetch_add(1, std::memory_order_release);
  return true;
}

bool AsyncSVGDocument::loadFromString(std::string_view svgBytes) {
  ParseWarningSink sink;
  auto result = svg::parser::SVGParser::ParseSVG(svgBytes, sink);
  if (result.hasError()) {
    // Stash the diagnostic so the source pane can show a line marker.
    // Leave the existing document in place — the user can keep editing
    // until the source parses again. We deliberately do NOT bump the
    // frame version: the renderer's view of the document is unchanged,
    // and the source pane polls `lastParseError()` directly.
    lastParseError_ = std::move(result.error());
    return false;
  }
  setDocument(std::move(result.result()));
  return true;
}

void AsyncSVGDocument::applyOne(const EditorCommand& command) {
  switch (command.kind) {
    case EditorCommand::Kind::SetTransform: {
      if (!command.element.has_value()) {
        return;
      }
      // The element handle was captured by a public API (hitTest, tree
      // traversal, or querySelector) so we just cast to the graphics-
      // element interface and call the public transform setter — no
      // direct ECS access.
      auto graphics = command.element->cast<svg::SVGGraphicsElement>();
      graphics.setTransform(command.transform);
      break;
    }

    case EditorCommand::Kind::ReplaceDocument: {
      // ReplaceDocument bypasses applyOne's mid-flush context: it always
      // produces a fresh document via the parser, so we re-enter the
      // setDocument path which clears the queue (already drained) and bumps
      // the version (which `flushFrame` will bump again, harmlessly).
      //
      // `preserveUndoOnReparse` commands are self-generated writebacks —
      // drag-end transforms, delete-element text patches — that mutate
      // only attribute values or delete whole subtrees. In the common
      // drag-end case the tree shape is identical, so try the structural
      // remap path: on success the worker's next tick will preserve the
      // compositor's cached layer bitmaps + segments instead of paying
      // the full-reset cost. On failure (tree shape changed, e.g. delete-
      // element), the structural check returns an empty remap and the
      // replacement falls back to the standard `FullReplace` semantics.
      //
      // Non-writeback `ReplaceDocument`s (file open, user text edit that
      // made the whole thing unparseable until now) aren't tagged
      // `preserveUndoOnReparse`, and go through the straight
      // `loadFromString` path — they genuinely replace the entity space.
      if (command.preserveUndoOnReparse) {
        ParseWarningSink sink;
        auto result = svg::parser::SVGParser::ParseSVG(command.bytes, sink);
        if (result.hasError()) {
          lastParseError_ = std::move(result.error());
          return;
        }
        (void)setDocumentMaybeStructural(std::move(result).result());
      } else {
        (void)loadFromString(command.bytes);
      }
      break;
    }

    case EditorCommand::Kind::SetAttribute: {
      if (!command.element.has_value()) {
        return;
      }
      // Apply the attribute change via the public SVGElement::setAttribute
      // API — the same path the parser uses. This triggers presentation-
      // attribute parsing, style cascade, and dirty-flag marking through
      // the same code path as initial parse. The structured-editing M5
      // fast path will eventually replace this with a targeted
      // AttributeParser::ParseAndSetAttribute call, but for M3 the
      // public setAttribute is correct and sufficient.
      svg::SVGElement element = *command.element;
      element.setAttribute(xml::XMLQualifiedNameRef(command.attributeName), command.attributeValue);
      break;
    }

    case EditorCommand::Kind::DeleteElement: {
      if (!command.element.has_value()) {
        return;
      }
      // Detach via the public `SVGElement::remove()` method. The ECS
      // entity stays in the registry (orphaned, not garbage-collected)
      // so any in-flight references — stale selection, undo snapshots —
      // remain valid but invisible to the renderer because the tree
      // walk stops at the detach point. We copy to a local because
      // `remove()` is non-const and `command` is `const&`.
      svg::SVGElement element = *command.element;
      element.remove();
      break;
    }
  }
}

}  // namespace donner::editor
