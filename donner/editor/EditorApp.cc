#include "donner/editor/EditorApp.h"

#include "donner/editor/AttributeWriteback.h"
#include "donner/editor/TextPatch.h"

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

svg::SVGElement ResolveSnapshotElement(AsyncSVGDocument& document, const UndoSnapshot& snapshot) {
  svg::SVGElement liveElement = snapshot.element;
  if (document.hasDocument() && snapshot.writebackTarget.has_value()) {
    if (auto resolved =
            resolveAttributeWritebackTarget(document.document(), *snapshot.writebackTarget);
        resolved.has_value()) {
      liveElement = *resolved;
    }
  }
  return liveElement;
}

void ApplyTimelineSnapshot(EditorApp& app, AsyncSVGDocument& document,
                           const UndoSnapshot& snapshot) {
  if (snapshot.kind == UndoSnapshot::Kind::DocumentSource) {
    app.applyMutation(EditorCommand::ReplaceDocumentCommand(snapshot.documentSource,
                                                            /*preserveUndoOnReparse=*/true));
    return;
  }

  svg::SVGElement liveElement = ResolveSnapshotElement(document, snapshot);

  // Route the restored transform through the command queue so every
  // DOM write — tool drags, text-pane re-parse, and undo — goes through
  // the same mutation seam. The queue coalesces with any pending
  // commands and applies on the next `flushFrame()`.
  app.applyMutation(EditorCommand::SetTransformCommand(liveElement, snapshot.transform));

  // Capture the source-text writeback target BEFORE the command drains
  // so the path-based target resolves against the in-sync document.
  // The writeback will be applied by `main.cc` after `flushFrame()`
  // lands the undone transform on the element — at that point the
  // transform the user sees on the canvas and the `transform=` value
  // in the source must agree. Without this the DOM reverts but the
  // source keeps the post-drag text, and the next edit lands on the
  // wrong baseline.
  if (snapshot.writebackTarget.has_value()) {
    app.enqueueTransformWriteback(EditorApp::CompletedTransformWriteback{
        .target = *snapshot.writebackTarget,
        .transform = snapshot.transform,
        .sourceTransformAttributeValue = snapshot.sourceTransformAttributeValue,
        .restoreSourceTransformAttributeValue = snapshot.restoreSourceTransformAttributeValue});
  } else if (auto target = captureAttributeWritebackTarget(liveElement); target.has_value()) {
    app.enqueueTransformWriteback(EditorApp::CompletedTransformWriteback{
        .target = std::move(*target),
        .transform = snapshot.transform,
        .sourceTransformAttributeValue = snapshot.sourceTransformAttributeValue,
        .restoreSourceTransformAttributeValue = snapshot.restoreSourceTransformAttributeValue});
  }

  for (const UndoSnapshot& extra : snapshot.extras) {
    ApplyTimelineSnapshot(app, document, extra);
  }
}

}  // namespace

EditorApp::EditorApp() = default;

bool EditorApp::loadFromString(std::string_view svgBytes) {
  selection_.clear();
  refreshFirstSelectionCache();
  controller_.reset();
  undoTimeline_.clear();
  pendingTransformWritebacks_.clear();
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

bool EditorApp::revertToCleanSource() {
  const std::string source(cleanSourceText_);
  if (source.empty()) {
    return false;
  }

  if (!loadFromString(source)) {
    return false;
  }

  setCleanSourceText(source);
  return true;
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

bool EditorApp::deleteSelectionWithUndo(std::string_view currentSourceText) {
  if (selection_.empty()) {
    return false;
  }

  const std::vector<svg::SVGElement> selected = selection_;
  std::vector<std::optional<AttributeWritebackTarget>> writebackTargets;
  writebackTargets.reserve(selected.size());

  std::vector<TextPatch> removePatches;
  removePatches.reserve(selected.size());
  for (const auto& element : selected) {
    std::optional<AttributeWritebackTarget> target = captureAttributeWritebackTarget(element);
    if (target.has_value()) {
      if (std::optional<TextPatch> patch = buildElementRemoveWriteback(currentSourceText, *target);
          patch.has_value()) {
        removePatches.push_back(std::move(*patch));
      }
    }
    writebackTargets.push_back(std::move(target));
  }

  std::string sourceAfterDelete(currentSourceText);
  const ApplyPatchesResult patchResult = applyPatches(sourceAfterDelete, removePatches);
  if (!removePatches.empty() && patchResult.applied == removePatches.size() &&
      patchResult.rejectedBounds == 0 && sourceAfterDelete != currentSourceText) {
    const char* label = selected.size() == 1u ? "Delete element" : "Delete elements";
    undoTimeline_.record(label, captureDocumentSourceSnapshot(selected.front(), currentSourceText),
                         captureDocumentSourceSnapshot(selected.front(), sourceAfterDelete));
  }

  setSelection(std::nullopt);
  for (std::size_t i = 0; i < selected.size(); ++i) {
    if (writebackTargets[i].has_value()) {
      enqueueElementRemoveWriteback(CompletedElementRemoveWriteback{
          .target = std::move(*writebackTargets[i]),
      });
    }
    applyMutation(EditorCommand::DeleteElementCommand(selected[i]));
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

  if (snapshot->kind == UndoSnapshot::Kind::DocumentSource) {
    pendingTransformWritebacks_.clear();
    pendingElementRemoveWritebacks_.clear();
  }
  ApplyTimelineSnapshot(*this, document_, *snapshot);
}

void EditorApp::redo() {
  auto snapshot = undoTimeline_.redo();
  if (!snapshot.has_value()) {
    return;
  }

  if (snapshot->kind == UndoSnapshot::Kind::DocumentSource) {
    pendingTransformWritebacks_.clear();
    pendingElementRemoveWritebacks_.clear();
  }
  ApplyTimelineSnapshot(*this, document_, *snapshot);
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
