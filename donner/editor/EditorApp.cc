#include "donner/editor/EditorApp.h"

#include "donner/editor/AttributeWriteback.h"

namespace donner::editor {

namespace {

/// AABB-vs-AABB intersection test. Returns true if the two boxes
/// overlap by any non-zero amount, including edge-touching contact.
/// Used by `hitTestRect` to decide which elements a marquee covers.
bool BoxesIntersect(const Box2d& a, const Box2d& b) {
  return a.topLeft.x <= b.bottomRight.x && a.bottomRight.x >= b.topLeft.x &&
         a.topLeft.y <= b.bottomRight.y && a.bottomRight.y >= b.topLeft.y;
}

/// Depth-first walk of the SVG tree rooted at `node`, invoking
/// `visit(geometry)` on every `SVGGeometryElement` encountered. Used
/// by `hitTestRect` so marquee selection lives entirely on top of
/// the public DOM API — no ECS reach-through.
template <typename Visitor>
void ForEachGeometryElement(const svg::SVGElement& node, Visitor& visit) {
  if (node.isa<svg::SVGGeometryElement>()) {
    visit(node.cast<svg::SVGGeometryElement>());
  }
  for (auto child = node.firstChild(); child.has_value(); child = child->nextSibling()) {
    ForEachGeometryElement(*child, visit);
  }
}

}  // namespace

EditorApp::EditorApp() = default;

bool EditorApp::loadFromString(std::string_view svgBytes) {
  selection_.clear();
  refreshFirstSelectionCache();
  controller_.reset();
  undoTimeline_.clear();
  pendingTransformWriteback_.reset();
  pendingElementRemoveWritebacks_.clear();
  const bool result = document_.loadFromString(svgBytes);
  // A successful load resets the dirty state — the in-memory document
  // now matches the last-loaded bytes. `setCurrentFilePath` should be
  // called separately by the caller if the bytes came from a file.
  if (result) {
    isDirty_ = false;
  }
  return result;
}

bool EditorApp::flushFrame() {
  std::optional<svg::SVGDocument> documentBeforeFlush;
  std::vector<AttributeWritebackTarget> selectionTargets;
  if (document_.hasDocument()) {
    documentBeforeFlush = document_.document();
    selectionTargets.reserve(selection_.size());
    for (const auto& element : selection_) {
      if (auto target = captureAttributeWritebackTarget(element); target.has_value()) {
        selectionTargets.push_back(std::move(*target));
      }
    }
  }

  if (!document_.flushFrame()) {
    return false;
  }

  const auto& documentFlush = document_.lastFlushResult();
  if (documentBeforeFlush.has_value() && document_.hasDocument() &&
      !(*documentBeforeFlush == document_.document())) {
    std::vector<svg::SVGElement> remappedSelection;
    remappedSelection.reserve(selectionTargets.size());
    for (const auto& target : selectionTargets) {
      if (auto element = resolveAttributeWritebackTarget(document_.document(), target);
          element.has_value()) {
        remappedSelection.push_back(*element);
      }
    }

    selection_ = std::move(remappedSelection);
    refreshFirstSelectionCache();
    controller_.reset();
    controllerVersion_ = 0;
    if (!(documentFlush.replacedDocument && documentFlush.preserveUndoOnReparse)) {
      undoTimeline_.clear();
    }
  }

  return true;
}

void EditorApp::setSelection(std::optional<svg::SVGElement> element) {
  selection_.clear();
  if (element.has_value()) {
    selection_.push_back(std::move(*element));
  }
  refreshFirstSelectionCache();
}

void EditorApp::setSelection(std::vector<svg::SVGElement> elements) {
  selection_ = std::move(elements);
  refreshFirstSelectionCache();
}

void EditorApp::toggleInSelection(const svg::SVGElement& element) {
  // SVGElement equality compares the underlying entt handle, so a
  // linear scan is correct (and fine for the typical N ≤ 100 case).
  for (auto it = selection_.begin(); it != selection_.end(); ++it) {
    if (*it == element) {
      selection_.erase(it);
      refreshFirstSelectionCache();
      return;
    }
  }
  selection_.push_back(element);
  refreshFirstSelectionCache();
}

void EditorApp::addToSelection(const svg::SVGElement& element) {
  for (const auto& existing : selection_) {
    if (existing == element) {
      return;
    }
  }
  selection_.push_back(element);
  refreshFirstSelectionCache();
}

void EditorApp::refreshFirstSelectionCache() {
  if (selection_.empty()) {
    cachedFirstSelection_.reset();
  } else {
    cachedFirstSelection_ = selection_.front();
  }
}

void EditorApp::undo() {
  auto snapshot = undoTimeline_.undo();
  if (!snapshot.has_value()) {
    return;
  }

  svg::SVGElement liveElement = snapshot->element;
  if (document_.hasDocument() && snapshot->writebackTarget.has_value()) {
    if (auto resolved =
            resolveAttributeWritebackTarget(document_.document(), *snapshot->writebackTarget);
        resolved.has_value()) {
      liveElement = *resolved;
    }
  }

  // Route the restored transform through the command queue so every
  // DOM write — tool drags, text-pane re-parse, and undo — goes through
  // the same mutation seam. The queue coalesces with any pending
  // commands and applies on the next `flushFrame()`.
  applyMutation(EditorCommand::SetTransformCommand(liveElement, snapshot->transform));

  // Capture the source-text writeback target BEFORE the command drains
  // so the path-based target resolves against the in-sync document.
  // The writeback will be applied by `main.cc` after `flushFrame()`
  // lands the undone transform on the element — at that point the
  // transform the user sees on the canvas and the `transform=` value
  // in the source must agree. Without this the DOM reverts but the
  // source keeps the post-drag text, and the next edit lands on the
  // wrong baseline.
  if (snapshot->writebackTarget.has_value()) {
    enqueueTransformWriteback(CompletedTransformWriteback{.target = *snapshot->writebackTarget,
                                                          .transform = snapshot->transform});
  } else if (auto target = captureAttributeWritebackTarget(liveElement); target.has_value()) {
    enqueueTransformWriteback(CompletedTransformWriteback{.target = std::move(*target),
                                                          .transform = snapshot->transform});
  }
}

void EditorApp::redo() {
  // "Redo" in the non-destructive timeline model is "break the active
  // undo chain and undo again". Breaking the chain causes the next
  // undo to start a fresh chain from the end of the timeline, which
  // means the first entry it walks is the most recently-appended
  // undo-entry — whose `before` state is the post-drag position.
  undoTimeline_.breakUndoChain();
  undo();
}

std::optional<svg::SVGGeometryElement> EditorApp::hitTest(const Vector2d& documentPoint) {
  if (!document_.hasDocument()) {
    return std::nullopt;
  }

  // Rebuild the DonnerController whenever the document version advances
  // past the version we built it for. The controller copies the SVGDocument
  // handle (which internally shares the registry), so reconstruction is
  // cheap relative to a full re-parse.
  const auto currentVersion = document_.currentFrameVersion();
  if (!controller_.has_value() || controllerVersion_ != currentVersion) {
    controller_.emplace(document_.document());
    controllerVersion_ = currentVersion;
  }

  return controller_->findIntersecting(documentPoint);
}

std::vector<svg::SVGGeometryElement> EditorApp::hitTestRect(const Box2d& documentRect) {
  std::vector<svg::SVGGeometryElement> hits;
  if (!document_.hasDocument()) {
    return hits;
  }

  // Walk the live document and collect every geometry element whose
  // world-space AABB intersects the marquee rect. We don't go through
  // `DonnerController` because it's point-only; the linear walk is
  // simple, allocation-light, and fine for documents up to a few
  // thousand elements (the typical editor workload).
  const svg::SVGElement root = document_.document().svgElement();
  auto visit = [&](const svg::SVGGeometryElement& geometry) {
    if (auto bounds = geometry.worldBounds(); bounds.has_value()) {
      if (BoxesIntersect(*bounds, documentRect)) {
        hits.push_back(geometry);
      }
    }
  };
  ForEachGeometryElement(root, visit);
  return hits;
}

}  // namespace donner::editor
