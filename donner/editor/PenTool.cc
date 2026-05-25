#include "donner/editor/PenTool.h"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <vector>

#include "donner/base/FormatNumber.h"
#include "donner/base/Path.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/EditorCommand.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/SVGSVGElement.h"
#include "donner/svg/parser/PathParser.h"

namespace donner::editor {

namespace {

constexpr double kClosePointScreenTolerance = 10.0;

std::string FormatPoint(const Vector2d& point) {
  return donner::detail::FormatNumberForSVG(point.x) + " " +
         donner::detail::FormatNumberForSVG(point.y);
}

std::string MoveToPathData(const Vector2d& point) {
  return "M " + FormatPoint(point);
}

bool HasMeaningfulHandle(const Vector2d& anchor, const std::optional<Vector2d>& handle) {
  return handle.has_value() && std::hypot(handle->x - anchor.x, handle->y - anchor.y) > 1e-9;
}

}  // namespace

void PenTool::onMouseDown(EditorApp& editor, const Vector2d& documentPoint,
                          MouseModifiers modifiers) {
  if (!editor.document().hasDocument()) {
    cancel();
    return;
  }

  if (!activePath_.has_value()) {
    if (std::optional<OpenPathState> state = openStateForSelectedPath(editor); state.has_value()) {
      continueSelectedPath(editor, editor.selectedElements().front().cast<svg::SVGPathElement>(),
                           *state, documentPoint, modifiers);
      return;
    }

    startNewPath(editor, documentPoint);
    return;
  }

  if (shouldCloseAt(documentPoint, modifiers)) {
    closePath(editor);
    return;
  }

  appendLine(editor, constrainedPoint(documentPoint, modifiers));
}

void PenTool::onMouseMove(EditorApp& editor, const Vector2d& documentPoint, bool buttonHeld) {
  (void)editor;
  if (!buttonHeld || !draggingAnchor_) {
    return;
  }

  updateDraggedAnchor(documentPoint);
}

void PenTool::onMouseUp(EditorApp& editor, const Vector2d& documentPoint) {
  if (!draggingAnchor_) {
    return;
  }

  updateDraggedAnchor(documentPoint);
  draggingAnchor_ = false;
  if (draggedAnchorChanged_) {
    commitActivePathData(editor);
  }
  draggedAnchorChanged_ = false;
}

void PenTool::cancel() {
  activePath_.reset();
  anchors_.clear();
  startPoint_ = Vector2d::Zero();
  currentPoint_ = Vector2d::Zero();
  activePathData_.clear();
  closed_ = false;
  draggingAnchor_ = false;
  draggedAnchorChanged_ = false;
  draggingAnchorIndex_ = 0;
}

std::optional<PenTool::OpenPathState> PenTool::openStateForSelectedPath(
    const EditorApp& editor) const {
  const std::vector<svg::SVGElement>& selected = editor.selectedElements();
  if (selected.size() != 1u || !selected.front().isa<svg::SVGPathElement>()) {
    return std::nullopt;
  }

  const svg::SVGPathElement pathElement = selected.front().cast<svg::SVGPathElement>();
  const std::string pathData(std::string_view(pathElement.d()));
  auto parsed = svg::parser::PathParser::Parse(pathData);
  if (parsed.hasError() || !parsed.hasResult()) {
    return std::nullopt;
  }

  const Path& path = parsed.result();
  if (path.empty()) {
    return std::nullopt;
  }

  std::vector<Anchor> anchors;
  bool subpathOpen = false;
  for (const Path::Command& command : path.commands()) {
    switch (command.verb) {
      case Path::Verb::MoveTo:
        anchors.clear();
        anchors.push_back(Anchor{.point = path.points()[command.pointIndex]});
        subpathOpen = true;
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
      case Path::Verb::ClosePath: subpathOpen = false; break;
    }
  }

  if (!subpathOpen || anchors.empty()) {
    return std::nullopt;
  }

  return OpenPathState{.anchors = std::move(anchors)};
}

Vector2d PenTool::constrainedPoint(const Vector2d& documentPoint,
                                   const MouseModifiers& modifiers) const {
  if (!modifiers.shift || anchors_.empty()) {
    return documentPoint;
  }

  const double dx = documentPoint.x - currentPoint_.x;
  const double dy = documentPoint.y - currentPoint_.y;
  if (std::abs(dx) >= std::abs(dy)) {
    return Vector2d(documentPoint.x, currentPoint_.y);
  }
  return Vector2d(currentPoint_.x, documentPoint.y);
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
    result += " Z";
  }
  return result;
}

