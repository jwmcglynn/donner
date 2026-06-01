#include "donner/editor/AsyncSVGDocument.h"

#include "donner/base/ParseWarningSink.h"
#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/SVGStyleElement.h"
#include "donner/svg/SVGTextContentElement.h"
#include "donner/svg/compositor/CompositorController.h"
#include "donner/svg/parser/SVGParser.h"

namespace donner::editor {

namespace {

/// Parse options for editor documents. The editor round-trips user content, so
/// the parser must PRESERVE user / `data-*` attributes in the DOM rather than
/// dropping them — editor features read them back via `getAttribute` (e.g.
/// `IsLocked` reads `data-donner-locked`), and a save must not silently lose the
/// user's own attributes. The library default `disableUserAttributes = true` is
/// correct for a strict renderer, not for an authoring tool.
svg::parser::SVGParser::Options EditorParseOptions() {
  svg::parser::SVGParser::Options options;
  options.disableUserAttributes = false;
  return options;
}

}  // namespace

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
  const Vector2i preservedCanvasSize = document_->canvasSize();

  // Build the remap BEFORE the swap. The walker needs both documents'
  // XML trees live; once we move `newDocument` into `document_`, the
  // old document is gone. Note this runs on the UI thread with no
  // locking — consistent with the existing `setDocument` contract that
  // only the UI thread touches the document outside a render.
  auto remap = svg::compositor::BuildStructuralEntityRemap(*document_, newDocument);
  const bool structural = !remap.empty();
  if (structural) {
    // The editor owns the raster canvas size independently from the SVG
    // source. A structural source writeback reparses into a fresh
    // document whose canvas would otherwise fall back to intrinsic SVG
    // dimensions, forcing `RenderCoordinator` to call setCanvasSize on
    // the next frame. That late resize invalidates the whole render tree
    // and dirties every preserved compositor layer. Carry the canvas
    // size into the new document before the worker consumes the remap so
    // it prepares the replacement at the already-displayed size.
    newDocument.setCanvasSize(preservedCanvasSize.x, preservedCanvasSize.y);
  }

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

  bool removedElements = false;
  for (const EditorCommand& command : queueFlush.effectiveCommands) {
    if (command.kind == EditorCommand::Kind::DeleteElement) {
      removedElements = true;
      break;
    }
  }

  lastFlushResult_ = FlushResult{
      .appliedCommands = true,
      .replacedDocument = queueFlush.hadReplaceDocument,
      .preserveUndoOnReparse = queueFlush.preserveUndoOnReparse,
      .removedElements = removedElements,
  };

  for (const auto& cmd : queueFlush.effectiveCommands) {
    applyOne(cmd);
  }

  frameVersion_.fetch_add(1, std::memory_order_release);
  return true;
}

xml::ApplySourceEditResult AsyncSVGDocument::applySourceEdit(const xml::XMLEditIntent& intent) {
  if (!document_.has_value()) {
    xml::ApplySourceEditResult result;
    result.diagnostic =
        ParseDiagnostic::Error("Cannot apply source edit without a loaded document", intent.range);
    lastParseError_ = result.diagnostic;
    return result;
  }

  xml::ApplySourceEditResult result = document_->applySourceEdit(intent);
  if (result.diagnostic.has_value()) {
    lastParseError_ = result.diagnostic;
  } else {
    lastParseError_.reset();
  }

  if (result.applied || !result.mutations.empty()) {
    frameVersion_.fetch_add(1, std::memory_order_release);
  }

  return result;
}

