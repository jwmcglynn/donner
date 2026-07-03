#include "tools/mcp-servers/editor-control/EditorControlSession.h"

#include <fcntl.h>
#include <pixelmatch/pixelmatch.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/EcsRegistry.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/editor/AttributeWriteback.h"
#include "donner/editor/EditorCommand.h"
#include "donner/editor/LayersPanel.h"
#include "donner/editor/PresentationRenderScheduler.h"
#include "donner/editor/PresentedFrameComposer.h"
#include "donner/editor/SelectionTransformHandles.h"
#include "donner/editor/TextPatch.h"
#include "donner/editor/repro/GlRnrReplay.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/SVGGeometryElement.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/IdComponent.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/compositor/CompositorController.h"
#include "donner/svg/compositor/ScopedCompositorHint.h"
#include "donner/svg/renderer/RendererImageIO.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "tools/mcp-servers/editor-control/EditorControlSessionInternal.h"

namespace donner::editor::mcp {
namespace {

using nlohmann::json;

json ElementJson(const svg::SVGElement& element) {
  const Entity entity = element.unsafeEntityHandle().entity();
  return element.withReadAccess([&element, entity](svg::DocumentReadAccess&, EntityHandle) {
    return json{
        {"entity", EntityToJsonValue(entity)},
        {"tag", element.tagName().toString()},
        {"id", std::string(element.id())},
        {"class", std::string(element.className())},
    };
  });
}

}  // namespace

EditorControlSession::EditorControlSession() : selectTool_(std::make_unique<SelectTool>()) {}

EditorControlSession::~EditorControlSession() = default;

json EditorControlSession::toolList() {
  return json::array({
      {
          {"name", "load_document"},
          {"description", "Load an SVG file into the headless Donner editor session."},
          {"inputSchema",
           {
               {"type", "object"},
               {"properties",
                {
                    {"svg_path", {{"type", "string"}, {"description", "Path to an SVG file."}}},
                    {"canvas_width", {{"type", "integer"}, {"minimum", 1}}},
                    {"canvas_height", {{"type", "integer"}, {"minimum", 1}}},
                    {"device_pixel_ratio", {{"type", "number"}, {"default", 1.0}}},
                    {"render_after_load", {{"type", "boolean"}, {"default", true}}},
                    {"include_final_frame", {{"type", "boolean"}, {"default", false}}},
                    {"embed_png_base64", {{"type", "boolean"}, {"default", false}}},
                }},
               {"required", json::array({"svg_path"})},
           }},
      },
      {
          {"name", "load_svg"},
          {"description", "Load SVG source bytes directly into the headless editor session."},
          {"inputSchema",
           {
               {"type", "object"},
               {"properties",
                {
                    {"svg_source", {{"type", "string"}, {"description", "Complete SVG XML."}}},
                    {"source_path", {{"type", "string"}}},
                    {"canvas_width", {{"type", "integer"}, {"minimum", 1}}},
                    {"canvas_height", {{"type", "integer"}, {"minimum", 1}}},
                    {"device_pixel_ratio", {{"type", "number"}, {"default", 1.0}}},
                    {"render_after_load", {{"type", "boolean"}, {"default", true}}},
                    {"include_final_frame", {{"type", "boolean"}, {"default", false}}},
                    {"embed_png_base64", {{"type", "boolean"}, {"default", false}}},
                }},
               {"required", json::array({"svg_source"})},
           }},
      },
      {
          {"name", "get_svg_source"},
          {"description",
           "Return the editable SVG source draft currently owned by the headless editor session."},
          {"inputSchema",
           {
               {"type", "object"},
               {"properties",
                {
                    {"offset", {{"type", "integer"}, {"minimum", 0}, {"default", 0}}},
                    {"length", {{"type", "integer"}, {"minimum", 0}}},
                }},
           }},
      },
      {
          {"name", "edit_svg_source"},
          {"description",
           "Apply incremental text edits to the editable SVG source draft, then parse and "
           "optionally render the result when the draft is valid."},
          {"inputSchema",
           {
               {"type", "object"},
               {"properties",
                {
                    {"replace_source",
                     {{"type", "string"},
                      {"description", "Optional complete replacement for the SVG draft."}}},
                    {"source_path", {{"type", "string"}}},
                    {"expected_source_revision",
                     {{"type", "integer"},
                      {"minimum", 0},
                      {"description",
                       "Optional guard: reject the edit if the source draft has changed."}}},
                    {"edits",
                     {{"type", "array"},
                      {"items",
                       {{"type", "object"},
                        {"properties",
                         {
                             {"offset", {{"type", "integer"}, {"minimum", 0}}},
                             {"delete_count",
                              {{"type", "integer"}, {"minimum", 0}, {"default", 0}}},
                             {"insert", {{"type", "string"}, {"default", ""}}},
                         }},
                        {"required", json::array({"offset"})}}}}},
                    {"allow_parse_failure", {{"type", "boolean"}, {"default", true}}},
                    {"render_after_edit", {{"type", "boolean"}, {"default", true}}},
                    {"canvas_width", {{"type", "integer"}, {"minimum", 1}}},
                    {"canvas_height", {{"type", "integer"}, {"minimum", 1}}},
                    {"device_pixel_ratio", {{"type", "number"}, {"default", 1.0}}},
                    {"include_final_frame", {{"type", "boolean"}, {"default", true}}},
                    {"include_tile_images", {{"type", "boolean"}, {"default", false}}},
                    {"embed_png_base64", {{"type", "boolean"}, {"default", false}}},
                }},
           }},
      },
      {
          {"name", "select_by_selector"},
          {"description", "Select the first element matching a CSS selector."},
          {"inputSchema",
           {
               {"type", "object"},
               {"properties",
                {
                    {"selector", {{"type", "string"}}},
                    {"render", {{"type", "boolean"}, {"default", true}}},
                    {"include_final_frame", {{"type", "boolean"}, {"default", false}}},
                    {"include_tile_images", {{"type", "boolean"}, {"default", false}}},
                    {"embed_png_base64", {{"type", "boolean"}, {"default", false}}},
                }},
               {"required", json::array({"selector"})},
           }},
      },
      {
          {"name", "click_layer_button"},
          {"description",
           "Find a Layers-panel row by CSS selector and click one of its row buttons through the "
           "same shared handler used by the UI."},
          {"inputSchema",
           {
               {"type", "object"},
               {"properties",
                {
                    {"selector", {{"type", "string"}}},
                    {"button",
                     {{"type", "string"},
                      {"enum", json::array({"visibility", "lock"})},
                      {"default", "visibility"}}},
                    {"render_after_click", {{"type", "boolean"}, {"default", true}}},
                    {"include_display_before_render", {{"type", "boolean"}, {"default", true}}},
                    {"include_final_frame", {{"type", "boolean"}, {"default", true}}},
                    {"include_tile_images", {{"type", "boolean"}, {"default", false}}},
                    {"include_display_frame", {{"type", "boolean"}, {"default", false}}},
                    {"embed_png_base64", {{"type", "boolean"}, {"default", false}}},
                }},
               {"required", json::array({"selector"})},
           }},
      },
      {
          {"name", "set_active_tool"},
          {"description",
           "Set the active headless canvas tool. Recorded .rnr sessions store this as a semantic "
           "action so replay dispatches later pointer frames through the same tool."},
          {"inputSchema",
           {
               {"type", "object"},
               {"properties",
                {
                    {"tool",
                     {{"type", "string"},
                      {"enum", json::array({"select", "pen"})},
                      {"description", "Canvas tool to activate."}}},
                }},
               {"required", json::array({"tool"})},
           }},
      },
      {
          {"name", "set_style_property"},
          {"description",
           "Set active fill/stroke paint and merge the same CSS style property into the current "
           "selection when one exists."},
          {"inputSchema",
           {
               {"type", "object"},
               {"properties",
                {
                    {"property",
                     {{"type", "string"},
                      {"description", "CSS property name, usually fill or stroke."}}},
                    {"value",
                     {{"type", "string"}, {"description", "CSS property value, e.g. #ff0000."}}},
                    {"render_after_set", {{"type", "boolean"}, {"default", true}}},
                    {"include_final_frame", {{"type", "boolean"}, {"default", false}}},
                    {"include_tile_images", {{"type", "boolean"}, {"default", false}}},
                    {"embed_png_base64", {{"type", "boolean"}, {"default", false}}},
                }},
               {"required", json::array({"property", "value"})},
           }},
      },
      {
          {"name", "pen_path"},
          {"description",
           "Create a path by dispatching document-space clicks through PenTool. The tool records "
           "the active-tool action and per-click frames into an active .rnr recording."},
          {"inputSchema",
           {
               {"type", "object"},
               {"properties",
                {
                    {"points",
                     {{"type", "array"},
                      {"items",
                       {{"type", "object"},
                        {"properties",
                         {
                             {"x", {{"type", "number"}}},
                             {"y", {{"type", "number"}}},
                         }},
                        {"required", json::array({"x", "y"})}}},
                      {"minItems", 1}}},
                    {"close", {{"type", "boolean"}, {"default", false}}},
                    {"commit_open", {{"type", "boolean"}, {"default", true}}},
                    {"render_after_path", {{"type", "boolean"}, {"default", true}}},
                    {"include_final_frame", {{"type", "boolean"}, {"default", false}}},
                    {"include_tile_images", {{"type", "boolean"}, {"default", false}}},
                    {"embed_png_base64", {{"type", "boolean"}, {"default", false}}},
                }},
               {"required", json::array({"points"})},
           }},
      },
      {
          {"name", "drag_selector"},
          {"description",
           "Find an element by CSS selector and synthesize click-and-drag frames through "
           "SelectTool."},
          {"inputSchema",
           {
               {"type", "object"},
               {"properties",
                {
                    {"selector", {{"type", "string"}}},
                    {"delta_x", {{"type", "number"}, {"default", 16.0}}},
                    {"delta_y", {{"type", "number"}, {"default", 0.0}}},
                    {"frames", {{"type", "integer"}, {"minimum", 1}, {"maximum", kMaxDragFrames}}},
                    {"selection_mode",
                     {{"type", "string"},
                      {"enum", json::array({"hit_test", "direct"})},
                      {"default", "direct"}}},
                    {"render_mouse_down", {{"type", "boolean"}, {"default", true}}},
                    {"release", {{"type", "boolean"}, {"default", true}}},
                    {"include_final_frame", {{"type", "boolean"}, {"default", true}}},
                    {"include_tile_images", {{"type", "boolean"}, {"default", false}}},
                    {"include_display_frame", {{"type", "boolean"}, {"default", false}}},
                    {"embed_png_base64", {{"type", "boolean"}, {"default", false}}},
                }},
               {"required", json::array({"selector"})},
           }},
      },
      {
          {"name", "transform_selector"},
          {"description",
           "Find an element by CSS selector and synthesize selection transform-handle drag "
           "frames through SelectTool. Use mode='scale'/'resize' for corner scale handles or "
           "mode='rotate' for the rotate ring."},
          {"inputSchema",
           {
               {"type", "object"},
               {"properties",
                {
                    {"selector", {{"type", "string"}}},
                    {"mode",
                     {{"type", "string"},
                      {"enum", json::array({"scale", "resize", "rotate"})},
                      {"default", "scale"}}},
                    {"corner",
                     {{"type", "string"},
                      {"enum",
                       json::array({"top_left", "top_right", "bottom_right", "bottom_left"})},
                      {"default", "bottom_right"}}},
                    {"delta_x", {{"type", "number"}, {"default", 16.0}}},
                    {"delta_y", {{"type", "number"}, {"default", 0.0}}},
                    {"frames", {{"type", "integer"}, {"minimum", 1}, {"maximum", kMaxDragFrames}}},
                    {"shift", {{"type", "boolean"}, {"default", false}}},
                    {"option", {{"type", "boolean"}, {"default", false}}},
                    {"pixels_per_doc_unit",
                     {{"type", "number"},
                      {"description",
                       "Optional override for screen-pixel-stable transform handle hit testing."}}},
                    {"render_mouse_down", {{"type", "boolean"}, {"default", true}}},
                    {"release", {{"type", "boolean"}, {"default", true}}},
                    {"include_final_frame", {{"type", "boolean"}, {"default", true}}},
                    {"include_tile_images", {{"type", "boolean"}, {"default", false}}},
                    {"include_display_frame", {{"type", "boolean"}, {"default", false}}},
                    {"embed_png_base64", {{"type", "boolean"}, {"default", false}}},
                }},
               {"required", json::array({"selector"})},
           }},
      },
      {
          {"name", "render_frame"},
          {"description", "Render the current editor state and return split compositor tiles."},
          {"inputSchema",
           {
               {"type", "object"},
               {"properties",
                {
                    {"include_final_frame", {{"type", "boolean"}, {"default", true}}},
                    {"include_tile_images", {{"type", "boolean"}, {"default", false}}},
                    {"embed_png_base64", {{"type", "boolean"}, {"default", false}}},
                }},
           }},
      },
      {
          {"name", "session_state"},
          {"description", "Return selection, canvas, and compositor diagnostics."},
          {"inputSchema", {{"type", "object"}, {"properties", json::object()}}},
      },
      {
          {"name", "start_rnr_recording"},
          {"description", "Start recording subsequent MCP-driven editor gestures to an .rnr file."},
          {"inputSchema",
           {
               {"type", "object"},
               {"properties",
                {
                    {"output_path", {{"type", "string"}}},
                    {"svg_path", {{"type", "string"}}},
                    {"window_width", {{"type", "integer"}, {"minimum", 1}}},
                    {"window_height", {{"type", "integer"}, {"minimum", 1}}},
                    {"display_scale", {{"type", "number"}, {"default", 1.0}}},
                    {"frame_delta_ms", {{"type", "number"}, {"default", 16.6666667}}},
                }},
           }},
      },
      {
          {"name", "stop_rnr_recording"},
          {"description", "Stop the active .rnr recording and optionally write it to disk."},
          {"inputSchema",
           {
               {"type", "object"},
               {"properties",
                {
                    {"output_path", {{"type", "string"}}},
                    {"write_file", {{"type", "boolean"}, {"default", true}}},
                }},
           }},
      },
      {
          {"name", "rnr_recording_state"},
          {"description", "Return the current .rnr recording state."},
          {"inputSchema", {{"type", "object"}, {"properties", json::object()}}},
      },
      {
          {"name", "replay_rnr"},
          {"description", "Replay an .rnr file through the headless editor session."},
          {"inputSchema",
           {
               {"type", "object"},
               {"properties",
                {
                    {"rnr_path", {{"type", "string"}}},
                    {"svg_path", {{"type", "string"}}},
                    {"render_each_frame", {{"type", "boolean"}, {"default", true}}},
                    {"simulate_editor_shell_frame_loop", {{"type", "boolean"}, {"default", false}}},
                    {"gl_readback", {{"type", "boolean"}, {"default", false}}},
                    {"gl_capture_frame", {{"type", "integer"}, {"minimum", 0}}},
                    {"gl_capture_left_mousedown", {{"type", "integer"}, {"minimum", 1}}},
                    {"gl_max_frame", {{"type", "integer"}, {"minimum", 0}}},
                    {"gl_crop",
                     {{"type", "string"},
                      {"enum", json::array({"full", "render-pane", "document-canvas"})},
                      {"default", "full"}}},
                    {"gl_output_dir", {{"type", "string"}}},
                    {"gl_visible", {{"type", "boolean"}, {"default", false}}},
                    {"gl_pace", {{"type", "boolean"}, {"default", true}}},
                    {"gl_drive_document_input", {{"type", "boolean"}, {"default", false}}},
                    {"gl_timeout_ms", {{"type", "integer"}, {"minimum", 1}, {"default", 120000}}},
                    {"include_gl_images", {{"type", "boolean"}, {"default", true}}},
                    {"include_frame_results", {{"type", "boolean"}, {"default", true}}},
                    {"max_frame_results", {{"type", "integer"}, {"minimum", 0}}},
                    {"stop_after_mouse_ups", {{"type", "integer"}, {"minimum", 0}}},
                    {"compare_presented_after_left_mouse_down",
                     {{"type", "integer"}, {"minimum", 0}}},
                    {"compare_presented_frame_offset_after_left_mouse_down",
                     {{"type", "integer"}, {"minimum", 0}}},
                    {"include_display_diff", {{"type", "boolean"}, {"default", false}}},
                    {"include_final_frame", {{"type", "boolean"}, {"default", false}}},
                    {"include_tile_images", {{"type", "boolean"}, {"default", false}}},
                    {"embed_png_base64", {{"type", "boolean"}, {"default", false}}},
                }},
               {"required", json::array({"rnr_path"})},
           }},
      },
  });
}

ToolCallResult EditorControlSession::handleToolCall(std::string_view name, const json& arguments) {
  if (!arguments.is_object()) {
    return MakeErrorResult("tool arguments must be a JSON object");
  }
  if (name == "load_document") return loadDocument(arguments);
  if (name == "load_svg") return loadSvg(arguments);
  if (name == "get_svg_source") return getSvgSource(arguments);
  if (name == "edit_svg_source") return editSvgSource(arguments);
  if (name == "select_by_selector") return selectBySelector(arguments);
  if (name == "click_layer_button") return clickLayerButton(arguments);
  if (name == "set_active_tool") return setActiveTool(arguments);
  if (name == "set_style_property") return setStyleProperty(arguments);
  if (name == "pen_path") return penPath(arguments);
  if (name == "drag_selector") return dragSelector(arguments);
  if (name == "transform_selector") return transformSelector(arguments);
  if (name == "render_frame") return renderFrameTool(arguments);
  if (name == "session_state") return sessionState(arguments);
  if (name == "start_rnr_recording") return startRnrRecording(arguments);
  if (name == "stop_rnr_recording") return stopRnrRecording(arguments);
  if (name == "rnr_recording_state") return rnrRecordingState(arguments);
  if (name == "replay_rnr") return replayRnr(arguments);

  return MakeErrorResult("unknown tool: " + std::string(name));
}

bool EditorControlSession::syncCanvasSize(const ViewportState& viewport) {
  if (!app_.hasDocument()) {
    return false;
  }

  const Vector2i desired = viewport.desiredCanvasSize();
  const Vector2i current = app_.document().document().canvasSize();
  if (desired == current) {
    return false;
  }

  app_.document().document().setCanvasSize(desired.x, desired.y);
  canvasWidth_ = desired.x;
  canvasHeight_ = desired.y;
  devicePixelRatio_ = viewport.devicePixelRatio;
  return true;
}

void EditorControlSession::syncSourceTextFromDocumentIfChanged() {
  if (!app_.hasDocument() || !app_.document().document().hasSourceStore()) {
    return;
  }

  const std::string sourceAfter(app_.document().document().source());
  if (sourceAfter == currentSourceText_) {
    return;
  }

  currentSourceText_ = sourceAfter;
  ++sourceRevision_;
  loadedSourceRevision_ = sourceRevision_;
  lastParseError_.reset();
}

std::optional<svg::SVGElement> EditorControlSession::querySelector(std::string_view selector) {
  return app_.document().document().querySelector(selector);
}

std::optional<Box2d> EditorControlSession::elementWorldBounds(
    const svg::SVGElement& element) const {
  std::optional<Box2d> result;
  const std::optional<svg::SVGGeometryElement> geometry =
      element.withReadAccess([&element](svg::DocumentReadAccess&, EntityHandle) {
        if (!element.isa<svg::SVGGeometryElement>()) {
          return std::optional<svg::SVGGeometryElement>();
        }

        return std::optional(element.cast<svg::SVGGeometryElement>());
      });
  if (geometry.has_value()) {
    result = geometry->worldBounds();
  }

  std::optional<svg::SVGElement> child = element.withReadAccess(
      [&element](svg::DocumentReadAccess&, EntityHandle) { return element.firstChild(); });
  while (child.has_value()) {
    const svg::SVGElement currentChild = *child;
    if (auto childBounds = elementWorldBounds(*child); childBounds.has_value()) {
      if (result.has_value()) {
        result->addBox(*childBounds);
      } else {
        result = *childBounds;
      }
    }

    child = currentChild.withReadAccess([&currentChild](svg::DocumentReadAccess&, EntityHandle) {
      return currentChild.nextSibling();
    });
  }

  return result;
}

json EditorControlSession::selectedElementJson() const {
  if (!app_.selectedElement().has_value()) {
    return nullptr;
  }
  return ElementJson(*app_.selectedElement());
}

bool EditorControlSession::ensureDocumentLoaded(std::string* error) const {
  if (!app_.hasDocument()) {
    *error = "no SVG document loaded";
    return false;
  }
  return true;
}

bool EditorControlSession::waitUntilIdle(std::string* error) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (asyncRenderer_.isBusy() && std::chrono::steady_clock::now() < deadline) {
    (void)asyncRenderer_.pollResult();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  while (asyncRenderer_.pollResult().has_value()) {}
  if (asyncRenderer_.isBusy()) {
    *error = "timed out waiting for async renderer to go idle";
    return false;
  }
  return true;
}

}  // namespace donner::editor::mcp