std::vector<PenTool::PreviewSegment> PenTool::previewSegments() const {
  std::vector<PreviewSegment> result;
  if (anchors_.size() < 2u) {
    return result;
  }

  result.reserve(anchors_.size() - 1u);
  for (std::size_t i = 1; i < anchors_.size(); ++i) {
    const Anchor& previous = anchors_[i - 1u];
    const Anchor& current = anchors_[i];
    const bool cubic = HasMeaningfulHandle(previous.point, previous.outHandle) ||
                       HasMeaningfulHandle(current.point, current.inHandle);
    result.push_back(PreviewSegment{
        .start = previous.point,
        .control1 = previous.outHandle.value_or(previous.point),
        .control2 = current.inHandle.value_or(current.point),
        .end = current.point,
        .cubic = cubic,
    });
  }
  return result;
}

std::vector<Vector2d> PenTool::previewAnchors() const {
  std::vector<Vector2d> result;
  result.reserve(anchors_.size());
  for (const Anchor& anchor : anchors_) {
    result.push_back(anchor.point);
  }
  return result;
}

std::vector<PenTool::PreviewHandleLine> PenTool::previewHandleLines() const {
  std::vector<PreviewHandleLine> result;
  for (const Anchor& anchor : anchors_) {
    if (HasMeaningfulHandle(anchor.point, anchor.inHandle)) {
      result.push_back(PreviewHandleLine{.start = anchor.point, .end = *anchor.inHandle});
    }
    if (HasMeaningfulHandle(anchor.point, anchor.outHandle)) {
      result.push_back(PreviewHandleLine{.start = anchor.point, .end = *anchor.outHandle});
    }
  }
  return result;
}

void PenTool::startNewPath(EditorApp& editor, const Vector2d& documentPoint) {
  svg::SVGDocument& document = editor.document().document();
  svg::SVGPathElement path = svg::SVGPathElement::Create(document);
  anchors_.clear();
  anchors_.push_back(Anchor{.point = documentPoint});
  closed_ = false;
  rebuildActivePathData();
  path.setAttribute("d", activePathData_);
  const ActivePaintStyle& paintStyle = editor.activePaintStyle();
  path.setAttribute("fill", paintStyle.fill);
  path.setAttribute("stroke", paintStyle.stroke);
  path.setAttribute("stroke-width", donner::detail::FormatNumberForSVG(paintStyle.strokeWidth));

  startPoint_ = documentPoint;
  currentPoint_ = documentPoint;
  activePath_ = path;
  beginDragLastAnchor();

  svg::SVGElement parent = document.svgElement();
  editor.applyMutation(EditorCommand::InsertElementCommand(parent, path));
  editor.setSelection(path);
}

void PenTool::continueSelectedPath(EditorApp& editor, const svg::SVGPathElement& path,
                                   const OpenPathState& state, const Vector2d& documentPoint,
                                   const MouseModifiers& modifiers) {
  activePath_ = path;
  anchors_ = state.anchors;
  closed_ = false;
  startPoint_ = anchors_.front().point;
  currentPoint_ = anchors_.back().point;
  rebuildActivePathData();
  if (shouldCloseAt(documentPoint, modifiers)) {
    closePath(editor);
    return;
  }

  appendLine(editor, constrainedPoint(documentPoint, modifiers));
}

void PenTool::rebuildActivePathData() {
  activePathData_ = serializePathData();
}

void PenTool::beginDragLastAnchor() {
  draggingAnchor_ = !anchors_.empty();
  draggedAnchorChanged_ = false;
  draggingAnchorIndex_ = anchors_.empty() ? 0u : anchors_.size() - 1u;
}

void PenTool::updateDraggedAnchor(const Vector2d& documentPoint) {
  if (!draggingAnchor_ || draggingAnchorIndex_ >= anchors_.size()) {
    return;
  }

  Anchor& anchor = anchors_[draggingAnchorIndex_];
  const Vector2d delta = documentPoint - anchor.point;
  if (std::hypot(delta.x, delta.y) <= 1e-9) {
    return;
  }

  if (draggingAnchorIndex_ > 0u) {
    anchor.inHandle = anchor.point - delta;
  }
  anchor.outHandle = anchor.point + delta;
  draggedAnchorChanged_ = true;
  rebuildActivePathData();
}

void PenTool::commitActivePathData(EditorApp& editor) {
  if (!activePath_.has_value()) {
    return;
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

void PenTool::closePath(EditorApp& editor) {
  if (!activePath_.has_value()) {
    return;
  }

  closed_ = true;
  rebuildActivePathData();
  commitActivePathData(editor);
  cancel();
}

}  // namespace donner::editor