bool AsyncSVGDocument::loadFromString(std::string_view svgBytes) {
  ParseWarningSink sink;
  auto result = svg::parser::SVGParser::ParseSVG(svgBytes, sink, EditorParseOptions());
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
      svg::SVGGraphicsElement graphics =
          command.element->withReadAccess([&command](svg::DocumentReadAccess&, EntityHandle) {
            return command.element->cast<svg::SVGGraphicsElement>();
          });
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
        auto result = svg::parser::SVGParser::ParseSVG(command.bytes, sink, EditorParseOptions());
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
      xml::ApplySourceEditResult result = document_->setElementAttribute(
          element, xml::XMLQualifiedNameRef(command.attributeName), command.attributeValue);
      lastFlushResult_.sourceDeltas.insert(lastFlushResult_.sourceDeltas.end(),
                                           result.sourceDeltas.begin(), result.sourceDeltas.end());
      break;
    }

    case EditorCommand::Kind::RemoveAttribute: {
      if (!command.element.has_value()) {
        return;
      }
      // DOM-level attribute removal via the same structured-editing path as
      // SetAttribute, so the source reflection drops the attribute too.
      svg::SVGElement element = *command.element;
      xml::ApplySourceEditResult result = document_->removeElementAttribute(
          element, xml::XMLQualifiedNameRef(command.attributeName));
      lastFlushResult_.sourceDeltas.insert(lastFlushResult_.sourceDeltas.end(),
                                           result.sourceDeltas.begin(), result.sourceDeltas.end());
      break;
    }

    case EditorCommand::Kind::InsertElement: {
      if (!command.parentElement.has_value() || !command.element.has_value()) {
        return;
      }
      xml::ApplySourceEditResult result = document_->insertElement(
          *command.parentElement, *command.element, command.referenceElement);
      lastFlushResult_.sourceDeltas.insert(lastFlushResult_.sourceDeltas.end(),
                                           result.sourceDeltas.begin(), result.sourceDeltas.end());
      if (result.diagnostic.has_value()) {
        lastParseError_ = std::move(result.diagnostic);
      }
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
      xml::ApplySourceEditResult result = document_->removeElement(element);
      lastFlushResult_.sourceDeltas.insert(lastFlushResult_.sourceDeltas.end(),
                                           result.sourceDeltas.begin(), result.sourceDeltas.end());
      break;
    }

    case EditorCommand::Kind::CutShapes:
    case EditorCommand::Kind::PasteShapes:
    case EditorCommand::Kind::ConvertTextToOutlines: {
      // Shape-clipboard and text-to-outline structural replaces reparse `bytes`
      // into a fresh document, exactly like
      // ReplaceDocument(preserveUndoOnReparse=true). The kind discriminator is
      // preserved at this layer purely for labelling — undo entries are
      // recorded by the orchestrator (EditorShell), not here. On parse failure
      // we leave the existing document in place and surface a diagnostic.
      ParseWarningSink sink;
      auto result = svg::parser::SVGParser::ParseSVG(command.bytes, sink, EditorParseOptions());
      if (result.hasError()) {
        lastParseError_ = std::move(result.error());
        return;
      }
      (void)setDocumentMaybeStructural(std::move(result).result());
      break;
    }

    case EditorCommand::Kind::InsertText: {
      if (!command.parentElement.has_value() || !command.element.has_value()) {
        return;
      }
      // Insert the element first, then set its text content. `insertElement`
      // re-projects the element's XML subtree (which has no data child yet),
      // so setting the text before insertion would be overwritten by the
      // projection. Setting it afterward leaves the live DOM with the text.
      svg::SVGElement textElement = *command.element;
      xml::ApplySourceEditResult result =
          document_->insertElement(*command.parentElement, textElement, command.referenceElement);
      lastFlushResult_.sourceDeltas.insert(lastFlushResult_.sourceDeltas.end(),
                                           result.sourceDeltas.begin(), result.sourceDeltas.end());
      if (result.diagnostic.has_value()) {
        lastParseError_ = std::move(result.diagnostic);
      }
      if (textElement.isa<svg::SVGTextContentElement>()) {
        textElement.cast<svg::SVGTextContentElement>().setTextContent(command.textContent);
      }
      break;
    }

    case EditorCommand::Kind::SetTextContent: {
      if (!command.element.has_value()) {
        return;
      }
      svg::SVGElement textElement = *command.element;
      // Replace the live DOM text content so the renderer and inspector reflect
      // the new content immediately. The source pane re-syncs through the
      // editor's normal DOM->source path. `<style>` text lives in the element's
      // Data child nodes / parsed stylesheet rather than a TextComponent, so it
      // takes the SVGStyleElement path.
      if (textElement.isa<svg::SVGTextContentElement>()) {
        textElement.cast<svg::SVGTextContentElement>().setTextContent(command.textContent);
      } else if (textElement.isa<svg::SVGStyleElement>()) {
        textElement.cast<svg::SVGStyleElement>().setTextContent(command.textContent);
      }
      break;
    }
  }
}

}  // namespace donner::editor
