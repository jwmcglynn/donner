#include "donner/editor/EditorApp.h"

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "donner/base/FormatNumber.h"
#include "donner/css/CSS.h"
#include "donner/css/Declaration.h"
#include "donner/editor/AttributeWriteback.h"
#include "donner/editor/TextPatch.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGPathElement.h"

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

std::optional<std::string> MergeStyleProperty(std::string_view existingStyle,
                                              std::string_view propertyName,
                                              std::string_view propertyValue) {
  const std::string update = std::string(propertyName) + ": " + std::string(propertyValue);
  std::vector<css::Declaration> updateDeclarations = css::CSS::ParseStyleAttribute(update);
  if (updateDeclarations.empty()) {
    return std::nullopt;
  }

  const std::vector<css::Declaration> existingDeclarations =
      css::CSS::ParseStyleAttribute(existingStyle);
  return css::mergeStyleDeclarations(existingDeclarations, updateDeclarations);
}

struct PathOperationSelection {
  std::vector<Box2d> bounds;
};

bool PathOperationPrototypeSupports(PathOperationKind operation) {
  return operation == PathOperationKind::Union || operation == PathOperationKind::Intersect;
}

std::optional<Box2d> IntersectBoxes(const std::vector<Box2d>& bounds) {
  if (bounds.empty()) {
    return std::nullopt;
  }

  Box2d result = bounds.front();
  for (std::size_t i = 1; i < bounds.size(); ++i) {
    result.topLeft.x = std::max(result.topLeft.x, bounds[i].topLeft.x);
    result.topLeft.y = std::max(result.topLeft.y, bounds[i].topLeft.y);
    result.bottomRight.x = std::min(result.bottomRight.x, bounds[i].bottomRight.x);
    result.bottomRight.y = std::min(result.bottomRight.y, bounds[i].bottomRight.y);
  }

  if (result.width() <= 0.0 || result.height() <= 0.0) {
    return std::nullopt;
  }

  return result;
}

Box2d UnionBoxes(const std::vector<Box2d>& bounds) {
  Box2d result = bounds.front();
  for (std::size_t i = 1; i < bounds.size(); ++i) {
    result.addBox(bounds[i]);
  }
  return result;
}

std::string FormatPathPoint(const Vector2d& point) {
  return donner::detail::FormatNumberForSVG(point.x) + " " +
         donner::detail::FormatNumberForSVG(point.y);
}

std::string RectanglePathData(const Box2d& box) {
  const Vector2d topRight(box.bottomRight.x, box.topLeft.y);
  const Vector2d bottomLeft(box.topLeft.x, box.bottomRight.y);

  std::string result = "M ";
  result += FormatPathPoint(box.topLeft);
  result += " L ";
  result += FormatPathPoint(topRight);
  result += " L ";
  result += FormatPathPoint(box.bottomRight);
  result += " L ";
  result += FormatPathPoint(bottomLeft);
  result += " Z";
  return result;
}

PathOperationSelection CollectPathOperationSelection(std::span<const svg::SVGElement> selection) {
  PathOperationSelection result;
  result.bounds.reserve(selection.size());

  for (const svg::SVGElement& element : selection) {
    if (!element.isa<svg::SVGGeometryElement>()) {
      continue;
    }

    const svg::SVGGeometryElement geometry = element.cast<svg::SVGGeometryElement>();
    std::optional<Box2d> bounds = geometry.worldBounds();
    if (!bounds.has_value() || bounds->isEmpty()) {
      continue;
    }

    result.bounds.push_back(*bounds);
  }

  return result;
}

