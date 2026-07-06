#include "donner/editor/PenTool.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "donner/base/FormatNumber.h"
#include "donner/base/MathUtils.h"
#include "donner/base/Path.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/EditorCommand.h"
#include "donner/editor/UndoTimeline.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/SVGSVGElement.h"
#include "donner/svg/parser/PathParser.h"

namespace donner::editor {

namespace {

// Close the contour when a click lands within this many logical (screen)
// pixels of the first anchor. Matches the v0.8 Pen tool quality bar.
constexpr double kClosePointScreenTolerance = 6.0;

// Anchor/control-point hit tolerance in logical (screen) pixels - slightly
// larger than the drawn anchor (5 px) and control point (4 px) chrome so the
// targets stay grabbable at every zoom.
constexpr double kPointHitScreenTolerance = 6.0;

// Constrain `documentPoint` to the nearest 45-degree direction around
// `origin` by projecting it onto that direction - the cursor's travel along
// the constrained axis is preserved, matching standard design-tool Shift
// behavior.
Vector2d ConstrainTo45Degrees(const Vector2d& origin, const Vector2d& documentPoint) {
  const Vector2d delta = documentPoint - origin;
  const double length = std::hypot(delta.x, delta.y);
  if (length <= 1e-9) {
    return documentPoint;
  }

  constexpr double kStep = MathConstants<double>::kPi / 4.0;
  const int octant =
      static_cast<int>(((std::lround(std::atan2(delta.y, delta.x) / kStep) % 8) + 8) % 8);
  // Integer direction vectors keep axis/diagonal projections numerically exact
  // (no cos/sin round-off leaking into serialized path data).
  static constexpr std::array<Vector2d, 8> kDirections = {
      Vector2d(1.0, 0.0),  Vector2d(1.0, 1.0),   Vector2d(0.0, 1.0),  Vector2d(-1.0, 1.0),
      Vector2d(-1.0, 0.0), Vector2d(-1.0, -1.0), Vector2d(0.0, -1.0), Vector2d(1.0, -1.0),
  };
  const Vector2d direction = kDirections[static_cast<std::size_t>(octant)];
  return origin + direction * (delta.dot(direction) / direction.dot(direction));
}

std::string FormatPoint(const Vector2d& point) {
  return donner::detail::FormatNumberForSVG(point.x) + " " +
         donner::detail::FormatNumberForSVG(point.y);
}

std::string MoveToPathData(const Vector2d& point) {
  return "M " + FormatPoint(point);
}

bool SamePoint(const Vector2d& lhs, const Vector2d& rhs) {
  return std::hypot(lhs.x - rhs.x, lhs.y - rhs.y) <= 1e-9;
}

bool SameOptionalPoint(const std::optional<Vector2d>& lhs, const std::optional<Vector2d>& rhs) {
  if (lhs.has_value() != rhs.has_value()) {
    return false;
  }

  if (!lhs.has_value()) {
    return true;
  }

  return SamePoint(*lhs, *rhs);
}

bool HasMeaningfulHandle(const Vector2d& anchor, const std::optional<Vector2d>& handle) {
  return handle.has_value() && !SamePoint(*handle, anchor);
}

}  // namespace

void PenTool::onMouseDown(EditorApp& editor, const Vector2d& documentPoint,
                          MouseModifiers modifiers) {
  if (!editor.document().hasDocument()) {
    cancel();
    return;
  }

  const double toleranceDoc =
      kPointHitScreenTolerance / std::max(modifiers.pixelsPerDocUnit, 0.000001);

  if (activePath_.has_value()) {
    // Close-path wins over point editing for the first anchor - unless
    // Cmd/Ctrl explicitly restricts the gesture to point editing.
    if (!modifiers.command && shouldCloseAt(documentPoint, modifiers)) {
      closePath(editor, documentPoint, toleranceDoc);
      return;
    }

    if (std::optional<PointHit> hit = HitTestPoints(anchors_, documentPoint, toleranceDoc)) {
      beginPointDrag(*hit);
      // Alt/Option-click (no drag) on any anchor toggles corner/smooth;
      // otherwise a click (no drag) on the draft's last anchor retracts its
      // outgoing handle. Both resolve on mouse-up if no movement happened.
      pendingConvertAnchor_ = modifiers.option && hit->mode == DragMode::MoveAnchor;
      pendingRetractLastAnchor_ = !pendingConvertAnchor_ && !editingExistingPath_ &&
                                  hit->mode == DragMode::MoveAnchor &&
                                  hit->index + 1u == anchors_.size();
      return;
    }

    if (modifiers.command) {
      return;  // Point-edit only: a miss never places an anchor.
    }

    if (std::optional<SegmentHit> segmentHit =
            HitTestSegments(anchors_, closed_, documentPoint, toleranceDoc)) {
      // Clicking ON an existing segment inserts an anchor at the hit point,
      // splitting the segment without changing the rendered shape.
      const std::size_t insertedIndex = insertAnchorAt(*segmentHit);
      rebuildActivePathData();
      commitActivePathData(editor);
      beginPointDrag(PointHit{.mode = DragMode::MoveAnchor, .index = insertedIndex});
      return;
    }

    appendLine(editor, constrainedPoint(documentPoint, modifiers));
    return;
  }

  if (std::optional<SelectedPathState> state = stateForSelectedPath(editor); state.has_value()) {
    // §concurrent-dom: `cast<SVGPathElement>()` resolves the selected
    // element's EntityHandle, a raw ECS read. The live editor keeps the
    // document in ThreadingMode::ConcurrentDom, which requires the calling
    // thread to hold a read-access scope keyed to that element's own document
    // state; without it SVGElement's scoped-access release assert aborts the
    // editor. Scope the read off the selected element itself (the same idiom
    // SelectTool uses) so the access marker matches the entity being resolved.
    const svg::SVGElement selectedElement = editor.selectedElements().front();
    const svg::SVGPathElement pathElement =
        selectedElement.withReadAccess([&selectedElement](svg::DocumentReadAccess&, EntityHandle) {
          return selectedElement.cast<svg::SVGPathElement>();
        });

    if (std::optional<PointHit> hit = HitTestPoints(state->anchors, documentPoint, toleranceDoc)) {
      // Begin a point-edit session on the committed selected path. The session
      // finalizes into one undoable command on mouse-up.
      beginPenSession(editor);
      activePath_ = pathElement;
      sessionUndoAnchor_ = pathElement;
      sessionCreatedPath_ = false;
      editingExistingPath_ = true;
      anchors_ = std::move(state->anchors);
      closed_ = state->closed;
      startPoint_ = anchors_.front().point;
      currentPoint_ = anchors_.back().point;
      rebuildActivePathData();
      beginPointDrag(*hit);
      // Alt/Option-click (no drag) toggles corner/smooth; a plain click (no
      // drag) on the open endpoint resumes drafting from it.
      pendingConvertAnchor_ = modifiers.option && hit->mode == DragMode::MoveAnchor;
      pendingResumeDraft_ = !pendingConvertAnchor_ && !closed_ && !modifiers.command &&
                            hit->mode == DragMode::MoveAnchor && hit->index + 1u == anchors_.size();
      return;
    }

    if (!modifiers.command) {
      if (std::optional<SegmentHit> segmentHit =
              HitTestSegments(state->anchors, state->closed, documentPoint, toleranceDoc)) {
        // Clicking ON a segment of the selected committed path inserts an
        // anchor at the hit point as a self-contained point-edit session.
        beginPenSession(editor);
        activePath_ = pathElement;
        sessionUndoAnchor_ = pathElement;
        sessionCreatedPath_ = false;
        editingExistingPath_ = true;
        anchors_ = std::move(state->anchors);
        closed_ = state->closed;
        startPoint_ = anchors_.front().point;
        currentPoint_ = anchors_.back().point;
        const std::size_t insertedIndex = insertAnchorAt(*segmentHit);
        rebuildActivePathData();
        commitActivePathData(editor);
        beginPointDrag(PointHit{.mode = DragMode::MoveAnchor, .index = insertedIndex});
        return;
      }
    }

    if (!state->closed && !modifiers.command) {
      continueSelectedPath(editor, pathElement, *state, documentPoint, modifiers);
      return;
    }
  }

  if (modifiers.command) {
    return;  // Point-edit only: never start a new path with Cmd/Ctrl held.
  }

  startNewPath(editor, documentPoint);
}

void PenTool::onMouseMove(EditorApp& editor, const Vector2d& documentPoint, bool buttonHeld) {
  onMouseMove(editor, documentPoint, buttonHeld, MouseModifiers{});
}

void PenTool::onMouseMove(EditorApp& editor, const Vector2d& documentPoint, bool buttonHeld,
                          MouseModifiers modifiers) {
  if (!buttonHeld || dragMode_ == DragMode::None) {
    return;
  }

  const bool changed = dragMode_ == DragMode::NewAnchorHandles
                           ? updateDraggedAnchor(documentPoint, /*breakSymmetry=*/modifiers.option,
                                                 /*constrainAngle=*/modifiers.shift)
                           : updatePointDrag(documentPoint, modifiers);
  if (changed) {
    commitActivePathData(editor);
    if (dragMode_ == DragMode::NewAnchorHandles) {
      draggedAnchorChanged_ = false;
    }
  }
}

void PenTool::onMouseUp(EditorApp& editor, const Vector2d& documentPoint) {
  switch (dragMode_) {
    case DragMode::None: return;

    case DragMode::NewAnchorHandles: {
      const bool changed = updateDraggedAnchor(documentPoint, /*breakSymmetry=*/false,
                                               /*constrainAngle=*/false);
      dragMode_ = DragMode::None;
      if (changed || draggedAnchorChanged_) {
        commitActivePathData(editor);
      }
      draggedAnchorChanged_ = false;
      if (pendingFinalizeOnRelease_) {
        // The drag was shaping the closing anchor's handles after a
        // close-path click; releasing the button ends the session.
        pendingFinalizeOnRelease_ = false;
        finalize(editor);
      }
      return;
    }

    case DragMode::MoveAnchor:
    case DragMode::MoveInHandle:
    case DragMode::MoveOutHandle: {
      const bool moved = draggedAnchorChanged_;
      dragMode_ = DragMode::None;
      draggedAnchorChanged_ = false;

      if (!moved && pendingResumeDraft_) {
        // Click on the open endpoint of a selected path: resume drafting from
        // it without adding an anchor. The point-edit session becomes a
        // regular append session (finalized on close/commit, not here).
        pendingResumeDraft_ = false;
        pendingRetractLastAnchor_ = false;
        editingExistingPath_ = false;
        currentPoint_ = anchors_.back().point;
        return;
      }

      if (!moved && pendingConvertAnchor_ && dragIndex_ < anchors_.size()) {
        convertAnchorSmoothness(dragIndex_);
        rebuildActivePathData();
        commitActivePathData(editor);
      } else if (!moved && pendingRetractLastAnchor_ && !anchors_.empty()) {
        // Retract only the OUTGOING handle: the segment into the anchor keeps
        // its curvature; the next segment starts straight.
        Anchor& last = anchors_.back();
        if (HasMeaningfulHandle(last.point, last.outHandle)) {
          last.outHandle.reset();
          rebuildActivePathData();
          commitActivePathData(editor);
        }
      }
      pendingResumeDraft_ = false;
      pendingRetractLastAnchor_ = false;
      pendingConvertAnchor_ = false;

      if (editingExistingPath_) {
        // Point edits on a committed path are self-contained sessions: one
        // undoable command per drag, then back to the not-drafting state.
        editingExistingPath_ = false;
        finalize(editor);
      }
      return;
    }
  }
}

void PenTool::cancel(EditorApp& editor) {
  // Escape/abort must leave the document AND the undo stack exactly as they
  // were before the pen session began. The session created exactly one new
  // `<path>` element (`activePath_`), so removing it restores the original
  // document without rewriting any existing geometry. DeleteElement is a soft
  // delete routed through the mutation seam: it detaches the element next flush
  // and records no undo entry, so the undo stack stays untouched.
  std::optional<svg::SVGElement> createdPath;
  if (sessionCreatedPath_ && activePath_.has_value()) {
    createdPath = *activePath_;
  }

  if (createdPath.has_value()) {
    editor.setSelection(std::nullopt);
    editor.applyMutation(EditorCommand::DeleteElementCommand(*createdPath));
  }

  cancel();
}

void PenTool::cancel() {
  activePath_.reset();
  anchors_.clear();
  startPoint_ = Vector2d::Zero();
  currentPoint_ = Vector2d::Zero();
  activePathData_.clear();
  closed_ = false;
  dragMode_ = DragMode::None;
  draggedAnchorChanged_ = false;
  dragIndex_ = 0;
  dragStartAnchor_ = Anchor{};
  dragStartAnchorSmooth_ = false;
  editingExistingPath_ = false;
  pendingRetractLastAnchor_ = false;
  pendingResumeDraft_ = false;
  pendingConvertAnchor_ = false;
  pendingFinalizeOnRelease_ = false;
  sessionBeforeSource_.reset();
  sessionUndoAnchor_.reset();
  sessionCreatedPath_ = false;
}

std::optional<PenTool::SelectedPathState> PenTool::stateForSelectedPath(
    const EditorApp& editor) const {
  const std::vector<svg::SVGElement>& selected = editor.selectedElements();
  if (selected.empty()) {
    return std::nullopt;
  }

  // §concurrent-dom: `isa`, `cast`, and `SVGPathElement::d()` each resolve the
  // selected element's EntityHandle, a raw ECS read. The live editor keeps the
  // document in ThreadingMode::ConcurrentDom, which requires the calling thread
  // to hold a read-access scope keyed to that element's own document state;
  // without it SVGElement's scoped-access release assert aborts the editor (the
  // crash the user hit when clicking the Pen tool with a path selected). Scope
  // the whole inspection off the selected element itself (the same idiom
  // SelectTool uses) so the access marker matches the entity being resolved.
  const std::optional<std::string> pathData = selected.front().withReadAccess(
      [&selected](svg::DocumentReadAccess&, EntityHandle) -> std::optional<std::string> {
        if (selected.size() != 1u || !selected.front().isa<svg::SVGPathElement>()) {
          return std::nullopt;
        }
        const svg::SVGPathElement pathElement = selected.front().cast<svg::SVGPathElement>();
        return std::string(std::string_view(pathElement.d()));
      });
  if (!pathData.has_value()) {
    return std::nullopt;
  }

  auto parsed = svg::parser::PathParser::Parse(*pathData);
  if (parsed.hasError() || !parsed.hasResult()) {
    return std::nullopt;
  }

  const Path& path = parsed.result();
  if (path.empty()) {
    return std::nullopt;
  }

  // Only single-contour paths are editable; multi-subpath editing is deferred
  // to a future Path Edit mode.
  std::vector<Anchor> anchors;
  bool closed = false;
  bool sawSubpath = false;
  for (const Path::Command& command : path.commands()) {
    if (closed && command.verb != Path::Verb::ClosePath) {
      return std::nullopt;  // Content after Z - multiple subpaths.
    }
    switch (command.verb) {
      case Path::Verb::MoveTo:
        if (sawSubpath) {
          return std::nullopt;  // Second MoveTo - multiple subpaths.
        }
        sawSubpath = true;
        anchors.push_back(Anchor{.point = path.points()[command.pointIndex]});
        break;
      case Path::Verb::LineTo:
        if (anchors.empty()) {
          return std::nullopt;
        }
        anchors.push_back(Anchor{.point = path.points()[command.pointIndex]});
        break;
      case Path::Verb::QuadTo: {
        if (anchors.empty()) {
          return std::nullopt;
        }
        const Vector2d start = anchors.back().point;
        const Vector2d control = path.points()[command.pointIndex];
        const Vector2d end = path.points()[command.pointIndex + 1u];
        anchors.back().outHandle = start + (control - start) * (2.0 / 3.0);
        anchors.push_back(Anchor{
            .point = end,
            .inHandle = end + (control - end) * (2.0 / 3.0),
        });
        break;
      }
      case Path::Verb::CurveTo:
        if (anchors.empty()) {
          return std::nullopt;
        }
        anchors.back().outHandle = path.points()[command.pointIndex];
        anchors.push_back(Anchor{
            .point = path.points()[command.pointIndex + 2u],
            .inHandle = path.points()[command.pointIndex + 1u],
        });
        break;
      case Path::Verb::ClosePath: closed = true; break;
    }
  }

  if (anchors.empty()) {
    return std::nullopt;
  }

  // A curved close serializes as an explicit cubic back into the first point
  // followed by `Z`, which parses as a duplicate anchor coincident with the
  // first. Merge it back so re-editing sees one anchor carrying both the
  // closing segment's incoming handle and the first segment's outgoing one.
  if (closed && anchors.size() > 1u && SamePoint(anchors.front().point, anchors.back().point)) {
    anchors.front().inHandle = anchors.back().inHandle;
    anchors.pop_back();
  }

  return SelectedPathState{.anchors = std::move(anchors), .closed = closed};
}

Vector2d PenTool::constrainedPoint(const Vector2d& documentPoint,
                                   const MouseModifiers& modifiers) const {
  if (!modifiers.shift || anchors_.empty()) {
    return documentPoint;
  }

  return ConstrainTo45Degrees(currentPoint_, documentPoint);
}

bool PenTool::shouldCloseAt(const Vector2d& documentPoint, const MouseModifiers& modifiers) const {
  if (anchors_.size() < 3u) {
    return false;
  }

  const double pixelsPerDocUnit = std::max(modifiers.pixelsPerDocUnit, 0.000001);
  const double toleranceDoc = kClosePointScreenTolerance / pixelsPerDocUnit;
  const double distance =
      std::hypot(documentPoint.x - startPoint_.x, documentPoint.y - startPoint_.y);
  return distance <= toleranceDoc;
}

std::string PenTool::serializePathData() const {
  if (anchors_.empty()) {
    return "";
  }

  std::string result = MoveToPathData(anchors_.front().point);
  for (std::size_t i = 1; i < anchors_.size(); ++i) {
    const Anchor& previous = anchors_[i - 1u];
    const Anchor& current = anchors_[i];
    const bool cubic = HasMeaningfulHandle(previous.point, previous.outHandle) ||
                       HasMeaningfulHandle(current.point, current.inHandle);
    result += " ";
    if (cubic) {
      result += "C ";
      result += FormatPoint(previous.outHandle.value_or(previous.point));
      result += " ";
      result += FormatPoint(current.inHandle.value_or(current.point));
      result += " ";
      result += FormatPoint(current.point);
    } else {
      result += "L ";
      result += FormatPoint(current.point);
    }
  }

  if (closed_) {
    // The closing segment (last anchor back to the first) carries curvature
    // through the last anchor's outgoing and the first anchor's incoming
    // handle; a bare `Z` closes with a straight line, so a curved close needs
    // the cubic emitted explicitly before it.
    const Anchor& last = anchors_.back();
    const Anchor& first = anchors_.front();
    if (anchors_.size() > 1u && (HasMeaningfulHandle(last.point, last.outHandle) ||
                                 HasMeaningfulHandle(first.point, first.inHandle))) {
      result += " C ";
      result += FormatPoint(last.outHandle.value_or(last.point));
      result += " ";
      result += FormatPoint(first.inHandle.value_or(first.point));
      result += " ";
      result += FormatPoint(first.point);
    }
    result += " Z";
  }
  return result;
}

void PenTool::startNewPath(EditorApp& editor, const Vector2d& documentPoint) {
  // Capture the source baseline BEFORE queueing the first insert so the whole
  // pen session can later collapse into one undoable command.
  beginPenSession(editor);

  svg::SVGDocument& document = editor.document().document();
  svg::SVGPathElement path = svg::SVGPathElement::Create(document);
  anchors_.clear();
  anchors_.push_back(Anchor{.point = documentPoint});
  closed_ = false;
  rebuildActivePathData();
  path.setAttribute("d", activePathData_);
  const ActivePaintStyle& paintStyle = editor.activePaintStyle();
  // Emit paint as a single inline `style` attribute (style="fill: ...; stroke: ...;
  // stroke-width: ...") rather than individual presentation attributes, so new
  // geometry round-trips through the source pane as CSS style like the rest of
  // the showcase content.
  path.setAttribute(
      "style", "fill: " + paintStyle.fill + "; stroke: " + paintStyle.stroke +
                   "; stroke-width: " + donner::detail::FormatNumberForSVG(paintStyle.strokeWidth));

  startPoint_ = documentPoint;
  currentPoint_ = documentPoint;
  activePath_ = path;
  sessionUndoAnchor_ = path;
  sessionCreatedPath_ = true;
  beginDragLastAnchor();

  svg::SVGElement parent = document.svgElement();
  editor.applyMutation(EditorCommand::InsertElementCommand(parent, path));
  editor.setSelection(path);
}

void PenTool::continueSelectedPath(EditorApp& editor, const svg::SVGPathElement& path,
                                   const SelectedPathState& state, const Vector2d& documentPoint,
                                   const MouseModifiers& modifiers) {
  beginPenSession(editor);

  activePath_ = path;
  sessionUndoAnchor_ = path;
  anchors_ = state.anchors;
  closed_ = false;
  startPoint_ = anchors_.front().point;
  currentPoint_ = anchors_.back().point;
  rebuildActivePathData();
  if (shouldCloseAt(documentPoint, modifiers)) {
    closePath(editor, documentPoint,
              kPointHitScreenTolerance / std::max(modifiers.pixelsPerDocUnit, 0.000001));
    return;
  }

  appendLine(editor, constrainedPoint(documentPoint, modifiers));
}

void PenTool::rebuildActivePathData() {
  activePathData_ = serializePathData();
}

void PenTool::beginDragLastAnchor() {
  dragMode_ = anchors_.empty() ? DragMode::None : DragMode::NewAnchorHandles;
  draggedAnchorChanged_ = false;
  dragIndex_ = anchors_.empty() ? 0u : anchors_.size() - 1u;
}

bool PenTool::updateDraggedAnchor(const Vector2d& documentPoint, bool breakSymmetry,
                                  bool constrainAngle) {
  if (dragMode_ != DragMode::NewAnchorHandles || dragIndex_ >= anchors_.size()) {
    return false;
  }

  // A close-path click that stays within its own hit tolerance is a plain
  // click, not a handle-shaping drag.
  if (pendingFinalizeOnRelease_ && !draggedAnchorChanged_ &&
      std::hypot(documentPoint.x - closeClickPoint_.x, documentPoint.y - closeClickPoint_.y) <=
          closeClickToleranceDoc_) {
    return false;
  }

  Anchor& anchor = anchors_[dragIndex_];
  const Vector2d target =
      constrainAngle ? ConstrainTo45Degrees(anchor.point, documentPoint) : documentPoint;
  const Vector2d delta = target - anchor.point;
  if (std::hypot(delta.x, delta.y) <= 1e-9) {
    return false;
  }

  // The outgoing handle always follows the mouse. With symmetry (default) the
  // incoming handle mirrors it through the anchor for a smooth node; Alt/Option
  // breaks that link so the incoming handle keeps whatever value it already had
  // (its previous segment's tangent) - the anchor becomes a corner with
  // mismatched handles. The first anchor only has an incoming segment to
  // mirror into once the contour is closed.
  std::optional<Vector2d> nextInHandle = anchor.inHandle;
  if ((dragIndex_ > 0u || closed_) && !breakSymmetry) {
    nextInHandle = anchor.point - delta;
  }

  const std::optional<Vector2d> nextOutHandle(anchor.point + delta);
  if (SameOptionalPoint(anchor.inHandle, nextInHandle) &&
      SameOptionalPoint(anchor.outHandle, nextOutHandle)) {
    return false;
  }

  anchor.inHandle = nextInHandle;
  anchor.outHandle = nextOutHandle;
  draggedAnchorChanged_ = true;
  rebuildActivePathData();
  return true;
}

std::optional<PenTool::PointHit> PenTool::HitTestPoints(const std::vector<Anchor>& anchors,
                                                        const Vector2d& documentPoint,
                                                        double toleranceDoc) {
  const auto distanceTo = [&documentPoint](const Vector2d& point) {
    return std::hypot(point.x - documentPoint.x, point.y - documentPoint.y);
  };

  std::optional<PointHit> bestAnchor;
  double bestAnchorDistance = toleranceDoc;
  std::optional<PointHit> bestHandle;
  double bestHandleDistance = toleranceDoc;

  for (std::size_t i = 0; i < anchors.size(); ++i) {
    const Anchor& anchor = anchors[i];
    if (const double distance = distanceTo(anchor.point); distance <= bestAnchorDistance) {
      bestAnchor = PointHit{.mode = DragMode::MoveAnchor, .index = i};
      bestAnchorDistance = distance;
    }

    // Zero-length handles coincide with their anchor and are not grab targets.
    if (HasMeaningfulHandle(anchor.point, anchor.inHandle)) {
      if (const double distance = distanceTo(*anchor.inHandle); distance <= bestHandleDistance) {
        bestHandle = PointHit{.mode = DragMode::MoveInHandle, .index = i};
        bestHandleDistance = distance;
      }
    }
    if (HasMeaningfulHandle(anchor.point, anchor.outHandle)) {
      if (const double distance = distanceTo(*anchor.outHandle); distance <= bestHandleDistance) {
        bestHandle = PointHit{.mode = DragMode::MoveOutHandle, .index = i};
        bestHandleDistance = distance;
      }
    }
  }

  // Anchors win ties; a handle must be strictly closer to take the hit.
  if (bestAnchor.has_value() && bestAnchorDistance <= bestHandleDistance) {
    return bestAnchor;
  }
  return bestHandle.has_value() ? bestHandle : bestAnchor;
}

void PenTool::beginPointDrag(const PointHit& hit) {
  dragMode_ = hit.mode;
  dragIndex_ = hit.index;
  dragStartAnchor_ = anchors_[hit.index];
  draggedAnchorChanged_ = false;
  pendingRetractLastAnchor_ = false;
  pendingResumeDraft_ = false;

  // Smoothness at drag start decides handle coupling for the whole drag: the
  // handles are collinear through the anchor (opposite directions).
  dragStartAnchorSmooth_ = false;
  if (HasMeaningfulHandle(dragStartAnchor_.point, dragStartAnchor_.inHandle) &&
      HasMeaningfulHandle(dragStartAnchor_.point, dragStartAnchor_.outHandle)) {
    const Vector2d outDir = (*dragStartAnchor_.outHandle - dragStartAnchor_.point).normalize();
    const Vector2d inDir = (dragStartAnchor_.point - *dragStartAnchor_.inHandle).normalize();
    dragStartAnchorSmooth_ = outDir.dot(inDir) >= 0.99;
  }
}

bool PenTool::updatePointDrag(const Vector2d& documentPoint, const MouseModifiers& modifiers) {
  if (dragIndex_ >= anchors_.size()) {
    return false;
  }

  Anchor& anchor = anchors_[dragIndex_];
  Anchor next = anchor;

  if (dragMode_ == DragMode::MoveAnchor) {
    const Vector2d target = modifiers.shift
                                ? ConstrainTo45Degrees(dragStartAnchor_.point, documentPoint)
                                : documentPoint;
    const Vector2d delta = target - dragStartAnchor_.point;
    next.point = dragStartAnchor_.point + delta;
    next.inHandle = dragStartAnchor_.inHandle.has_value()
                        ? std::make_optional(*dragStartAnchor_.inHandle + delta)
                        : std::nullopt;
    next.outHandle = dragStartAnchor_.outHandle.has_value()
                         ? std::make_optional(*dragStartAnchor_.outHandle + delta)
                         : std::nullopt;
  } else {
    const Vector2d target =
        modifiers.shift ? ConstrainTo45Degrees(anchor.point, documentPoint) : documentPoint;
    const Vector2d handleDirection = target - anchor.point;
    const double handleLength = std::hypot(handleDirection.x, handleDirection.y);
    if (dragMode_ == DragMode::MoveOutHandle) {
      next.outHandle = target;
    } else {
      next.inHandle = target;
    }

    // Aligned coupling: a smooth anchor keeps its handles collinear - the
    // opposite handle rotates with the drag while preserving its own length.
    // Alt/Option breaks the coupling so only the grabbed handle moves.
    if (dragStartAnchorSmooth_ && !modifiers.option && handleLength > 1e-9) {
      const Vector2d direction = handleDirection / handleLength;
      if (dragMode_ == DragMode::MoveOutHandle && dragStartAnchor_.inHandle.has_value()) {
        const double oppositeLength =
            std::hypot(dragStartAnchor_.inHandle->x - dragStartAnchor_.point.x,
                       dragStartAnchor_.inHandle->y - dragStartAnchor_.point.y);
        next.inHandle = anchor.point - direction * oppositeLength;
      } else if (dragMode_ == DragMode::MoveInHandle && dragStartAnchor_.outHandle.has_value()) {
        const double oppositeLength =
            std::hypot(dragStartAnchor_.outHandle->x - dragStartAnchor_.point.x,
                       dragStartAnchor_.outHandle->y - dragStartAnchor_.point.y);
        // The outgoing handle sits on the opposite side of the anchor from the
        // dragged incoming handle.
        next.outHandle = anchor.point - direction * oppositeLength;
      }
    }
  }

  if (SamePoint(next.point, anchor.point) && SameOptionalPoint(next.inHandle, anchor.inHandle) &&
      SameOptionalPoint(next.outHandle, anchor.outHandle)) {
    return false;
  }

  anchor = next;
  draggedAnchorChanged_ = true;
  rebuildActivePathData();
  return true;
}

void PenTool::commitActivePathData(EditorApp& editor) {
  if (!activePath_.has_value()) {
    return;
  }

  // Re-resolve the active path against the live document before writing.
  //
  // The editor's source-sync runs a self-writeback reparse on the frame after
  // every pen mutation (DOM change -> source mirror -> ReplaceDocument that
  // refreshes XML source ranges; see DocumentSyncController). That reparse swaps
  // `document_` for a fresh one, which makes the entity handle cached in
  // `activePath_` (and in `sessionUndoAnchor_`) stale: it names an element in
  // the *previous* document, whose XML node has no source range in the new
  // source store. Queueing `SetAttribute("d")` against that stale handle makes
  // `setElementAttribute` fail to locate the element's source span and
  // re-serialize the `<path>` at the end of the document -- AFTER the root
  // `</svg>` -- so the shape never renders (the user-reported Pen bug).
  //
  // The editor keeps the *selection* fresh across that reparse (it re-resolves
  // selection targets against the new document), and the pen keeps its active
  // path as the sole selection. Adopt that fresh handle so the geometry write
  // and undo anchor target a valid, source-backed element -- exactly the path
  // that already works for the per-click commits.
  if (editor.selectedElements().size() == 1u) {
    // §concurrent-dom: `isa`/`cast` resolve the element's EntityHandle (a raw ECS
    // read). The live editor keeps the document in ThreadingMode::ConcurrentDom,
    // which requires a read-access scope keyed to the element's own document
    // state; scope the inspection off the selected element itself (the same
    // idiom the rest of PenTool uses) so the access marker matches.
    const svg::SVGElement selected = editor.selectedElements().front();
    const std::optional<svg::SVGPathElement> freshPath = selected.withReadAccess(
        [&selected](svg::DocumentReadAccess&, EntityHandle) -> std::optional<svg::SVGPathElement> {
          if (!selected.isa<svg::SVGPathElement>()) {
            return std::nullopt;
          }
          return selected.cast<svg::SVGPathElement>();
        });
    if (freshPath.has_value()) {
      activePath_ = *freshPath;
      if (sessionUndoAnchor_.has_value()) {
        sessionUndoAnchor_ = *freshPath;
      }
    }
  }

  editor.applyMutation(EditorCommand::SetAttributeCommand(*activePath_, "d", activePathData_));
  editor.setSelection(*activePath_);
}

void PenTool::appendLine(EditorApp& editor, const Vector2d& documentPoint) {
  if (!activePath_.has_value()) {
    return;
  }

  anchors_.push_back(Anchor{.point = documentPoint});
  currentPoint_ = documentPoint;
  rebuildActivePathData();
  commitActivePathData(editor);
  beginDragLastAnchor();
}

void PenTool::closePath(EditorApp& editor, const Vector2d& clickPoint, double toleranceDoc) {
  if (!activePath_.has_value()) {
    return;
  }

  closed_ = true;
  rebuildActivePathData();
  commitActivePathData(editor);

  // Keep the session alive through the (still-held) close click: dragging
  // shapes the closing anchor's mirrored handles, and mouse-up finalizes.
  // A click that never leaves the hit tolerance around its own down-point
  // finalizes with the anchor untouched (the close click may land up to the
  // tolerance away from the exact anchor, which must not read as a drag).
  dragMode_ = DragMode::NewAnchorHandles;
  dragIndex_ = 0;
  draggedAnchorChanged_ = false;
  pendingFinalizeOnRelease_ = true;
  closeClickPoint_ = clickPoint;
  closeClickToleranceDoc_ = toleranceDoc;
}

std::optional<PenTool::SegmentHit> PenTool::HitTestSegments(const std::vector<Anchor>& anchors,
                                                            bool closed,
                                                            const Vector2d& documentPoint,
                                                            double toleranceDoc) {
  if (anchors.size() < 2u) {
    return std::nullopt;
  }

  const auto cubicPoint = [](const Vector2d& p0, const Vector2d& p1, const Vector2d& p2,
                             const Vector2d& p3, double t) {
    const double u = 1.0 - t;
    return p0 * (u * u * u) + p1 * (3.0 * u * u * t) + p2 * (3.0 * u * t * t) + p3 * (t * t * t);
  };
  const auto distanceSquared = [&](const Vector2d& point) {
    const Vector2d delta = point - documentPoint;
    return delta.x * delta.x + delta.y * delta.y;
  };

  std::optional<SegmentHit> best;
  double bestDistanceSquared = toleranceDoc * toleranceDoc;
  const std::size_t segmentCount = closed ? anchors.size() : anchors.size() - 1u;
  for (std::size_t i = 0; i < segmentCount; ++i) {
    const Anchor& from = anchors[i];
    const Anchor& to = anchors[(i + 1u) % anchors.size()];
    const bool cubic = HasMeaningfulHandle(from.point, from.outHandle) ||
                       HasMeaningfulHandle(to.point, to.inHandle);
    if (!cubic) {
      // Straight segment: project the click onto it.
      const Vector2d direction = to.point - from.point;
      const double lengthSquared = direction.x * direction.x + direction.y * direction.y;
      if (lengthSquared <= 1e-12) {
        continue;
      }
      const double t =
          std::clamp(((documentPoint - from.point).dot(direction)) / lengthSquared, 0.0, 1.0);
      const double distSq = distanceSquared(from.point + direction * t);
      if (distSq < bestDistanceSquared) {
        bestDistanceSquared = distSq;
        best = SegmentHit{.index = i, .t = t};
      }
      continue;
    }

    // Cubic segment: coarse-sample, then refine around the best sample.
    const Vector2d p0 = from.point;
    const Vector2d p1 = from.outHandle.value_or(from.point);
    const Vector2d p2 = to.inHandle.value_or(to.point);
    const Vector2d p3 = to.point;
    constexpr int kSamples = 64;
    double coarseT = 0.0;
    double coarseDistSq = std::numeric_limits<double>::max();
    for (int s = 0; s <= kSamples; ++s) {
      const double t = static_cast<double>(s) / kSamples;
      const double distSq = distanceSquared(cubicPoint(p0, p1, p2, p3, t));
      if (distSq < coarseDistSq) {
        coarseDistSq = distSq;
        coarseT = t;
      }
    }
    double lo = std::max(0.0, coarseT - 1.0 / kSamples);
    double hi = std::min(1.0, coarseT + 1.0 / kSamples);
    for (int iteration = 0; iteration < 24; ++iteration) {
      const double t1 = lo + (hi - lo) / 3.0;
      const double t2 = hi - (hi - lo) / 3.0;
      if (distanceSquared(cubicPoint(p0, p1, p2, p3, t1)) <
          distanceSquared(cubicPoint(p0, p1, p2, p3, t2))) {
        hi = t2;
      } else {
        lo = t1;
      }
    }
    const double refinedT = (lo + hi) * 0.5;
    const double refinedDistSq = distanceSquared(cubicPoint(p0, p1, p2, p3, refinedT));
    if (refinedDistSq < bestDistanceSquared) {
      bestDistanceSquared = refinedDistSq;
      best = SegmentHit{.index = i, .t = refinedT};
    }
  }
  return best;
}

std::size_t PenTool::insertAnchorAt(const SegmentHit& hit) {
  const std::size_t fromIndex = hit.index;
  const std::size_t toIndex = (hit.index + 1u) % anchors_.size();
  Anchor& from = anchors_[fromIndex];
  Anchor& to = anchors_[toIndex];
  const bool cubic =
      HasMeaningfulHandle(from.point, from.outHandle) || HasMeaningfulHandle(to.point, to.inHandle);

  Anchor inserted;
  if (cubic) {
    // De Casteljau split at t: the two halves reproduce the original curve
    // exactly, so inserting the anchor does not change the rendered shape.
    const double t = hit.t;
    const Vector2d p0 = from.point;
    const Vector2d p1 = from.outHandle.value_or(from.point);
    const Vector2d p2 = to.inHandle.value_or(to.point);
    const Vector2d p3 = to.point;
    const auto lerp = [](const Vector2d& a, const Vector2d& b, double s) {
      return a + (b - a) * s;
    };
    const Vector2d q0 = lerp(p0, p1, t);
    const Vector2d q1 = lerp(p1, p2, t);
    const Vector2d q2 = lerp(p2, p3, t);
    const Vector2d r0 = lerp(q0, q1, t);
    const Vector2d r1 = lerp(q1, q2, t);
    inserted.point = lerp(r0, r1, t);
    inserted.inHandle = r0;
    inserted.outHandle = r1;
    from.outHandle = q0;
    to.inHandle = q2;
  } else {
    inserted.point = from.point + (to.point - from.point) * hit.t;
  }

  const std::size_t insertedIndex = hit.index + 1u;
  anchors_.insert(anchors_.begin() + static_cast<std::ptrdiff_t>(insertedIndex),
                  std::move(inserted));
  return insertedIndex;
}

std::optional<Path> PenTool::previewSegmentPath(const Vector2d& documentPoint,
                                                const MouseModifiers& modifiers) const {
  if (!activePath_.has_value() || editingExistingPath_ || dragMode_ != DragMode::None ||
      anchors_.empty() || closed_) {
    return std::nullopt;
  }

  const Anchor& last = anchors_.back();
  const Vector2d target = shouldCloseAt(documentPoint, modifiers)
                              ? startPoint_
                              : constrainedPoint(documentPoint, modifiers);
  PathBuilder builder;
  builder.moveTo(last.point);
  if (HasMeaningfulHandle(last.point, last.outHandle)) {
    builder.curveTo(*last.outHandle, target, target);
  } else {
    builder.lineTo(target);
  }
  return builder.build();
}

bool PenTool::wouldCloseAt(const Vector2d& documentPoint, const MouseModifiers& modifiers) const {
  return activePath_.has_value() && !editingExistingPath_ && !closed_ &&
         shouldCloseAt(documentPoint, modifiers);
}

PenTool::PenHoverIntent PenTool::hoverIntentAt(const EditorApp& editor,
                                               const Vector2d& documentPoint,
                                               const MouseModifiers& modifiers) const {
  const double toleranceDoc =
      kPointHitScreenTolerance / std::max(modifiers.pixelsPerDocUnit, 0.000001);

  if (activePath_.has_value()) {
    // A drag or an existing-path point-edit session already owns the pointer;
    // no contextual hint until it ends.
    if (editingExistingPath_ || dragMode_ != DragMode::None) {
      return PenHoverIntent::None;
    }
    // Precedence follows onMouseDown: close wins, then an existing anchor, then
    // a segment.
    if (wouldCloseAt(documentPoint, modifiers)) {
      return PenHoverIntent::CloseContour;
    }
    if (const std::optional<PointHit> hit = HitTestPoints(anchors_, documentPoint, toleranceDoc);
        hit.has_value() && hit->mode == DragMode::MoveAnchor) {
      return PenHoverIntent::RemoveAnchor;
    }
    if (HitTestSegments(anchors_, closed_, documentPoint, toleranceDoc).has_value()) {
      return PenHoverIntent::AddAnchor;
    }
    return PenHoverIntent::None;
  }

  // Not drafting: a selected single-contour <path> is editable, so hovering its
  // anchors/segments previews the same add/remove gestures onMouseDown performs.
  if (const std::optional<SelectedPathState> state = stateForSelectedPath(editor);
      state.has_value()) {
    if (const std::optional<PointHit> hit =
            HitTestPoints(state->anchors, documentPoint, toleranceDoc);
        hit.has_value() && hit->mode == DragMode::MoveAnchor) {
      return PenHoverIntent::RemoveAnchor;
    }
    if (HitTestSegments(state->anchors, state->closed, documentPoint, toleranceDoc).has_value()) {
      return PenHoverIntent::AddAnchor;
    }
  }
  return PenHoverIntent::None;
}

void PenTool::convertAnchorSmoothness(std::size_t index) {
  Anchor& anchor = anchors_[index];
  const bool hasHandles = HasMeaningfulHandle(anchor.point, anchor.inHandle) ||
                          HasMeaningfulHandle(anchor.point, anchor.outHandle);
  if (hasHandles) {
    // Smooth (or partially curved) -> corner: drop both handles.
    anchor.inHandle.reset();
    anchor.outHandle.reset();
    return;
  }

  // Corner -> smooth: synthesize mirrored handles along the neighbor chord,
  // each one third of the distance to its neighbor (endpoints of an open
  // contour get only the handle facing their single neighbor). Closed
  // contours wrap around.
  const std::size_t count = anchors_.size();
  std::optional<Vector2d> previousPoint;
  std::optional<Vector2d> nextPoint;
  if (index > 0u) {
    previousPoint = anchors_[index - 1u].point;
  } else if (closed_ && count > 1u) {
    previousPoint = anchors_[count - 1u].point;
  }
  if (index + 1u < count) {
    nextPoint = anchors_[index + 1u].point;
  } else if (closed_ && count > 1u) {
    nextPoint = anchors_[0u].point;
  }
  if (!previousPoint.has_value() && !nextPoint.has_value()) {
    return;
  }

  const Vector2d chord = nextPoint.value_or(anchor.point) - previousPoint.value_or(anchor.point);
  const double chordLength = std::hypot(chord.x, chord.y);
  if (chordLength <= 1e-9) {
    return;
  }
  const Vector2d direction = chord / chordLength;

  if (previousPoint.has_value()) {
    const double inLength =
        std::hypot(anchor.point.x - previousPoint->x, anchor.point.y - previousPoint->y) / 3.0;
    anchor.inHandle = anchor.point - direction * inLength;
  }
  if (nextPoint.has_value()) {
    const double outLength =
        std::hypot(nextPoint->x - anchor.point.x, nextPoint->y - anchor.point.y) / 3.0;
    anchor.outHandle = anchor.point + direction * outLength;
  }
}

bool PenTool::removeLastAnchor(EditorApp& editor) {
  if (!activePath_.has_value() || editingExistingPath_ || dragMode_ != DragMode::None) {
    return false;
  }

  if (anchors_.size() <= 1u) {
    // Removing the only anchor leaves nothing to draft - discard the draft,
    // restoring the pre-pen document and undo stack.
    cancel(editor);
    return true;
  }

  anchors_.pop_back();
  currentPoint_ = anchors_.back().point;
  rebuildActivePathData();
  commitActivePathData(editor);
  return true;
}

bool PenTool::commitOpenPath(EditorApp& editor) {
  // Enter / double-click / tool-switch commit of an in-progress open path.
  // Requires at least one real segment; a lone anchor is not a path.
  if (!activePath_.has_value() || anchors_.size() < 2u) {
    return false;
  }

  // A drag may still be shaping the final anchor's handles - or an existing
  // point - when commit fires.
  if (dragMode_ != DragMode::None) {
    dragMode_ = DragMode::None;
    if (draggedAnchorChanged_) {
      commitActivePathData(editor);
    }
    draggedAnchorChanged_ = false;
  }

  // A point-edit session on a committed path must not rewrite the path's
  // closed/open state - only the edited points - and a commit that lands
  // mid-close-drag keeps the just-closed contour closed. Finalize as-is.
  if (!editingExistingPath_ && !pendingFinalizeOnRelease_) {
    closed_ = false;
  }
  pendingFinalizeOnRelease_ = false;
  rebuildActivePathData();
  commitActivePathData(editor);
  finalize(editor);
  return true;
}

void PenTool::beginPenSession(EditorApp& editor) {
  if (sessionBeforeSource_.has_value()) {
    return;  // Session already open - keep the original baseline.
  }
  if (editor.document().hasDocument() && editor.document().document().hasSourceStore()) {
    sessionBeforeSource_ = std::string(editor.document().document().source());
  }
}

void PenTool::finalize(EditorApp& editor) {
  // Defer one DocumentSource undo entry spanning the whole pen session
  // (baseline source -> finalized source) to the editor's next flushFrame().
  // The just-committed `SetAttribute` geometry is still queued; recording the
  // undo on the normal flush path keeps the source-sync writeback intact and
  // lets the entire pen session collapse into a single undoable command. Undo
  // therefore removes the whole pen path in one step, and redo restores it.
  if (sessionBeforeSource_.has_value() && sessionUndoAnchor_.has_value()) {
    editor.recordDocumentSourceUndoOnNextFlush("Pen path", *sessionUndoAnchor_,
                                               *sessionBeforeSource_);
  }

  // Clear local draft state without rolling back the document - the queued
  // geometry is the committed path. (cancel() with no editor only resets the
  // tool's own fields.)
  cancel();
}

}  // namespace donner::editor
