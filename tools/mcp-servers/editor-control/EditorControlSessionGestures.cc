/// @file
/// EditorControlSession gesture tools: selector-driven selection, Layers-panel
/// row buttons, tool/style state, pen path authoring, and synthesized
/// click/drag and scale/rotate transform gestures.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/editor/EditorCommand.h"
#include "donner/editor/LayersPanel.h"
#include "donner/editor/SelectionTransformHandles.h"
#include "donner/editor/Tool.h"
#include "donner/editor/ViewportState.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"
#include "nlohmann/json.hpp"
#include "tools/mcp-servers/editor-control/EditorControlSessionInternal.h"

namespace donner::editor::mcp {

namespace {

using nlohmann::json;

std::string_view SelectionTransformCornerName(SelectionTransformCorner corner) {
  switch (corner) {
    case SelectionTransformCorner::TopLeft: return "top_left";
    case SelectionTransformCorner::TopRight: return "top_right";
    case SelectionTransformCorner::BottomRight: return "bottom_right";
    case SelectionTransformCorner::BottomLeft: return "bottom_left";
  }
  return "top_left";
}

std::optional<SelectionTransformCorner> ParseSelectionTransformCorner(std::string_view corner) {
  if (corner == "top_left" || corner == "top-left" || corner == "tl") {
    return SelectionTransformCorner::TopLeft;
  }
  if (corner == "top_right" || corner == "top-right" || corner == "tr") {
    return SelectionTransformCorner::TopRight;
  }
  if (corner == "bottom_right" || corner == "bottom-right" || corner == "br") {
    return SelectionTransformCorner::BottomRight;
  }
  if (corner == "bottom_left" || corner == "bottom-left" || corner == "bl") {
    return SelectionTransformCorner::BottomLeft;
  }
  return std::nullopt;
}

std::string_view ActiveGestureKindName(SelectTool::ActiveGestureKind kind) {
  switch (kind) {
    case SelectTool::ActiveGestureKind::Move: return "move";
    case SelectTool::ActiveGestureKind::Resize: return "resize";
    case SelectTool::ActiveGestureKind::Rotate: return "rotate";
  }
  return "move";
}

json ActiveGesturePreviewJson(
    const std::optional<SelectTool::ActiveGesturePreview>& activeGesturePreview) {
  if (!activeGesturePreview.has_value()) {
    return nullptr;
  }

  return json{
      {"kind", ActiveGestureKindName(activeGesturePreview->kind)},
      {"corner", SelectionTransformCornerName(activeGesturePreview->corner)},
      {"start_bounds_doc", BoxToJson(activeGesturePreview->startBoundsDoc)},
      {"document_from_start_document",
       TransformToJson(activeGesturePreview->documentFromStartDocument)},
      {"current_document_delta", VectorToJson(activeGesturePreview->currentDocumentDelta)},
      {"has_moved", activeGesturePreview->hasMoved},
  };
}

int ModifierMaskFromMouseModifiers(const MouseModifiers& modifiers) {
  int mask = 0;
  if (modifiers.shift) {
    mask |= 1 << 1;
  }
  if (modifiers.option) {
    mask |= 1 << 2;
  }
  return mask;
}

Vector2d RotateHandlePointForCorner(const Box2d& bounds, SelectionTransformCorner corner,
                                    double pixelsPerDocUnit) {
  constexpr double kRotateHandleAxisOffsetPixels = 14.0;
  const double scale = std::max(std::abs(pixelsPerDocUnit), 1e-9);
  const double axisOffsetDoc = kRotateHandleAxisOffsetPixels / scale;
  Vector2d direction = Vector2d::Zero();
  switch (corner) {
    case SelectionTransformCorner::TopLeft: direction = Vector2d(-1.0, -1.0); break;
    case SelectionTransformCorner::TopRight: direction = Vector2d(1.0, -1.0); break;
    case SelectionTransformCorner::BottomRight: direction = Vector2d(1.0, 1.0); break;
    case SelectionTransformCorner::BottomLeft: direction = Vector2d(-1.0, 1.0); break;
  }
  return SelectionTransformCornerPoint(bounds, corner) + direction * axisOffsetDoc;
}

}  // namespace

ToolCallResult EditorControlSession::selectBySelector(const json& arguments) {
  std::string error;
  if (!ensureDocumentLoaded(&error)) {
    return MakeErrorResult(error);
  }
  (void)drainPendingWritebacks();

  std::string selector;
  if (!ReadRequiredString(arguments, "selector", &selector, &error)) {
    return MakeErrorResult(error);
  }
  std::optional<svg::SVGElement> element = querySelector(selector);
  if (!element.has_value()) {
    return MakeErrorResult("selector did not match an element: " + selector);
  }

  app_.setSelection(*element);
  app_.flushFrame();

  ToolCallResult out;
  out.body = json{
      {"ok", true},
      {"selector", selector},
      {"selection", selectedElementJson()},
  };

  if (auto bounds = elementWorldBounds(*element); bounds.has_value()) {
    out.body["selection_bounds_doc"] = BoxToJson(*bounds);
  } else {
    out.body["selection_bounds_doc"] = nullptr;
  }

  bool shouldRender = true;
  CaptureOptions capture;
  if (!ReadOptionalBool(arguments, "render", true, &shouldRender, &error) ||
      !ReadCaptureOptions(arguments, false, &capture, &error)) {
    return MakeErrorResult(error);
  }

  if (shouldRender) {
    std::vector<CapturedRenderResult> renderResults;
    if (!renderCurrentFrame(&renderResults, &error)) {
      return MakeErrorResult(error);
    }
    out.body["render_stages"] =
        RenderResultsJson(renderResults, &out, capture, "select_by_selector");
  }
  return out;
}

ToolCallResult EditorControlSession::clickLayerButton(const json& arguments) {
  std::string error;
  if (!ensureDocumentLoaded(&error)) {
    return MakeErrorResult(error);
  }
  (void)drainPendingWritebacks();

  std::string selector;
  std::string button;
  bool renderAfterClick = true;
  bool includeDisplayBeforeRender = true;
  CaptureOptions capture;
  if (!ReadRequiredString(arguments, "selector", &selector, &error) ||
      !ReadOptionalString(arguments, "button", "visibility", &button, &error) ||
      !ReadOptionalBool(arguments, "render_after_click", true, &renderAfterClick, &error) ||
      !ReadOptionalBool(arguments, "include_display_before_render", true,
                        &includeDisplayBeforeRender, &error) ||
      !ReadCaptureOptions(arguments, true, &capture, &error)) {
    return MakeErrorResult(error);
  }
  if (button != "visibility" && button != "lock") {
    return MakeErrorResult("button must be 'visibility' or 'lock'");
  }

  std::optional<svg::SVGElement> element = querySelector(selector);
  if (!element.has_value()) {
    return MakeErrorResult("selector did not match an element: " + selector);
  }

  LayersPanel layersPanel;
  layersPanel.refreshSnapshot(app_);
  const std::vector<LayerTreeRow>& rows = layersPanel.rows();
  const auto rowIt =
      std::ranges::find_if(rows, [&](const LayerTreeRow& row) { return row.element == *element; });
  if (rowIt == rows.end()) {
    return MakeErrorResult("selector did not map to a visible Layers-panel row: " + selector);
  }

  const std::size_t rowIndex = static_cast<std::size_t>(std::distance(rows.begin(), rowIt));
  const std::uint64_t stableId = rowIt->stableId;
  const bool wasVisible = rowIt->isVisible;
  const bool wasLocked = rowIt->isLocked;
  const std::string displayName = rowIt->displayName;

  if (button == "visibility") {
    layersPanel.handleEyeClick(app_, rowIndex);
  } else {
    layersPanel.handleLockClick(app_, rowIndex);
  }

  const bool mutationQueued = layersPanel.consumeQueuedMutation();
  if (mutationQueued) {
    app_.flushFrame();
    if (app_.document().document().hasSourceStore()) {
      const std::string sourceAfter(app_.document().document().source());
      if (sourceAfter != currentSourceText_) {
        currentSourceText_ = sourceAfter;
        ++sourceRevision_;
        loadedSourceRevision_ = sourceRevision_;
        lastParseError_.reset();
      }
    }
  }

  layersPanel.refreshSnapshot(app_);
  const auto afterIt = std::ranges::find_if(
      layersPanel.rows(), [stableId](const LayerTreeRow& row) { return row.stableId == stableId; });

  ToolCallResult out;
  out.body = json{
      {"ok", true},
      {"selector", selector},
      {"button", button},
      {"row_index", rowIndex},
      {"row_stable_id", stableId},
      {"display_name", displayName},
      {"mutation_queued", mutationQueued},
      {"before", {{"visible", wasVisible}, {"locked", wasLocked}}},
      {"after", afterIt == layersPanel.rows().end()
                    ? json(nullptr)
                    : json{{"visible", afterIt->isVisible}, {"locked", afterIt->isLocked}}},
      {"source", sourceStateJson()},
  };

  const DisplayFrameSnapshot displayBeforeRender = currentDisplayFrame();
  out.body["display_before_render"] = DisplayFrameJson(displayBeforeRender);
  if (includeDisplayBeforeRender) {
    const std::optional<svg::RendererBitmap> displayBitmap =
        composeDisplayFrameBitmap(displayBeforeRender);
    if (displayBitmap.has_value()) {
      json bitmapJson = BitmapSummary(*displayBitmap);
      AttachBitmapImage(&out, "click_layer_button/display_before_render", *displayBitmap,
                        capture.embedPngBase64, &bitmapJson);
      out.body["display_before_render_bitmap"] = std::move(bitmapJson);
    } else {
      out.body["display_before_render_bitmap"] = nullptr;
    }
  }

  if (renderAfterClick) {
    std::vector<CapturedRenderResult> renderResults;
    if (!renderCurrentFrame(&renderResults, &error)) {
      return MakeErrorResult(error);
    }
    out.body["render_stages"] =
        RenderResultsJson(renderResults, &out, capture, "click_layer_button");
    if (capture.includeDisplayFrame) {
      const DisplayFrameSnapshot displayAfterRender =
          renderResults.empty() ? displayBeforeRender : renderResults.back().displayFrame;
      out.body["display_after_render"] = DisplayFrameJson(displayAfterRender);

      const std::optional<svg::RendererBitmap> displayBitmap =
          composeDisplayFrameBitmap(displayAfterRender);
      if (displayBitmap.has_value()) {
        json bitmapJson = BitmapSummary(*displayBitmap);
        AttachBitmapImage(&out, "click_layer_button/display_after_render", *displayBitmap,
                          capture.embedPngBase64, &bitmapJson);
        out.body["display_after_render_bitmap"] = std::move(bitmapJson);
      } else {
        out.body["display_after_render_bitmap"] = nullptr;
      }
    }
  }

  out.body["attached_image_count"] = out.images.size();
  return out;
}

ToolCallResult EditorControlSession::setActiveTool(const json& arguments) {
  std::string error;
  if (!ensureDocumentLoaded(&error)) {
    return MakeErrorResult(error);
  }
  (void)drainPendingWritebacks();

  std::string tool;
  if (!ReadRequiredString(arguments, "tool", &tool, &error)) {
    return MakeErrorResult(error);
  }
  if (!setActiveToolForReplay(tool, &error)) {
    return MakeErrorResult(error);
  }

  repro::ReproAction action;
  action.kind = repro::ReproAction::Kind::SetActiveTool;
  action.tool = tool;
  appendRnrActionFrame(std::move(action));

  (void)app_.flushFrame();
  syncSourceTextFromDocumentIfChanged();

  ToolCallResult out;
  out.body = json{
      {"ok", true},
      {"tool", tool},
      {"source", sourceStateJson()},
      {"selection", selectedElementJson()},
  };
  return out;
}

ToolCallResult EditorControlSession::setStyleProperty(const json& arguments) {
  std::string error;
  if (!ensureDocumentLoaded(&error)) {
    return MakeErrorResult(error);
  }
  (void)drainPendingWritebacks();

  std::string propertyName;
  std::string propertyValue;
  bool renderAfterSet = true;
  CaptureOptions capture;
  if (!ReadRequiredString(arguments, "property", &propertyName, &error) ||
      !ReadRequiredString(arguments, "value", &propertyValue, &error) ||
      !ReadOptionalBool(arguments, "render_after_set", true, &renderAfterSet, &error) ||
      !ReadCaptureOptions(arguments, false, &capture, &error)) {
    return MakeErrorResult(error);
  }

  repro::ReproAction action;
  action.kind = repro::ReproAction::Kind::SetStyleProperty;
  action.propertyName = propertyName;
  action.propertyValue = propertyValue;
  appendRnrActionFrame(std::move(action));

  const bool queuedSelectionMutation = applyStylePropertyForReplay(propertyName, propertyValue);
  const bool flushed = app_.flushFrame();
  syncSourceTextFromDocumentIfChanged();

  ToolCallResult out;
  out.body = json{
      {"ok", true},
      {"property", propertyName},
      {"value", propertyValue},
      {"queued_selection_mutation", queuedSelectionMutation},
      {"flushed_mutation", flushed},
      {"source", sourceStateJson()},
      {"selection", selectedElementJson()},
  };

  if (renderAfterSet) {
    std::vector<CapturedRenderResult> renderResults;
    if (!renderCurrentFrame(&renderResults, &error)) {
      return MakeErrorResult(error);
    }
    out.body["render_stages"] =
        RenderResultsJson(renderResults, &out, capture, "set_style_property");
  }
  out.body["attached_image_count"] = out.images.size();
  return out;
}

ToolCallResult EditorControlSession::penPath(const json& arguments) {
  std::string error;
  if (!ensureDocumentLoaded(&error)) {
    return MakeErrorResult(error);
  }
  (void)drainPendingWritebacks();

  const auto pointsIt = arguments.find("points");
  if (pointsIt == arguments.end() || !pointsIt->is_array()) {
    return MakeErrorResult("missing array argument: points");
  }

  std::vector<Vector2d> points;
  points.reserve(pointsIt->size());
  int pointIndex = 0;
  for (const json& pointJson : *pointsIt) {
    if (!pointJson.is_object()) {
      return MakeErrorResult("point " + std::to_string(pointIndex) + " must be an object");
    }
    const auto xIt = pointJson.find("x");
    const auto yIt = pointJson.find("y");
    if (xIt == pointJson.end() || yIt == pointJson.end() || !xIt->is_number() ||
        !yIt->is_number()) {
      return MakeErrorResult("point " + std::to_string(pointIndex) + " requires numeric x and y");
    }
    points.push_back(Vector2d(xIt->get<double>(), yIt->get<double>()));
    ++pointIndex;
  }
  if (points.empty()) {
    return MakeErrorResult("points must contain at least one point");
  }

  bool close = false;
  bool commitOpen = true;
  bool renderAfterPath = true;
  CaptureOptions capture;
  if (!ReadOptionalBool(arguments, "close", false, &close, &error) ||
      !ReadOptionalBool(arguments, "commit_open", true, &commitOpen, &error) ||
      !ReadOptionalBool(arguments, "render_after_path", true, &renderAfterPath, &error) ||
      !ReadCaptureOptions(arguments, false, &capture, &error)) {
    return MakeErrorResult(error);
  }

  if (!setActiveToolForReplay("pen", &error)) {
    return MakeErrorResult(error);
  }
  repro::ReproAction toolAction;
  toolAction.kind = repro::ReproAction::Kind::SetActiveTool;
  toolAction.tool = "pen";
  appendRnrActionFrame(std::move(toolAction));

  const auto clickPoint = [&](const Vector2d& point) {
    MouseModifiers modifiers;
    modifiers.pixelsPerDocUnit = currentReproViewport().zoom;
    penTool_.onMouseDown(app_, point, modifiers);
    repro::ReproEvent mouseDown;
    mouseDown.kind = repro::ReproEvent::Kind::MouseDown;
    mouseDown.mouseButton = 0;
    appendRnrFrame(point, /*mouseButtonMask=*/1, /*modifierMask=*/0,
                   std::vector<repro::ReproEvent>{mouseDown});

    penTool_.onMouseUp(app_, point);
    repro::ReproEvent mouseUp;
    mouseUp.kind = repro::ReproEvent::Kind::MouseUp;
    mouseUp.mouseButton = 0;
    appendRnrFrame(point, /*mouseButtonMask=*/0, /*modifierMask=*/0,
                   std::vector<repro::ReproEvent>{mouseUp});

    (void)app_.flushFrame();
    syncSourceTextFromDocumentIfChanged();
  };

  for (const Vector2d& point : points) {
    clickPoint(point);
  }
  if (close && points.size() >= 3u) {
    clickPoint(points.front());
  } else if (commitOpen) {
    repro::ReproAction commitAction;
    commitAction.kind = repro::ReproAction::Kind::CommitPenPath;
    appendRnrActionFrame(std::move(commitAction));
    std::ignore = penTool_.commitOpenPath(app_);
    (void)app_.flushFrame();
    syncSourceTextFromDocumentIfChanged();
  }

  ToolCallResult out;
  out.body = json{
      {"ok", true},
      {"point_count", points.size()},
      {"closed", close && points.size() >= 3u},
      {"committed_open", !close && commitOpen},
      {"source", sourceStateJson()},
      {"selection", selectedElementJson()},
  };

  if (renderAfterPath) {
    std::vector<CapturedRenderResult> renderResults;
    if (!renderCurrentFrame(&renderResults, &error)) {
      return MakeErrorResult(error);
    }
    out.body["render_stages"] = RenderResultsJson(renderResults, &out, capture, "pen_path");
  }
  out.body["attached_image_count"] = out.images.size();
  return out;
}

ToolCallResult EditorControlSession::dragSelector(const json& arguments) {
  std::string error;
  if (!ensureDocumentLoaded(&error)) {
    return MakeErrorResult(error);
  }
  (void)drainPendingWritebacks();

  std::string selector;
  double dx = 16.0;
  double dy = 0.0;
  int requestedFrames = 8;
  std::string selectionMode;
  bool renderMouseDown = true;
  bool release = true;
  CaptureOptions capture;
  if (!ReadRequiredString(arguments, "selector", &selector, &error) ||
      !ReadOptionalDouble(arguments, "delta_x", 16.0, &dx, &error) ||
      !ReadOptionalDouble(arguments, "delta_y", 0.0, &dy, &error) ||
      !ReadOptionalInt(arguments, "frames", 8, &requestedFrames, &error) ||
      !ReadOptionalString(arguments, "selection_mode", "direct", &selectionMode, &error) ||
      !ReadOptionalBool(arguments, "render_mouse_down", true, &renderMouseDown, &error) ||
      !ReadOptionalBool(arguments, "release", true, &release, &error) ||
      !ReadCaptureOptions(arguments, true, &capture, &error)) {
    return MakeErrorResult(error);
  }
  const int frames = std::clamp(requestedFrames, 1, kMaxDragFrames);

  std::optional<svg::SVGElement> element = querySelector(selector);
  if (!element.has_value()) {
    return MakeErrorResult("selector did not match an element: " + selector);
  }

  std::optional<Box2d> bounds = elementWorldBounds(*element);
  if (!bounds.has_value() || bounds->isEmpty()) {
    return MakeErrorResult("selected element has no usable world bounds: " + selector);
  }

  const Vector2d start = (bounds->topLeft + bounds->bottomRight) * 0.5;
  const Vector2d delta(dx, dy);

  if (selectionMode == "direct") {
    app_.setSelection(*element);
    std::array<Box2d, 1> selectionBounds{*bounds};
    if (!selectTool_->tryStartRedragOnSelected(app_, start, MouseModifiers{}, selectionBounds)) {
      return MakeErrorResult("direct drag setup failed for selector: " + selector);
    }
  } else if (selectionMode == "hit_test") {
    selectTool_->onMouseDown(app_, start, MouseModifiers{});
  } else {
    return MakeErrorResult("selection_mode must be 'hit_test' or 'direct'");
  }

  if (!selectTool_->isDragging()) {
    return MakeErrorResult("mouse down did not start a drag for selector: " + selector);
  }

  if (rnrRecording_.active) {
    repro::ReproEvent event;
    event.kind = repro::ReproEvent::Kind::MouseDown;
    event.mouseButton = 0;
    repro::ReproHit hit =
        element->withReadAccess([&element](svg::DocumentReadAccess&, EntityHandle) {
          return repro::ReproHit{
              .id = std::string(element->id()),
              .tag = element->tagName().toString(),
          };
        });
    event.hit = std::move(hit);
    std::vector<repro::ReproEvent> events;
    events.push_back(std::move(event));
    appendRnrFrame(start, /*mouseButtonMask=*/1, /*modifierMask=*/0, std::move(events));
  }

  ToolCallResult out;
  out.body = json{
      {"ok", true},
      {"selector", selector},
      {"selection_mode", selectionMode},
      {"start_doc", VectorToJson(start)},
      {"delta_doc", VectorToJson(delta)},
      {"frames_requested", frames},
      {"selection_after_mouse_down", selectedElementJson()},
      {"element_bounds_doc", BoxToJson(*bounds)},
      {"frames", json::array()},
  };

  const auto attachDisplayBitmap = [&](json* frameJson, std::string_view fieldName,
                                       const DisplayFrameSnapshot& display,
                                       std::string_view imageLabel) {
    if (!capture.includeDisplayFrame) {
      return;
    }

    const std::optional<svg::RendererBitmap> bitmap = composeDisplayFrameBitmap(display);
    if (!bitmap.has_value()) {
      (*frameJson)[fieldName] = nullptr;
      return;
    }

    json bitmapJson = BitmapSummary(*bitmap);
    AttachBitmapImage(&out, std::string(imageLabel), *bitmap, capture.embedPngBase64, &bitmapJson);
    (*frameJson)[fieldName] = std::move(bitmapJson);
  };

  if (renderMouseDown) {
    app_.flushFrame();
    const DisplayFrameSnapshot displayBeforeRender = currentDisplayFrame();
    std::vector<CapturedRenderResult> renderResults;
    if (!renderCurrentFrame(&renderResults, &error)) {
      return MakeErrorResult(error);
    }
    json frameJson{
        {"label", "mouse_down"},
        {"point_doc", VectorToJson(start)},
        {"display_before_render", DisplayFrameJson(displayBeforeRender)},
        {"stages", RenderResultsJson(renderResults, &out, capture, "drag_selector/mouse_down")},
    };
    attachDisplayBitmap(&frameJson, "display_before_render_bitmap", displayBeforeRender,
                        "drag_selector/mouse_down/display_before_render");
    const DisplayFrameSnapshot displayAfterRender =
        renderResults.empty() ? displayBeforeRender : renderResults.back().displayFrame;
    attachDisplayBitmap(&frameJson, "display_after_render_bitmap", displayAfterRender,
                        "drag_selector/mouse_down/display_after_render");
    out.body["frames"].push_back(std::move(frameJson));
  }

  for (int i = 1; i <= frames; ++i) {
    const double t = static_cast<double>(i) / static_cast<double>(frames);
    const Vector2d point = start + delta * t;
    selectTool_->onMouseMove(app_, point, /*buttonHeld=*/true);
    appendRnrFrame(point, /*mouseButtonMask=*/1, /*modifierMask=*/0, {});
    app_.flushFrame();
    const DisplayFrameSnapshot displayBeforeRender = currentDisplayFrame();
    std::vector<CapturedRenderResult> renderResults;
    if (!renderCurrentFrame(&renderResults, &error)) {
      return MakeErrorResult(error);
    }
    json frameJson{
        {"label", "move_" + std::to_string(i)},
        {"point_doc", VectorToJson(point)},
        {"display_before_render", DisplayFrameJson(displayBeforeRender)},
        {"stages", RenderResultsJson(renderResults, &out, capture,
                                     "drag_selector/move_" + std::to_string(i))},
    };
    attachDisplayBitmap(&frameJson, "display_before_render_bitmap", displayBeforeRender,
                        "drag_selector/move_" + std::to_string(i) + "/display_before_render");
    const DisplayFrameSnapshot displayAfterRender =
        renderResults.empty() ? displayBeforeRender : renderResults.back().displayFrame;
    attachDisplayBitmap(&frameJson, "display_after_render_bitmap", displayAfterRender,
                        "drag_selector/move_" + std::to_string(i) + "/display_after_render");
    out.body["frames"].push_back(std::move(frameJson));
  }

  if (release) {
    const Vector2d end = start + delta;
    selectTool_->onMouseUp(app_, end);
    repro::ReproEvent event;
    event.kind = repro::ReproEvent::Kind::MouseUp;
    event.mouseButton = 0;
    std::vector<repro::ReproEvent> events;
    events.push_back(std::move(event));
    appendRnrFrame(end, /*mouseButtonMask=*/0, /*modifierMask=*/0, std::move(events));
    app_.flushFrame();
    const DisplayFrameSnapshot displayBeforeRender = currentDisplayFrame();
    std::vector<CapturedRenderResult> renderResults;
    if (!renderCurrentFrame(&renderResults, &error)) {
      return MakeErrorResult(error);
    }
    json releaseJson{
        {"point_doc", VectorToJson(end)},
        {"selection", selectedElementJson()},
        {"display_before_render", DisplayFrameJson(displayBeforeRender)},
        {"stages", RenderResultsJson(renderResults, &out, capture, "drag_selector/release")},
    };
    attachDisplayBitmap(&releaseJson, "display_before_render_bitmap", displayBeforeRender,
                        "drag_selector/release/display_before_render");
    const DisplayFrameSnapshot displayAfterRender =
        renderResults.empty() ? displayBeforeRender : renderResults.back().displayFrame;
    attachDisplayBitmap(&releaseJson, "display_after_render_bitmap", displayAfterRender,
                        "drag_selector/release/display_after_render");
    out.body["release"] = std::move(releaseJson);
  }

  out.body["final_selection"] = selectedElementJson();
  out.body["attached_image_count"] = out.images.size();
  return out;
}

ToolCallResult EditorControlSession::transformSelector(const json& arguments) {
  std::string error;
  if (!ensureDocumentLoaded(&error)) {
    return MakeErrorResult(error);
  }
  (void)drainPendingWritebacks();

  const double defaultPixelsPerDocUnit = currentReproViewport().zoom;
  std::string selector;
  std::string mode;
  std::string cornerName;
  double dx = 16.0;
  double dy = 0.0;
  int requestedFrames = 8;
  bool shift = false;
  bool option = false;
  double pixelsPerDocUnit = defaultPixelsPerDocUnit > 0.0 ? defaultPixelsPerDocUnit : 1.0;
  bool renderMouseDown = true;
  bool release = true;
  CaptureOptions capture;
  if (!ReadRequiredString(arguments, "selector", &selector, &error) ||
      !ReadOptionalString(arguments, "mode", "scale", &mode, &error) ||
      !ReadOptionalString(arguments, "corner", "bottom_right", &cornerName, &error) ||
      !ReadOptionalDouble(arguments, "delta_x", 16.0, &dx, &error) ||
      !ReadOptionalDouble(arguments, "delta_y", 0.0, &dy, &error) ||
      !ReadOptionalInt(arguments, "frames", 8, &requestedFrames, &error) ||
      !ReadOptionalBool(arguments, "shift", false, &shift, &error) ||
      !ReadOptionalBool(arguments, "option", false, &option, &error) ||
      !ReadOptionalDouble(arguments, "pixels_per_doc_unit", pixelsPerDocUnit, &pixelsPerDocUnit,
                          &error) ||
      !ReadOptionalBool(arguments, "render_mouse_down", true, &renderMouseDown, &error) ||
      !ReadOptionalBool(arguments, "release", true, &release, &error) ||
      !ReadCaptureOptions(arguments, true, &capture, &error)) {
    return MakeErrorResult(error);
  }

  const bool rotateMode = mode == "rotate";
  const bool resizeMode = mode == "scale" || mode == "resize";
  if (!rotateMode && !resizeMode) {
    return MakeErrorResult("mode must be 'scale', 'resize', or 'rotate'");
  }
  if (rotateMode && shift) {
    return MakeErrorResult("shift suppresses rotate handles; omit shift for mode='rotate'");
  }
  if (pixelsPerDocUnit <= 0.0 || !std::isfinite(pixelsPerDocUnit)) {
    return MakeErrorResult("pixels_per_doc_unit must be a positive finite number");
  }

  const std::optional<SelectionTransformCorner> corner = ParseSelectionTransformCorner(cornerName);
  if (!corner.has_value()) {
    return MakeErrorResult(
        "corner must be 'top_left', 'top_right', 'bottom_right', or 'bottom_left'");
  }
  const int frames = std::clamp(requestedFrames, 1, kMaxDragFrames);

  std::optional<svg::SVGElement> element = querySelector(selector);
  if (!element.has_value()) {
    return MakeErrorResult("selector did not match an element: " + selector);
  }

  std::optional<Box2d> bounds = elementWorldBounds(*element);
  if (!bounds.has_value() || bounds->isEmpty()) {
    return MakeErrorResult("selected element has no usable world bounds: " + selector);
  }

  const Vector2d start = rotateMode ? RotateHandlePointForCorner(*bounds, *corner, pixelsPerDocUnit)
                                    : SelectionTransformCornerPoint(*bounds, *corner);
  const Vector2d delta(dx, dy);
  MouseModifiers modifiers;
  modifiers.shift = shift;
  modifiers.option = option;
  modifiers.pixelsPerDocUnit = pixelsPerDocUnit;
  const int modifierMask = ModifierMaskFromMouseModifiers(modifiers);

  app_.setSelection(*element);
  app_.flushFrame();
  selectTool_->onMouseDown(app_, start, modifiers);
  if (!selectTool_->isDragging()) {
    return MakeErrorResult("transform handle mouse down did not start a drag for selector: " +
                           selector);
  }

  if (rnrRecording_.active) {
    repro::ReproEvent event;
    event.kind = repro::ReproEvent::Kind::MouseDown;
    event.mouseButton = 0;
    event.modifiers = modifierMask;
    repro::ReproHit hit =
        element->withReadAccess([&element](svg::DocumentReadAccess&, EntityHandle) {
          return repro::ReproHit{
              .id = std::string(element->id()),
              .tag = element->tagName().toString(),
          };
        });
    event.hit = std::move(hit);
    std::vector<repro::ReproEvent> events;
    events.push_back(std::move(event));
    appendRnrFrame(start, /*mouseButtonMask=*/1, modifierMask, std::move(events));
  }

  ToolCallResult out;
  out.body = json{
      {"ok", true},
      {"selector", selector},
      {"mode", mode},
      {"gesture_kind", rotateMode ? "rotate" : "resize"},
      {"corner", SelectionTransformCornerName(*corner)},
      {"handle_start_doc", VectorToJson(start)},
      {"delta_doc", VectorToJson(delta)},
      {"frames_requested", frames},
      {"pixels_per_doc_unit", pixelsPerDocUnit},
      {"modifiers", {{"shift", shift}, {"option", option}, {"mask", modifierMask}}},
      {"selection_after_mouse_down", selectedElementJson()},
      {"element_bounds_doc", BoxToJson(*bounds)},
      {"active_gesture_after_mouse_down",
       ActiveGesturePreviewJson(selectTool_->activeGesturePreview())},
      {"frames", json::array()},
  };

  const auto attachDisplayAfterRenderBitmap = [&](json* frameJson,
                                                  const DisplayFrameSnapshot& display,
                                                  std::string_view imageLabel) {
    if (!capture.includeDisplayFrame) {
      return;
    }

    const std::optional<svg::RendererBitmap> bitmap = composeDisplayFrameBitmap(display);
    if (!bitmap.has_value()) {
      (*frameJson)["display_after_render_bitmap"] = nullptr;
      return;
    }

    json bitmapJson = BitmapSummary(*bitmap);
    AttachBitmapImage(&out, std::string(imageLabel), *bitmap, capture.embedPngBase64, &bitmapJson);
    (*frameJson)["display_after_render_bitmap"] = std::move(bitmapJson);
  };

  if (renderMouseDown) {
    app_.flushFrame();
    const DisplayFrameSnapshot displayBeforeRender = currentDisplayFrame();
    std::vector<CapturedRenderResult> renderResults;
    if (!renderCurrentFrame(&renderResults, &error)) {
      return MakeErrorResult(error);
    }
    json frameJson{
        {"label", "mouse_down"},
        {"point_doc", VectorToJson(start)},
        {"active_gesture_preview", ActiveGesturePreviewJson(selectTool_->activeGesturePreview())},
        {"display_before_render", DisplayFrameJson(displayBeforeRender)},
        {"stages",
         RenderResultsJson(renderResults, &out, capture, "transform_selector/mouse_down")},
    };
    const DisplayFrameSnapshot displayAfterRender =
        renderResults.empty() ? displayBeforeRender : renderResults.back().displayFrame;
    attachDisplayAfterRenderBitmap(&frameJson, displayAfterRender,
                                   "transform_selector/mouse_down/display_after_render");
    out.body["frames"].push_back(std::move(frameJson));
  }

  for (int i = 1; i <= frames; ++i) {
    const double t = static_cast<double>(i) / static_cast<double>(frames);
    const Vector2d point = start + delta * t;
    selectTool_->onMouseMove(app_, point, /*buttonHeld=*/true, modifiers);
    appendRnrFrame(point, /*mouseButtonMask=*/1, modifierMask, {});
    app_.flushFrame();
    const DisplayFrameSnapshot displayBeforeRender = currentDisplayFrame();
    std::vector<CapturedRenderResult> renderResults;
    if (!renderCurrentFrame(&renderResults, &error)) {
      return MakeErrorResult(error);
    }
    json frameJson{
        {"label", "move_" + std::to_string(i)},
        {"point_doc", VectorToJson(point)},
        {"active_gesture_preview", ActiveGesturePreviewJson(selectTool_->activeGesturePreview())},
        {"display_before_render", DisplayFrameJson(displayBeforeRender)},
        {"stages", RenderResultsJson(renderResults, &out, capture,
                                     "transform_selector/move_" + std::to_string(i))},
    };
    const DisplayFrameSnapshot displayAfterRender =
        renderResults.empty() ? displayBeforeRender : renderResults.back().displayFrame;
    attachDisplayAfterRenderBitmap(
        &frameJson, displayAfterRender,
        "transform_selector/move_" + std::to_string(i) + "/display_after_render");
    out.body["frames"].push_back(std::move(frameJson));
  }

  if (release) {
    const Vector2d end = start + delta;
    selectTool_->onMouseUp(app_, end);
    repro::ReproEvent event;
    event.kind = repro::ReproEvent::Kind::MouseUp;
    event.mouseButton = 0;
    event.modifiers = modifierMask;
    std::vector<repro::ReproEvent> events;
    events.push_back(std::move(event));
    appendRnrFrame(end, /*mouseButtonMask=*/0, modifierMask, std::move(events));
    app_.flushFrame();
    const DisplayFrameSnapshot displayBeforeRender = currentDisplayFrame();
    std::vector<CapturedRenderResult> renderResults;
    if (!renderCurrentFrame(&renderResults, &error)) {
      return MakeErrorResult(error);
    }
    json releaseJson{
        {"point_doc", VectorToJson(end)},
        {"selection", selectedElementJson()},
        {"active_gesture_preview", ActiveGesturePreviewJson(selectTool_->activeGesturePreview())},
        {"display_before_render", DisplayFrameJson(displayBeforeRender)},
        {"stages", RenderResultsJson(renderResults, &out, capture, "transform_selector/release")},
    };
    const DisplayFrameSnapshot displayAfterRender =
        renderResults.empty() ? displayBeforeRender : renderResults.back().displayFrame;
    attachDisplayAfterRenderBitmap(&releaseJson, displayAfterRender,
                                   "transform_selector/release/display_after_render");
    out.body["release"] = std::move(releaseJson);
  }

  out.body["final_selection"] = selectedElementJson();
  out.body["attached_image_count"] = out.images.size();
  return out;
}

}  // namespace donner::editor::mcp