void CopyPathOperationStyle(const svg::SVGElement& source, svg::SVGPathElement& target) {
  constexpr std::array<const char*, 8> kCopiedAttributes = {
      "class",        "fill",           "fill-opacity", "stroke",
      "stroke-width", "stroke-opacity", "opacity",      "fill-rule",
  };

  for (const char* name : kCopiedAttributes) {
    if (std::optional<RcString> value = source.getAttribute(name); value.has_value()) {
      target.setAttribute(name, std::string_view(*value));
    }
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

bool EditorApp::setAttributeOnSelection(std::string_view attrName, std::string_view attrValue) {
  if (selection_.empty()) {
    return false;
  }

  for (const svg::SVGElement& element : selection_) {
    applyMutation(
        EditorCommand::SetAttributeCommand(element, std::string(attrName), std::string(attrValue)));
  }
  return true;
}

bool EditorApp::setStylePropertyOnSelection(std::string_view propertyName,
                                            std::string_view propertyValue) {
  if (selection_.empty()) {
    return false;
  }

  bool queuedMutation = false;
  for (const svg::SVGElement& element : selection_) {
    const std::optional<RcString> styleAttribute = element.getAttribute("style");
    const std::string_view existingStyle =
        styleAttribute.has_value() ? std::string_view(*styleAttribute) : std::string_view();
    const std::optional<std::string> mergedStyle =
        MergeStyleProperty(existingStyle, propertyName, propertyValue);
    if (!mergedStyle.has_value()) {
      continue;
    }

    applyMutation(EditorCommand::SetAttributeCommand(element, "style", *mergedStyle));
    queuedMutation = true;
  }
  return queuedMutation;
}

bool EditorApp::setStrokeWidthOnSelection(double strokeWidth) {
  const double clampedStrokeWidth = std::max(0.0, strokeWidth);
  return setStylePropertyOnSelection("stroke-width",
                                     donner::detail::FormatNumberForSVG(clampedStrokeWidth));
}

void EditorApp::setActiveStrokeWidth(double strokeWidth) {
  activePaintStyle_.strokeWidth = std::max(0.0, strokeWidth);
}

PathOperationAvailability EditorApp::pathOperationAvailability(PathOperationKind operation) const {
  if (!document_.hasDocument()) {
    return {.canApply = false, .reason = "No SVG document is loaded"};
  }

  if (!PathOperationPrototypeSupports(operation)) {
    return {.canApply = false, .reason = "Prototype supports Union and Intersect first"};
  }

  if (selection_.size() < 2u) {
    return {.canApply = false, .reason = "Select at least two bounded geometry elements"};
  }

  const PathOperationSelection pathSelection = CollectPathOperationSelection(selection_);
  if (pathSelection.bounds.size() != selection_.size()) {
    return {.canApply = false, .reason = "Selection includes unsupported or empty geometry"};
  }

  if (operation == PathOperationKind::Intersect &&
      !IntersectBoxes(pathSelection.bounds).has_value()) {
    return {.canApply = false, .reason = "Selected bounds do not overlap"};
  }

  return {.canApply = true};
}

bool EditorApp::applyPathOperation(PathOperationKind operation) {
  const PathOperationAvailability availability = pathOperationAvailability(operation);
  if (!availability.canApply) {
    return false;
  }

  const std::vector<svg::SVGElement> selected = selection_;
  const PathOperationSelection pathSelection = CollectPathOperationSelection(selected);
  std::optional<Box2d> resultBox;
  switch (operation) {
    case PathOperationKind::Union: resultBox = UnionBoxes(pathSelection.bounds); break;
    case PathOperationKind::Intersect: resultBox = IntersectBoxes(pathSelection.bounds); break;
    case PathOperationKind::SubtractFront:
    case PathOperationKind::SubtractBack:
    case PathOperationKind::Exclude: return false;
  }

  if (!resultBox.has_value()) {
    return false;
  }

  svg::SVGDocument& document = document_.document();
  svg::SVGPathElement resultPath = svg::SVGPathElement::Create(document);
  resultPath.setAttribute("d", RectanglePathData(*resultBox));
  CopyPathOperationStyle(selected.front(), resultPath);

  svg::SVGElement parent = selected.front().parentElement().value_or(document.svgElement());
  std::optional<svg::SVGElement> referenceElement;
  if (const std::optional<svg::SVGElement> selectedParent = selected.front().parentElement();
      selectedParent.has_value() && *selectedParent == parent) {
    referenceElement = selected.front();
  }

  applyMutation(EditorCommand::InsertElementCommand(parent, resultPath, referenceElement));
  for (const svg::SVGElement& element : selected) {
    applyMutation(EditorCommand::DeleteElementCommand(element));
  }
  setSelection(resultPath);
  return true;
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
