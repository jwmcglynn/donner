#include "donner/editor/EditorApp.h"

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "donner/base/FormatNumber.h"
#include "donner/base/PathOps.h"
#include "donner/css/CSS.h"
#include "donner/css/Declaration.h"
#include "donner/editor/AttributeWriteback.h"
#include "donner/editor/TextPatch.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/SVGPathElement.h"
#include "donner/svg/core/Stroke.h"
#include "donner/svg/properties/PaintServer.h"

namespace donner::editor {

namespace {

/// AABB-vs-AABB intersection test. Returns true if the two boxes
/// overlap by any non-zero amount, including edge-touching contact.
/// Used by `hitTestRect` to decide which elements a marquee covers.
bool BoxesIntersect(const Box2d& a, const Box2d& b) {
  return a.topLeft.x <= b.bottomRight.x && a.bottomRight.x >= b.topLeft.x &&
         a.topLeft.y <= b.bottomRight.y && a.bottomRight.y >= b.topLeft.y;
}

Path RectPath(const Box2d& box) {
  return PathBuilder()
      .moveTo(box.topLeft)
      .lineTo(Vector2d(box.bottomRight.x, box.topLeft.y))
      .lineTo(box.bottomRight)
      .lineTo(Vector2d(box.topLeft.x, box.bottomRight.y))
      .closePath()
      .build();
}

std::array<Vector2d, 4> BoxCorners(const Box2d& box) {
  return {
      box.topLeft,
      Vector2d(box.bottomRight.x, box.topLeft.y),
      box.bottomRight,
      Vector2d(box.topLeft.x, box.bottomRight.y),
  };
}

LineCap ToLineCap(svg::StrokeLinecap cap) {
  switch (cap) {
    case svg::StrokeLinecap::Butt: return LineCap::Butt;
    case svg::StrokeLinecap::Round: return LineCap::Round;
    case svg::StrokeLinecap::Square: return LineCap::Square;
  }
  return LineCap::Butt;
}

LineJoin ToLineJoin(svg::StrokeLinejoin join) {
  switch (join) {
    case svg::StrokeLinejoin::Miter: return LineJoin::Miter;
    case svg::StrokeLinejoin::MiterClip: return LineJoin::Miter;
    case svg::StrokeLinejoin::Round: return LineJoin::Round;
    case svg::StrokeLinejoin::Bevel: return LineJoin::Bevel;
    case svg::StrokeLinejoin::Arcs: return LineJoin::Miter;
  }
  return LineJoin::Miter;
}

bool PathEndpointIntersectsRect(const Path& path, const Transform2d& documentFromPath,
                                const Box2d& documentRect) {
  bool intersects = false;
  path.forEach([&](Path::Verb /*verb*/, std::span<const Vector2d> points) {
    if (points.empty()) {
      return;
    }

    if (documentRect.contains(documentFromPath.transformPosition(points.back()))) {
      intersects = true;
    }
  });
  return intersects;
}

int CountRectCornersInsidePath(const Path& path, FillRule fillRule,
                               const Transform2d& documentFromPath, const Box2d& documentRect) {
  if (std::abs(documentFromPath.determinant()) < 1e-12) {
    return 0;
  }

  const Transform2d pathFromDocument = documentFromPath.inverse();
  int insideCount = 0;
  for (const Vector2d& corner : BoxCorners(documentRect)) {
    if (path.isInside(pathFromDocument.transformPosition(corner), fillRule)) {
      ++insideCount;
    }
  }
  return insideCount;
}

bool FilledPathIntersectsRect(const Path& path, FillRule fillRule,
                              const Transform2d& documentFromPath, const Box2d& documentRect) {
  if (PathEndpointIntersectsRect(path, documentFromPath, documentRect)) {
    return true;
  }

  const int insideCornerCount =
      CountRectCornersInsidePath(path, fillRule, documentFromPath, documentRect);
  if (insideCornerCount == 4) {
    // The marquee is fully inside this filled shape. Do not select large containing geometry
    // (backgrounds/glows) unless its own boundary or vertices enter the marquee.
    return false;
  }
  if (insideCornerCount > 0) {
    return true;
  }

  const std::array<PathBooleanInput, 2> inputs = {
      PathBooleanInput{
          .path = path,
          .fillRule = fillRule,
          .outputFromPath = documentFromPath,
      },
      PathBooleanInput{
          .path = RectPath(documentRect),
          .fillRule = FillRule::NonZero,
          .outputFromPath = Transform2d(),
      },
  };
  const PathBooleanResult result = ApplyPathBoolean(PathBooleanOp::Intersect, inputs);
  return result.status == PathBooleanStatus::Ok;
}

bool GeometryIntersectsRect(const svg::SVGGeometryElement& geometry, const Box2d& documentRect) {
  std::optional<Box2d> bounds = geometry.worldBounds();
  if (!bounds.has_value()) {
    return false;
  }

  const auto style = geometry.getComputedStyle();
  const double strokeWidth = style.strokeWidth.get().value().value;
  const bool hasStroke =
      strokeWidth > 0.0 && !style.stroke.get().value().is<svg::PaintServer::None>();
  const Box2d interactionBounds =
      hasStroke ? bounds->inflatedBy(strokeWidth * style.strokeMiterlimit.get().value()) : *bounds;
  if (!BoxesIntersect(interactionBounds, documentRect)) {
    return false;
  }

  std::optional<Path> spline = geometry.computedSpline();
  if (!spline.has_value() || spline->empty()) {
    return false;
  }

  const Transform2d documentFromGeometry = geometry.elementFromWorld();
  if (!style.fill.get().value().is<svg::PaintServer::None>() &&
      FilledPathIntersectsRect(*spline, style.fillRule.get().value(), documentFromGeometry,
                               documentRect)) {
    return true;
  }

  if (hasStroke) {
    StrokeStyle strokeStyle;
    strokeStyle.width = strokeWidth;
    strokeStyle.cap = ToLineCap(style.strokeLinecap.get().value());
    strokeStyle.join = ToLineJoin(style.strokeLinejoin.get().value());
    strokeStyle.miterLimit = style.strokeMiterlimit.get().value();
    const Path strokePath = spline->strokeToFill(strokeStyle);
    return FilledPathIntersectsRect(strokePath, FillRule::NonZero, documentFromGeometry,
                                    documentRect);
  }

  return false;
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

/// Depth-first SVG tree walk in paint order, including non-geometry containers.
template <typename Visitor>
void ForEachElement(const svg::SVGElement& node, Visitor& visit) {
  visit(node);
  for (auto child = node.firstChild(); child.has_value(); child = child->nextSibling()) {
    ForEachElement(*child, visit);
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
  std::vector<PathBooleanInput> inputs;
};

std::vector<svg::SVGElement> SortSelectionByPaintOrder(const svg::SVGDocument& document,
                                                       std::span<const svg::SVGElement> selection) {
  std::vector<svg::SVGElement> result;
  result.reserve(selection.size());

  auto visit = [&](const svg::SVGElement& element) {
    if (std::find(selection.begin(), selection.end(), element) != selection.end()) {
      result.push_back(element);
    }
  };
  ForEachElement(document.svgElement(), visit);
  return result;
}

PathOperationSelection CollectPathOperationSelection(std::span<const svg::SVGElement> selection) {
  PathOperationSelection result;
  result.inputs.reserve(selection.size());

  for (const svg::SVGElement& element : selection) {
    std::optional<svg::SVGGeometryElement> maybeGeometry =
        element.withReadAccess([&element](svg::DocumentReadAccess&,
                                          EntityHandle) -> std::optional<svg::SVGGeometryElement> {
          if (!element.isa<svg::SVGGeometryElement>()) {
            return std::nullopt;
          }
          return element.cast<svg::SVGGeometryElement>();
        });
    if (!maybeGeometry.has_value()) {
      continue;
    }

    const svg::SVGGeometryElement geometry = element.cast<svg::SVGGeometryElement>();
    std::optional<Path> spline = geometry.computedSpline();
    if (!spline.has_value() || spline->empty()) {
      continue;
    }

    const Transform2d documentFromElement = geometry.elementFromWorld();
    result.inputs.push_back(PathBooleanInput{
        .path = std::move(*spline),
        .fillRule = geometry.getComputedStyle().fillRule.getRequired(),
        .outputFromPath = documentFromElement,
    });
  }

  return result;
}

PathBooleanOp BooleanOpForEditorOperation(PathOperationKind operation) {
  switch (operation) {
    case PathOperationKind::Union: return PathBooleanOp::Union;
    case PathOperationKind::Intersect: return PathBooleanOp::Intersect;
    case PathOperationKind::SubtractFront: return PathBooleanOp::Difference;
    case PathOperationKind::SubtractBack: return PathBooleanOp::Difference;
    case PathOperationKind::Exclude: return PathBooleanOp::Xor;
  }
  return PathBooleanOp::Union;
}

PathBooleanOptions EditorPathBooleanOptions() {
  return PathBooleanOptions{
      .geometricTolerance = 1e-3,
      .maxCurveCount = 20000,
      .maxIntersections = 20000,
      .maxOutputCommands = 8192,
  };
}

std::vector<PathBooleanInput> InputsForEditorOperation(PathOperationKind operation,
                                                       const PathOperationSelection& selection) {
  if (operation != PathOperationKind::SubtractBack || selection.inputs.empty()) {
    return selection.inputs;
  }

  std::vector<PathBooleanInput> inputs;
  inputs.reserve(selection.inputs.size());
  inputs.push_back(selection.inputs.back());
  inputs.insert(inputs.end(), selection.inputs.begin(), selection.inputs.end() - 1);
  return inputs;
}

Path CombineBooleanPaths(std::span<const Path> paths) {
  PathBuilder builder;
  for (const Path& path : paths) {
    builder.addPath(path);
  }
  return builder.build();
}

Path TransformPath(const Path& path, const Transform2d& outputFromPath) {
  PathBuilder builder;
  const std::span<const Vector2d> points = path.points();
  for (const Path::Command& command : path.commands()) {
    switch (command.verb) {
      case Path::Verb::MoveTo:
        builder.moveTo(outputFromPath.transformPosition(points[command.pointIndex]));
        break;
      case Path::Verb::LineTo:
        builder.lineTo(outputFromPath.transformPosition(points[command.pointIndex]));
        break;
      case Path::Verb::QuadTo:
        builder.quadTo(outputFromPath.transformPosition(points[command.pointIndex]),
                       outputFromPath.transformPosition(points[command.pointIndex + 1]));
        break;
      case Path::Verb::CurveTo:
        builder.curveTo(outputFromPath.transformPosition(points[command.pointIndex]),
                        outputFromPath.transformPosition(points[command.pointIndex + 1]),
                        outputFromPath.transformPosition(points[command.pointIndex + 2]));
        break;
      case Path::Verb::ClosePath: builder.closePath(); break;
    }
  }
  return builder.build();
}

svg::SVGElement BaseElementForPathOperation(PathOperationKind operation,
                                            std::span<const svg::SVGElement> selection) {
  switch (operation) {
    case PathOperationKind::SubtractFront: return selection.front();
    case PathOperationKind::Union:
    case PathOperationKind::Intersect:
    case PathOperationKind::SubtractBack:
    case PathOperationKind::Exclude: return selection.back();
  }
  return selection.front();
}

std::optional<Box2d> InputUnionBounds(const PathOperationSelection& selection) {
  std::optional<Box2d> bounds;
  for (const PathBooleanInput& input : selection.inputs) {
    const Box2d inputBounds = input.path.transformedBounds(input.outputFromPath);
    if (bounds.has_value()) {
      bounds->addBox(inputBounds);
    } else {
      bounds = inputBounds;
    }
  }
  return bounds;
}

std::optional<Box2d> InputIntersectionBounds(const PathOperationSelection& selection) {
  std::optional<Box2d> bounds;
  for (const PathBooleanInput& input : selection.inputs) {
    const Box2d inputBounds = input.path.transformedBounds(input.outputFromPath);
    if (!bounds.has_value()) {
      bounds = inputBounds;
      continue;
    }

    bounds->topLeft.x = std::max(bounds->topLeft.x, inputBounds.topLeft.x);
    bounds->topLeft.y = std::max(bounds->topLeft.y, inputBounds.topLeft.y);
    bounds->bottomRight.x = std::min(bounds->bottomRight.x, inputBounds.bottomRight.x);
    bounds->bottomRight.y = std::min(bounds->bottomRight.y, inputBounds.bottomRight.y);
    if (bounds->width() <= 0.0 || bounds->height() <= 0.0) {
      return std::nullopt;
    }
  }
  return bounds;
}

bool BoxContainsBox(const Box2d& outer, const Box2d& inner, double tolerance) {
  return inner.topLeft.x >= outer.topLeft.x - tolerance &&
         inner.topLeft.y >= outer.topLeft.y - tolerance &&
         inner.bottomRight.x <= outer.bottomRight.x + tolerance &&
         inner.bottomRight.y <= outer.bottomRight.y + tolerance;
}

bool PathOperationResultFitsInputBounds(const Path& result,
                                        const PathOperationSelection& selection) {
  const std::optional<Box2d> inputBounds = InputUnionBounds(selection);
  if (!inputBounds.has_value()) {
    return false;
  }
  constexpr double kResultBoundsTolerance = 0.5;
  return BoxContainsBox(*inputBounds, result.bounds(), kResultBoundsTolerance);
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

std::vector<AttributeWritebackTarget> CaptureSelectionTargets(
    std::span<const svg::SVGElement> selection) {
  std::vector<AttributeWritebackTarget> targets;
  targets.reserve(selection.size());
  for (const svg::SVGElement& element : selection) {
    if (std::optional<AttributeWritebackTarget> target = captureAttributeWritebackTarget(element);
        target.has_value()) {
      targets.push_back(std::move(*target));
    }
  }
  return targets;
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
    app.restoreSelectionAfterNextDocumentReplace(snapshot.selectionTargets);
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
  pendingDocumentSourceUndo_.reset();
  pendingSelectionRestoreTargets_.reset();
  const bool result = document_.loadFromString(svgBytes);
  // A successful load resets the dirty state — the in-memory document
  // now matches the last-loaded bytes. `setCurrentFilePath` should be
  // called separately by the caller if the bytes came from a file.
  if (result) {
    isDirty_ = false;
  }
  return result;
}

void EditorApp::restoreSelectionAfterNextDocumentReplace(
    std::vector<AttributeWritebackTarget> targets) {
  pendingSelectionRestoreTargets_ = std::move(targets);
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

  if (pendingSelectionRestoreTargets_.has_value() && document_.hasDocument()) {
    std::vector<svg::SVGElement> restoredSelection;
    restoredSelection.reserve(pendingSelectionRestoreTargets_->size());
    for (const AttributeWritebackTarget& target : *pendingSelectionRestoreTargets_) {
      if (auto element = resolveAttributeWritebackTarget(document_.document(), target);
          element.has_value()) {
        restoredSelection.push_back(*element);
      }
    }

    selection_ = std::move(restoredSelection);
    refreshFirstSelectionCache();
    controller_.reset();
    controllerVersion_ = 0;
    pendingSelectionRestoreTargets_.reset();
  }

  if (pendingDocumentSourceUndo_.has_value()) {
    if (document_.hasDocument() && document_.document().hasSourceStore()) {
      std::string sourceAfter(document_.document().source());
      if (sourceAfter != pendingDocumentSourceUndo_->before.documentSource) {
        UndoSnapshot after =
            captureDocumentSourceSnapshot(pendingDocumentSourceUndo_->before.element, sourceAfter);
        after.selectionTargets = CaptureSelectionTargets(selection_);
        undoTimeline_.record(pendingDocumentSourceUndo_->label,
                             std::move(pendingDocumentSourceUndo_->before), std::move(after));
      }
    }
    pendingDocumentSourceUndo_.reset();
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

  if (document_.hasPendingMutations()) {
    return {.canApply = false, .reason = "Document edits are still applying"};
  }

  if (selection_.size() < 2u) {
    return {.canApply = false, .reason = "Select at least two path-convertible elements"};
  }

  const std::vector<svg::SVGElement> selected =
      SortSelectionByPaintOrder(document_.document(), selection_);
  if (selected.size() != selection_.size()) {
    return {.canApply = false, .reason = "Selection includes detached geometry"};
  }

  const PathOperationSelection pathSelection = CollectPathOperationSelection(selected);
  if (pathSelection.inputs.size() != selection_.size()) {
    return {.canApply = false, .reason = "Selection includes unsupported or empty geometry"};
  }

  if (operation == PathOperationKind::Intersect &&
      !InputIntersectionBounds(pathSelection).has_value()) {
    return {.canApply = false, .reason = "Selected path bounds do not overlap"};
  }

  return {.canApply = true};
}

bool EditorApp::applyPathOperation(PathOperationKind operation) {
  const PathOperationAvailability availability = pathOperationAvailability(operation);
  if (!availability.canApply) {
    return false;
  }

  svg::SVGDocument& document = document_.document();
  const std::vector<svg::SVGElement> selected = SortSelectionByPaintOrder(document, selection_);
  if (selected.size() != selection_.size()) {
    return false;
  }

  const PathOperationSelection pathSelection = CollectPathOperationSelection(selected);
  const std::vector<PathBooleanInput> booleanInputs =
      InputsForEditorOperation(operation, pathSelection);
  const PathBooleanResult booleanResult = ApplyPathBoolean(
      BooleanOpForEditorOperation(operation), booleanInputs, EditorPathBooleanOptions());
  if (booleanResult.status != PathBooleanStatus::Ok || booleanResult.paths.empty()) {
    return false;
  }

  const Path resultDocumentSpline = CombineBooleanPaths(booleanResult.paths);
  if (resultDocumentSpline.empty()) {
    return false;
  }
  if (!PathOperationResultFitsInputBounds(resultDocumentSpline, pathSelection)) {
    return false;
  }

  const svg::SVGElement baseElement = BaseElementForPathOperation(operation, selected);
  svg::SVGElement parent = baseElement.parentElement().value_or(document.svgElement());
  Transform2d parentFromDocument;
  if (parent.isa<svg::SVGGraphicsElement>()) {
    const Transform2d documentFromParent =
        parent.cast<svg::SVGGraphicsElement>().elementFromWorld();
    parentFromDocument = documentFromParent.inverse();
  }

  const Path resultSpline = TransformPath(resultDocumentSpline, parentFromDocument);
  const RcString resultPathData = resultSpline.toSVGPathData();
  svg::SVGPathElement resultPath = svg::SVGPathElement::Create(document);
  resultPath.setAttribute("d", std::string_view(resultPathData));
  CopyPathOperationStyle(baseElement, resultPath);

  if (document.hasSourceStore()) {
    UndoSnapshot before = captureDocumentSourceSnapshot(baseElement, document.source());
    before.selectionTargets = CaptureSelectionTargets(selected);
    pendingDocumentSourceUndo_ = PendingDocumentSourceUndo{
        .label = "Path operation",
        .before = std::move(before),
    };
  }

  std::optional<svg::SVGElement> referenceElement;
  if (const std::optional<svg::SVGElement> selectedParent = baseElement.parentElement();
      selectedParent.has_value() && *selectedParent == parent) {
    referenceElement = baseElement;
  }

  applyMutation(EditorCommand::InsertElementCommand(parent, resultPath, referenceElement));
  for (const svg::SVGElement& element : selected) {
    applyMutation(EditorCommand::DeleteElementCommand(element));
  }
  setSelection(resultPath);
  return true;
}

PathOperationAvailability EditorApp::compoundPathUnbundleAvailability(
    std::optional<svg::SVGElement> target) const {
  if (!document_.hasDocument()) {
    return {.canApply = false, .reason = "No SVG document is loaded"};
  }

  if (document_.hasPendingMutations()) {
    return {.canApply = false, .reason = "Document edits are still applying"};
  }

  std::optional<svg::SVGElement> resolvedTarget = ResolveCompoundPathUnbundleTarget(*this, target);
  if (!resolvedTarget.has_value()) {
    return {.canApply = false, .reason = "Select one compound path"};
  }

  if (!resolvedTarget->isa<svg::SVGPathElement>()) {
    return {.canApply = false, .reason = "Target is not a path"};
  }

  if (!resolvedTarget->parentElement().has_value()) {
    return {.canApply = false, .reason = "Target path is detached"};
  }

  const svg::SVGPathElement pathElement = resolvedTarget->cast<svg::SVGPathElement>();
  const std::optional<Path> spline = pathElement.computedSpline();
  if (!spline.has_value() || spline->empty()) {
    return {.canApply = false, .reason = "Path has no geometry"};
  }

  const CompoundPathSplit split = SplitCompoundPathIntoContours(*spline);
  if (split.components.size() < 2u) {
    return {.canApply = false, .reason = "Path has one contour"};
  }

  return {.canApply = true};
}

bool EditorApp::unbundleCompoundPath(std::optional<svg::SVGElement> target) {
  const PathOperationAvailability availability = compoundPathUnbundleAvailability(target);
  if (!availability.canApply) {
    return false;
  }

  svg::SVGDocument& document = document_.document();
  std::optional<svg::SVGElement> resolvedTarget = ResolveCompoundPathUnbundleTarget(*this, target);
  if (!resolvedTarget.has_value() || !resolvedTarget->isa<svg::SVGPathElement>()) {
    return false;
  }

  const svg::SVGPathElement sourcePath = resolvedTarget->cast<svg::SVGPathElement>();
  const std::optional<Path> sourceSpline = sourcePath.computedSpline();
  if (!sourceSpline.has_value()) {
    return false;
  }

  const CompoundPathSplit split = SplitCompoundPathIntoContours(*sourceSpline);
  if (split.components.size() < 2u) {
    return false;
  }

  svg::SVGElement parent = sourcePath.parentElement().value_or(document.svgElement());
  std::vector<svg::SVGElement> replacementSelection;
  replacementSelection.reserve(split.components.size());

  if (document.hasSourceStore()) {
    UndoSnapshot before = captureDocumentSourceSnapshot(sourcePath, document.source());
    const std::array<svg::SVGElement, 1> sourceSelection = {sourcePath};
    before.selectionTargets = CaptureSelectionTargets(sourceSelection);
    pendingDocumentSourceUndo_ = PendingDocumentSourceUndo{
        .label = "Unbundle compound path",
        .before = std::move(before),
    };
  }

  for (const Path& component : split.components) {
    svg::SVGPathElement replacement = svg::SVGPathElement::Create(document);
    const RcString pathData = component.toSVGPathData();
    replacement.setAttribute("d", std::string_view(pathData));
    CopyUnbundleAttributes(sourcePath, replacement);
    replacementSelection.push_back(replacement);
    applyMutation(EditorCommand::InsertElementCommand(parent, replacement, sourcePath));
  }
  applyMutation(EditorCommand::DeleteElementCommand(sourcePath));

  setSelection(std::move(replacementSelection));
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

  // Walk the live document and collect every geometry element whose filled or stroked path
  // intersects the marquee rect. We don't go through `DonnerController` because it's point-only;
  // the linear walk is simple and fine for typical editor workloads.
  //
  // §concurrent-dom: the editor keeps the live document in ConcurrentDom, so this UI-thread walk
  // needs a scoped access guard or its DOM reads (isa / firstChild / nextSibling) trip the
  // `ElementAnchor` release assertion. GeometryIntersectsRect() calls worldBounds(), which lazily
  // computes shape state under *write* access, so the whole traversal takes one coarse write guard.
  svg::SVGDocument doc = document_.document();
  doc.withWriteAccess([&](svg::DocumentWriteAccess&) {
    const svg::SVGElement root = doc.svgElement();
    auto visit = [&](const svg::SVGGeometryElement& geometry) {
      if (GeometryIntersectsRect(geometry, documentRect)) {
        hits.push_back(geometry);
      }
    };
    ForEachGeometryElement(root, visit);
  });
  return hits;
}

}  // namespace donner::editor
