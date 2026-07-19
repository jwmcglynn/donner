/// @file
/// EditorControlSession .rnr tools: recording MCP-driven gestures, replaying
/// .rnr files (headless and GL-readback modes), and presented-frame diff
/// diagnostics.

#include <fcntl.h>
#include <pixelmatch/pixelmatch.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/editor/AttributeWriteback.h"
#include "donner/editor/EditorCommand.h"
#include "donner/editor/PresentationRenderScheduler.h"
#include "donner/editor/ViewportState.h"
#include "donner/editor/repro/GlRnrReplay.h"
#include "donner/editor/repro/ReproFile.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/renderer/RendererImageIO.h"
#include "nlohmann/json.hpp"
#include "tools/mcp-servers/editor-control/EditorControlSessionGlReadback.h"
#include "tools/mcp-servers/editor-control/EditorControlSessionInternal.h"

namespace donner::editor::mcp {

namespace {

using nlohmann::json;

constexpr float kDisplayDiffPixelmatchThreshold = 0.02f;
constexpr bool kDisplayDiffPixelmatchIncludeAa = false;

bool NearlyEqual(double a, double b) {
  return std::abs(a - b) <= 1e-9;
}

void ApplyReproViewport(ViewportState* viewport, const repro::ReproViewport& reproViewport) {
  viewport->paneOrigin = Vector2d(reproViewport.paneOriginX, reproViewport.paneOriginY);
  viewport->paneSize = Vector2d(reproViewport.paneSizeW, reproViewport.paneSizeH);
  viewport->devicePixelRatio = reproViewport.devicePixelRatio;
  viewport->zoom = reproViewport.zoom;
  viewport->panDocPoint = Vector2d(reproViewport.panDocX, reproViewport.panDocY);
  viewport->panScreenPoint = Vector2d(reproViewport.panScreenX, reproViewport.panScreenY);
  viewport->documentViewBox = Box2d::FromXYWH(reproViewport.viewBoxX, reproViewport.viewBoxY,
                                              reproViewport.viewBoxW, reproViewport.viewBoxH);
}

bool ReproViewportMatches(const ViewportState& viewport,
                          const repro::ReproViewport& reproViewport) {
  return NearlyEqual(viewport.paneOrigin.x, reproViewport.paneOriginX) &&
         NearlyEqual(viewport.paneOrigin.y, reproViewport.paneOriginY) &&
         NearlyEqual(viewport.paneSize.x, reproViewport.paneSizeW) &&
         NearlyEqual(viewport.paneSize.y, reproViewport.paneSizeH) &&
         NearlyEqual(viewport.devicePixelRatio, reproViewport.devicePixelRatio) &&
         NearlyEqual(viewport.zoom, reproViewport.zoom) &&
         NearlyEqual(viewport.panDocPoint.x, reproViewport.panDocX) &&
         NearlyEqual(viewport.panDocPoint.y, reproViewport.panDocY) &&
         NearlyEqual(viewport.panScreenPoint.x, reproViewport.panScreenX) &&
         NearlyEqual(viewport.panScreenPoint.y, reproViewport.panScreenY) &&
         NearlyEqual(viewport.documentViewBox.topLeft.x, reproViewport.viewBoxX) &&
         NearlyEqual(viewport.documentViewBox.topLeft.y, reproViewport.viewBoxY) &&
         NearlyEqual(viewport.documentViewBox.size().x, reproViewport.viewBoxW) &&
         NearlyEqual(viewport.documentViewBox.size().y, reproViewport.viewBoxH);
}

bool CanPixelmatchBitmaps(const svg::RendererBitmap& actual, const svg::RendererBitmap& expected) {
  if (actual.dimensions != expected.dimensions || actual.dimensions.x <= 0 ||
      actual.dimensions.y <= 0) {
    return false;
  }

  const std::size_t actualRowBytes = BitmapRowBytes(actual);
  const std::size_t expectedRowBytes = BitmapRowBytes(expected);
  return actualRowBytes == expectedRowBytes && actualRowBytes % 4u == 0u &&
         actual.pixels.size() == actualRowBytes * static_cast<std::size_t>(actual.dimensions.y) &&
         expected.pixels.size() ==
             expectedRowBytes * static_cast<std::size_t>(expected.dimensions.y);
}

json BitmapDiffSummary(const std::optional<svg::RendererBitmap>& actual,
                       const svg::RendererBitmap& expected) {
  if (!actual.has_value() || actual->empty() || expected.empty()) {
    return json{
        {"available", false},
        {"actual_bitmap", actual.has_value() ? BitmapSummary(*actual) : json(nullptr)},
        {"expected_bitmap", BitmapSummary(expected)},
    };
  }

  const svg::RendererBitmap& actualBitmap = *actual;
  if (!CanPixelmatchBitmaps(actualBitmap, expected)) {
    return json{
        {"available", false},
        {"actual_bitmap", BitmapSummary(actualBitmap)},
        {"expected_bitmap", BitmapSummary(expected)},
        {"dimension_mismatch", actualBitmap.dimensions != expected.dimensions},
        {"comparison", "pixelmatch"},
        {"pixelmatch_threshold", kDisplayDiffPixelmatchThreshold},
        {"pixelmatch_include_anti_aliasing", kDisplayDiffPixelmatchIncludeAa},
        {"pixelmatch_error", "bitmap layout is not compatible with pixelmatch"},
    };
  }

  std::vector<std::uint8_t> diffImage(actualBitmap.pixels.size(), 0u);
  pixelmatch::Options options;
  options.threshold = kDisplayDiffPixelmatchThreshold;
  options.includeAA = kDisplayDiffPixelmatchIncludeAa;
  const std::size_t rowBytes = BitmapRowBytes(actualBitmap);
  const int mismatchedPixels = pixelmatch::pixelmatch(
      expected.pixels, actualBitmap.pixels, diffImage, actualBitmap.dimensions.x,
      actualBitmap.dimensions.y, rowBytes / 4u, options);

  return json{
      {"available", true},
      {"actual_bitmap", BitmapSummary(actualBitmap)},
      {"expected_bitmap", BitmapSummary(expected)},
      {"dimension_mismatch", false},
      {"compared_pixels", static_cast<std::uint64_t>(actualBitmap.dimensions.x) *
                              static_cast<std::uint64_t>(actualBitmap.dimensions.y)},
      {"comparison", "pixelmatch"},
      {"pixelmatch_threshold", kDisplayDiffPixelmatchThreshold},
      {"pixelmatch_include_anti_aliasing", kDisplayDiffPixelmatchIncludeAa},
      {"differing_pixels", mismatchedPixels},
  };
}

std::filesystem::path DiagnosticOutputDir() {
  if (const char* dir = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR"); dir != nullptr) {
    return std::filesystem::path(dir);
  }
  return std::filesystem::temp_directory_path();
}

std::string SanitizeArtifactLabel(std::string_view label) {
  std::string out;
  out.reserve(label.size());
  for (const char c : label) {
    out.push_back(c == '/' || c == '\\' || c == ':' || c == ' ' ? '_' : c);
  }
  return out;
}

bool WriteBitmapPng(const svg::RendererBitmap& bitmap, const std::filesystem::path& path) {
  if (bitmap.empty() || bitmap.rowBytes % 4u != 0u ||
      bitmap.rowBytes != static_cast<std::size_t>(bitmap.dimensions.x) * 4u) {
    return false;
  }

  return svg::RendererImageIO::writeRgbaPixelsToPngFile(path.string().c_str(), bitmap.pixels,
                                                        bitmap.dimensions.x, bitmap.dimensions.y,
                                                        bitmap.rowBytes / 4u);
}

svg::RendererBitmap BuildDiffBitmap(const svg::RendererBitmap& actual,
                                    const svg::RendererBitmap& expected) {
  svg::RendererBitmap diff;
  if (!CanPixelmatchBitmaps(actual, expected)) {
    return diff;
  }

  const std::size_t rowBytes = BitmapRowBytes(actual);
  diff.dimensions = actual.dimensions;
  diff.rowBytes = rowBytes;
  diff.alphaType = svg::AlphaType::Premultiplied;
  diff.pixels.resize(static_cast<std::size_t>(diff.dimensions.y) * diff.rowBytes, 0u);

  pixelmatch::Options options;
  options.threshold = kDisplayDiffPixelmatchThreshold;
  options.includeAA = kDisplayDiffPixelmatchIncludeAa;
  static_cast<void>(pixelmatch::pixelmatch(expected.pixels, actual.pixels, diff.pixels,
                                           actual.dimensions.x, actual.dimensions.y, rowBytes / 4u,
                                           options));
  return diff;
}

void AddBitmapDiffArtifacts(json* diff, const std::optional<svg::RendererBitmap>& actual,
                            const svg::RendererBitmap& expected, std::string_view label) {
  if (!actual.has_value() || actual->empty() || expected.empty() ||
      !diff->value("available", false)) {
    return;
  }
  if (diff->value("differing_pixels", 0) == 0) {
    return;
  }

  const std::filesystem::path outDir = DiagnosticOutputDir();
  std::error_code ec;
  std::filesystem::create_directories(outDir, ec);
  const std::string artifactLabel = SanitizeArtifactLabel(label);
  const std::filesystem::path actualPath = outDir / ("actual_" + artifactLabel + ".png");
  const std::filesystem::path expectedPath = outDir / ("expected_" + artifactLabel + ".png");
  const std::filesystem::path diffPath = outDir / ("diff_" + artifactLabel + ".png");
  const svg::RendererBitmap diffBitmap = BuildDiffBitmap(*actual, expected);

  (*diff)["artifacts"] = json{
      {"actual", WriteBitmapPng(*actual, actualPath) ? json(actualPath.string()) : json(nullptr)},
      {"expected",
       WriteBitmapPng(expected, expectedPath) ? json(expectedPath.string()) : json(nullptr)},
      {"diff", WriteBitmapPng(diffBitmap, diffPath) ? json(diffPath.string()) : json(nullptr)},
  };
}

std::optional<std::vector<uint8_t>> ReadBinaryFile(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return std::nullopt;
  }

  return std::vector<uint8_t>(std::istreambuf_iterator<char>(stream),
                              std::istreambuf_iterator<char>());
}

std::optional<std::string> ReadTextFile(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return std::nullopt;
  }

  return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

int AttachPngFile(ToolCallResult* out, const std::string& label, const std::filesystem::path& path,
                  bool embedBase64, json* metadata) {
  std::optional<std::vector<uint8_t>> bytes = ReadBinaryFile(path);
  if (!bytes.has_value()) {
    (*metadata)["png_attached"] = false;
    return -1;
  }

  const std::string base64 = Base64Encode(*bytes);
  const int imageIndex = static_cast<int>(out->images.size());
  out->images.push_back(EncodedImage{
      .label = label,
      .mimeType = "image/png",
      .dataBase64 = base64,
  });
  (*metadata)["png_attached"] = true;
  (*metadata)["image_index"] = imageIndex;
  if (embedBase64) {
    (*metadata)["png_base64"] = base64;
  }
  return imageIndex;
}

bool DrainWritebackAndReparseSource(EditorApp* app, SelectTool* selectTool, std::string* source) {
  std::optional<SelectTool::CompletedDragWriteback> completed =
      selectTool->consumeCompletedDragWriteback();
  if (!completed.has_value()) {
    return false;
  }

  std::vector<TextPatch> patches;
  patches.reserve(1u + completed->extras.size());

  const auto appendPatchForTarget = [&](const AttributeWritebackTarget& target,
                                        const Transform2d& transform) {
    const RcString serialized = toSVGTransformString(transform);
    std::optional<TextPatch> patch;
    if (std::string_view(serialized).empty()) {
      patch = buildAttributeRemoveWriteback(*source, target, "transform");
    } else {
      patch = buildAttributeWriteback(*source, target, "transform", std::string_view(serialized));
    }
    if (patch.has_value()) {
      patches.push_back(*patch);
    }
  };

  appendPatchForTarget(completed->target, completed->transform);
  for (const SelectTool::CompletedDragWriteback& extra : completed->extras) {
    appendPatchForTarget(extra.target, extra.transform);
  }
  if (patches.empty()) {
    return false;
  }

  applyPatches(*source, patches);
  app->applyMutation(EditorCommand::ReplaceDocumentCommand(*source,
                                                           /*preserveUndoOnReparse=*/true));
  return true;
}

std::string_view ReproEventKindName(repro::ReproEvent::Kind kind) {
  switch (kind) {
    case repro::ReproEvent::Kind::MouseDown: return "mdown";
    case repro::ReproEvent::Kind::MouseUp: return "mup";
    case repro::ReproEvent::Kind::KeyDown: return "kdown";
    case repro::ReproEvent::Kind::KeyUp: return "kup";
    case repro::ReproEvent::Kind::Char: return "char";
    case repro::ReproEvent::Kind::Wheel: return "wheel";
    case repro::ReproEvent::Kind::Resize: return "resize";
    case repro::ReproEvent::Kind::Focus: return "focus";
  }

  return "unknown";
}

json ReproEventsJson(std::span<const repro::ReproEvent> events) {
  json out = json::array();
  for (const repro::ReproEvent& event : events) {
    json eventJson{
        {"kind", ReproEventKindName(event.kind)},
        {"mouse_button", event.mouseButton},
        {"key", event.key},
        {"modifiers", event.modifiers},
    };
    if (event.hit.has_value()) {
      eventJson["hit"] = json{
          {"id", event.hit->id},
          {"tag", event.hit->tag},
          {"doc_order_index", event.hit->docOrderIndex},
          {"empty", event.hit->empty},
      };
    } else {
      eventJson["hit"] = nullptr;
    }
    out.push_back(std::move(eventJson));
  }

  return out;
}

std::string_view ReproActionKindName(repro::ReproAction::Kind kind) {
  switch (kind) {
    case repro::ReproAction::Kind::SetActiveTool: return "active_tool";
    case repro::ReproAction::Kind::SetStyleProperty: return "style";
    case repro::ReproAction::Kind::CommitPenPath: return "commit_pen_path";
  }

  return "unknown";
}

json ReproActionsJson(std::span<const repro::ReproAction> actions) {
  json out = json::array();
  for (const repro::ReproAction& action : actions) {
    json actionJson{
        {"kind", ReproActionKindName(action.kind)},
    };
    if (!action.tool.empty()) {
      actionJson["tool"] = action.tool;
    }
    if (!action.propertyName.empty()) {
      actionJson["property"] = action.propertyName;
      actionJson["value"] = action.propertyValue;
    }
    out.push_back(std::move(actionJson));
  }
  return out;
}

}  // namespace

ToolCallResult EditorControlSession::startRnrRecording(const json& arguments) {
  std::string error;
  if (!ensureDocumentLoaded(&error)) {
    return MakeErrorResult(error);
  }
  if (rnrRecording_.active) {
    return MakeErrorResult("an .rnr recording is already active");
  }

  std::string outputPath;
  std::string svgPath;
  int windowWidth = canvasWidth_;
  int windowHeight = canvasHeight_;
  double displayScale = devicePixelRatio_;
  double frameDeltaMs = 1000.0 / 60.0;
  if (!ReadOptionalString(arguments, "output_path", "", &outputPath, &error) ||
      !ReadOptionalString(arguments, "svg_path", currentSourcePath_, &svgPath, &error) ||
      !ReadOptionalInt(arguments, "window_width", canvasWidth_, &windowWidth, &error) ||
      !ReadOptionalInt(arguments, "window_height", canvasHeight_, &windowHeight, &error) ||
      !ReadOptionalDouble(arguments, "display_scale", devicePixelRatio_, &displayScale, &error) ||
      !ReadOptionalDouble(arguments, "frame_delta_ms", 1000.0 / 60.0, &frameDeltaMs, &error)) {
    return MakeErrorResult(error);
  }
  if (svgPath.empty() || svgPath == "<memory>") {
    svgPath = "embedded.svg";
  }

  rnrRecording_ = RnrRecordingState{};
  rnrRecording_.active = true;
  if (!outputPath.empty()) {
    rnrRecording_.outputPath = std::filesystem::path(outputPath);
  }
  rnrRecording_.frameDeltaMs = frameDeltaMs > 0.0 ? frameDeltaMs : 1000.0 / 60.0;
  rnrRecording_.file.metadata.svgPath = svgPath;
  rnrRecording_.file.metadata.svgBasename = std::filesystem::path(svgPath).filename().string();
  rnrRecording_.file.metadata.svgContentHash = ReproContentHash(currentSourceText_);
  rnrRecording_.file.metadata.svgSource = currentSourceText_;
  rnrRecording_.file.metadata.windowWidth = windowWidth > 0 ? windowWidth : canvasWidth_;
  rnrRecording_.file.metadata.windowHeight = windowHeight > 0 ? windowHeight : canvasHeight_;
  rnrRecording_.file.metadata.displayScale = displayScale > 0.0 ? displayScale : 1.0;

  ToolCallResult out;
  out.body = json{
      {"ok", true},
      {"active", true},
      {"output_path", outputPath.empty() ? nullptr : json(outputPath)},
      {"svg_path", rnrRecording_.file.metadata.svgPath},
      {"svg_basename", rnrRecording_.file.metadata.svgBasename},
      {"svg_hash", rnrRecording_.file.metadata.svgContentHash},
      {"embedded_svg_source", rnrRecording_.file.metadata.svgSource.has_value()},
      {"frame_count", rnrRecording_.file.frames.size()},
  };
  return out;
}

ToolCallResult EditorControlSession::stopRnrRecording(const json& arguments) {
  if (!rnrRecording_.active) {
    return MakeErrorResult("no .rnr recording is active");
  }

  std::string error;
  std::string outputPath;
  bool writeFile = true;
  if (!ReadOptionalString(arguments, "output_path", "", &outputPath, &error) ||
      !ReadOptionalBool(arguments, "write_file", true, &writeFile, &error)) {
    return MakeErrorResult(error);
  }

  std::optional<std::filesystem::path> path = rnrRecording_.outputPath;
  if (!outputPath.empty()) {
    path = std::filesystem::path(outputPath);
  }

  const std::size_t frameCount = rnrRecording_.file.frames.size();
  bool wroteFile = false;
  if (writeFile) {
    if (!path.has_value()) {
      return MakeErrorResult(
          "stop_rnr_recording requires output_path when the recording was "
          "started without one");
    }
    if (!repro::WriteReproFile(*path, rnrRecording_.file)) {
      return MakeErrorResult("failed to write .rnr file: " + path->string());
    }
    wroteFile = true;
  }

  ToolCallResult out;
  out.body = json{
      {"ok", true},
      {"active", false},
      {"frame_count", frameCount},
      {"wrote_file", wroteFile},
      {"output_path", path.has_value() ? json(path->string()) : nullptr},
  };
  rnrRecording_ = RnrRecordingState{};
  return out;
}

ToolCallResult EditorControlSession::rnrRecordingState(const json&) const {
  ToolCallResult out;
  out.body = json{
      {"ok", true},
      {"active", rnrRecording_.active},
      {"frame_count", rnrRecording_.file.frames.size()},
      {"output_path",
       rnrRecording_.outputPath.has_value() ? json(rnrRecording_.outputPath->string()) : nullptr},
      {"svg_path", rnrRecording_.file.metadata.svgPath},
      {"svg_basename", rnrRecording_.file.metadata.svgBasename},
      {"svg_hash", rnrRecording_.file.metadata.svgContentHash},
      {"embedded_svg_source", rnrRecording_.file.metadata.svgSource.has_value()},
  };
  return out;
}

ToolCallResult EditorControlSession::replayRnr(const json& arguments) {
  std::string error;
  std::string rnrPathString;
  std::string svgPathOverride;
  bool renderEachFrame = true;
  bool simulateEditorShellFrameLoop = false;
  bool glReadback = false;
  bool includeGlImages = true;
  bool glVisible = false;
  bool glPace = true;
  bool glDriveDocumentInput = false;
  int glCaptureFrame = -1;
  int glCaptureLeftMouseDown = 0;
  int glMaxFrame = -1;
  int glTimeoutMs = 120000;
  std::string glCrop = "full";
  std::string glOutputDir;
  bool includeFrameResults = true;
  bool includeDisplayDiff = false;
  int maxFrameResults = 200;
  int stopAfterMouseUps = 0;
  int comparePresentedAfterLeftMouseDown = 0;
  int comparePresentedFrameOffsetAfterLeftMouseDown = 0;
  CaptureOptions capture;
  if (!ReadRequiredString(arguments, "rnr_path", &rnrPathString, &error) ||
      !ReadOptionalString(arguments, "svg_path", "", &svgPathOverride, &error) ||
      !ReadOptionalBool(arguments, "render_each_frame", true, &renderEachFrame, &error) ||
      !ReadOptionalBool(arguments, "simulate_editor_shell_frame_loop", false,
                        &simulateEditorShellFrameLoop, &error) ||
      !ReadOptionalBool(arguments, "gl_readback", false, &glReadback, &error) ||
      !ReadOptionalBool(arguments, "include_gl_images", true, &includeGlImages, &error) ||
      !ReadOptionalBool(arguments, "gl_visible", false, &glVisible, &error) ||
      !ReadOptionalBool(arguments, "gl_pace", true, &glPace, &error) ||
      !ReadOptionalBool(arguments, "gl_drive_document_input", false, &glDriveDocumentInput,
                        &error) ||
      !ReadOptionalInt(arguments, "gl_capture_frame", -1, &glCaptureFrame, &error) ||
      !ReadOptionalInt(arguments, "gl_capture_left_mousedown", 0, &glCaptureLeftMouseDown,
                       &error) ||
      !ReadOptionalInt(arguments, "gl_max_frame", -1, &glMaxFrame, &error) ||
      !ReadOptionalInt(arguments, "gl_timeout_ms", 120000, &glTimeoutMs, &error) ||
      !ReadOptionalString(arguments, "gl_crop", "full", &glCrop, &error) ||
      !ReadOptionalString(arguments, "gl_output_dir", "", &glOutputDir, &error) ||
      !ReadOptionalBool(arguments, "include_frame_results", true, &includeFrameResults, &error) ||
      !ReadOptionalBool(arguments, "include_display_diff", false, &includeDisplayDiff, &error) ||
      !ReadOptionalInt(arguments, "max_frame_results", 200, &maxFrameResults, &error) ||
      !ReadOptionalInt(arguments, "stop_after_mouse_ups", 0, &stopAfterMouseUps, &error) ||
      !ReadOptionalInt(arguments, "compare_presented_after_left_mouse_down", 0,
                       &comparePresentedAfterLeftMouseDown, &error) ||
      !ReadOptionalInt(arguments, "compare_presented_frame_offset_after_left_mouse_down", 0,
                       &comparePresentedFrameOffsetAfterLeftMouseDown, &error) ||
      !ReadCaptureOptions(arguments, false, &capture, &error)) {
    return MakeErrorResult(error);
  }

  if (glReadback) {
    const std::optional<repro::GlRnrReplayCropMode> cropMode =
        repro::ParseGlRnrReplayCropMode(glCrop);
    if (!cropMode.has_value()) {
      return MakeErrorResult("gl_crop must be one of: full, render-pane, document-canvas");
    }
    if (glCaptureFrame < -1) {
      return MakeErrorResult("gl_capture_frame must be non-negative");
    }
    if (glCaptureLeftMouseDown < 0) {
      return MakeErrorResult("gl_capture_left_mousedown must be positive");
    }
    if (glMaxFrame < -1) {
      return MakeErrorResult("gl_max_frame must be non-negative");
    }
    if (glTimeoutMs <= 0) {
      return MakeErrorResult("gl_timeout_ms must be positive");
    }

    repro::GlRnrReplayOptions replayOptions;
    replayOptions.rnrPath = std::filesystem::path(rnrPathString);
    if (!svgPathOverride.empty()) {
      replayOptions.svgPathOverride = std::filesystem::path(svgPathOverride);
    }
    replayOptions.outputDir = glOutputDir.empty() ? (DiagnosticOutputDir() / "gl_rnr_replay")
                                                  : std::filesystem::path(glOutputDir);
    if (glCaptureFrame >= 0) {
      replayOptions.captureFrames.insert(static_cast<std::uint64_t>(glCaptureFrame));
      if (glMaxFrame < 0) {
        replayOptions.maxFrame = static_cast<std::uint64_t>(glCaptureFrame);
      }
    }
    if (glCaptureLeftMouseDown > 0) {
      replayOptions.captureLeftMouseDownOrdinal = glCaptureLeftMouseDown;
    }
    if (glMaxFrame >= 0) {
      replayOptions.maxFrame = static_cast<std::uint64_t>(glMaxFrame);
    }
    replayOptions.cropMode = *cropMode;
    replayOptions.pace = glPace;
    replayOptions.visible = glVisible;
    replayOptions.driveDocumentSpaceInput = glDriveDocumentInput;

    GlReadbackRunner glReadbackRunner = GlReadbackRunner::InProcess;
    if (!SelectGlReadbackRunner(&glReadbackRunner, &error)) {
      return MakeErrorResult(error);
    }

    repro::GlRnrReplayResult replayResult;
    switch (glReadbackRunner) {
      case GlReadbackRunner::InProcess:
        if (!repro::RunGlRnrReplay(replayOptions, &replayResult, &error)) {
          return MakeErrorResult(error);
        }
        break;
      case GlReadbackRunner::BazelRun:
        if (!RunBazelGlRnrReplay(replayOptions, std::chrono::milliseconds(glTimeoutMs),
                                 &replayResult, &error)) {
          return MakeErrorResult(error);
        }
        break;
    }

    ToolCallResult out;
    out.body = json{
        {"ok", true},
        {"mode", "gl_readback"},
        {"gl_readback_runner", GlReadbackRunnerName(glReadbackRunner)},
        {"rnr_path", rnrPathString},
        {"svg_path", svgPathOverride.empty() ? json(nullptr) : json(svgPathOverride)},
        {"output_dir", replayOptions.outputDir.string()},
        {"crop", glCrop},
        {"gl_drive_document_input", replayOptions.driveDocumentSpaceInput},
        {"capture_count", replayResult.captures.size()},
        {"captures", json::array()},
    };
    for (const repro::GlRnrReplayCapture& captureResult : replayResult.captures) {
      json captureJson{
          {"frame_index", captureResult.frameIndex},
          {"reason", captureResult.reason},
          {"path", captureResult.path.string()},
      };
      if (includeGlImages) {
        AttachPngFile(&out,
                      "replay_rnr_gl_frame_" + std::to_string(captureResult.frameIndex) + "_" +
                          captureResult.reason,
                      captureResult.path, capture.embedPngBase64, &captureJson);
      }
      out.body["captures"].push_back(std::move(captureJson));
    }
    out.body["attached_image_count"] = out.images.size();
    return out;
  }

  const std::filesystem::path rnrPath(rnrPathString);
  std::optional<repro::ReproFile> replay = repro::ReadReproFile(rnrPath);
  if (!replay.has_value()) {
    return MakeErrorResult("failed to read .rnr file: " + rnrPathString);
  }

  std::filesystem::path svgDisplayPath;
  std::string liveSource;
  bool usedEmbeddedSvgSource = false;
  if (!svgPathOverride.empty()) {
    svgDisplayPath = std::filesystem::path(svgPathOverride);
    std::optional<std::string> source = ReadTextFile(svgDisplayPath);
    if (!source.has_value()) {
      return MakeErrorResult("failed to open SVG for .rnr replay: " + svgDisplayPath.string());
    }
    liveSource = std::move(*source);
  } else if (replay->metadata.svgSource.has_value()) {
    svgDisplayPath = !replay->metadata.svgBasename.empty()
                         ? std::filesystem::path(replay->metadata.svgBasename)
                         : std::filesystem::path(replay->metadata.svgPath).filename();
    if (svgDisplayPath.empty()) {
      svgDisplayPath = "embedded.svg";
    }
    liveSource = *replay->metadata.svgSource;
    usedEmbeddedSvgSource = true;
  } else {
    std::optional<std::filesystem::path> resolvedSvgPath =
        resolveRnrSvgPath(rnrPath, replay->metadata.svgPath);
    if (!resolvedSvgPath.has_value()) {
      return MakeErrorResult("failed to resolve SVG path from .rnr metadata: " +
                             replay->metadata.svgPath);
    }
    svgDisplayPath = *resolvedSvgPath;
    std::optional<std::string> source = ReadTextFile(svgDisplayPath);
    if (!source.has_value()) {
      return MakeErrorResult("failed to open SVG for .rnr replay: " + svgDisplayPath.string());
    }
    liveSource = std::move(*source);
  }

  LoadOptions loadOptions;
  loadOptions.canvasWidth =
      replay->metadata.windowWidth > 0 ? replay->metadata.windowWidth : kDefaultCanvasWidth;
  loadOptions.canvasHeight =
      replay->metadata.windowHeight > 0 ? replay->metadata.windowHeight : kDefaultCanvasHeight;
  loadOptions.devicePixelRatio =
      replay->metadata.displayScale > 0.0 ? replay->metadata.displayScale : 1.0;
  loadOptions.renderAfterLoad = false;
  ToolCallResult load = loadSvgSource(liveSource, loadOptions, svgDisplayPath.string());
  if (load.isError || !load.body.value("ok", false)) {
    return load;
  }

  ViewportState viewport;
  viewport.devicePixelRatio = loadOptions.devicePixelRatio;
  viewport.paneOrigin = Vector2d::Zero();
  viewport.paneSize = Vector2d(static_cast<double>(loadOptions.canvasWidth),
                               static_cast<double>(loadOptions.canvasHeight));
  viewport.documentViewBox = DocumentViewBoxOr(
      app_.document().document(),
      Box2d::FromXYWH(0.0, 0.0, loadOptions.canvasWidth, loadOptions.canvasHeight));
  viewport.resetTo100Percent();
  for (const repro::ReproFrame& frame : replay->frames) {
    if (frame.viewport.has_value()) {
      ApplyReproViewport(&viewport, *frame.viewport);
      break;
    }
  }
  (void)syncCanvasSize(viewport);
  app_.flushFrame();

  ToolCallResult out;
  out.body = json{
      {"ok", true},
      {"rnr_path", rnrPathString},
      {"svg_path", svgDisplayPath.string()},
      {"embedded_svg_source", usedEmbeddedSvgSource},
      {"svg_hash", replay->metadata.svgContentHash},
      {"frame_count", replay->frames.size()},
      {"rendered_frame_count", 0},
      {"mouse_up_count", 0},
      {"stopped_early", false},
      {"frames", json::array()},
  };

  if (simulateEditorShellFrameLoop) {
    out.body["mode"] = "editor_shell_frame_loop";

    struct PendingClick {
      Vector2d documentPoint = Vector2d::Zero();
      MouseModifiers modifiers;
      int leftMouseDownOrdinal = 0;
      bool dispatched = false;
    };

    std::optional<PendingClick> pendingClick;
    bool leftButtonHeld = false;
    int processedFrameCount = 0;
    int skippedIdleFrameCount = 0;
    int renderedFrameCount = 0;
    int storedFrameResults = 0;
    int mouseUpCount = 0;
    int leftMouseDownCount = 0;
    int targetLeftMouseDownFrameOffset = -1;
    bool comparedTargetPresentedFrame = false;
    PresentationRenderScheduler renderScheduler;

    const auto pollRenderResult = [&]() {
      std::optional<RenderResult> result = asyncRenderer_.pollResult();
      if (!result.has_value()) {
        return false;
      }

      recordDisplayFrame(*result);
      renderScheduler.noteRenderCompleted(result->version, asyncRenderer_.lastDocumentCanvasSize(),
                                          result->rasterViewport);
      ++renderedFrameCount;
      return true;
    };

    const auto waitUntilIdleAndRecord = [&]() {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
      while (std::chrono::steady_clock::now() < deadline) {
        const bool polled = pollRenderResult();
        if (!asyncRenderer_.isBusy()) {
          while (pollRenderResult()) {}
          return true;
        }
        if (!polled) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      }
      return false;
    };

    const auto maybeRequestGuiRender = [&]() {
      if (asyncRenderer_.isBusy() || !app_.hasDocument() || viewport.paneSize.x <= 0.0 ||
          viewport.paneSize.y <= 0.0) {
        return false;
      }

      (void)syncCanvasSize(viewport);
      (void)app_.flushFrame();

      const Vector2i currentCanvasSize = app_.document().document().canvasSize();
      const std::uint64_t currentVersion = app_.document().currentFrameVersion();
      const std::optional<SelectTool::ActiveDragPreview> dragPreview =
          selectTool_->activeDragPreview();
      const Entity selectedEntity = SelectedGraphicsEntity(app_);
      const PresentationRenderScheduleDecision schedule =
          renderScheduler.evaluate(displayPresentation_, PresentationRenderScheduleInput{
                                                             .selectedEntity = selectedEntity,
                                                             .activeDragPreview = dragPreview,
                                                             .currentVersion = currentVersion,
                                                             .currentCanvasSize = currentCanvasSize,
                                                         });
      if (!schedule.shouldRequestRender()) {
        return false;
      }

      RenderRequest request(renderer_, app_.document().document());
      request.captureCpuSnapshot = true;
      request.version = currentVersion;
      request.documentGeneration = app_.document().documentGeneration();
      request.structuralRemap = app_.document().consumePendingStructuralRemap();
      if (app_.selectedElement().has_value()) {
        request.selection = *app_.selectedElement();
      }
      request.selectedEntity = selectedEntity;
      if (schedule.dragPreview.has_value()) {
        request.dragPreview = *schedule.dragPreview;
      }
      asyncRenderer_.requestRender(request);
      return true;
    };

    const auto replayStart = std::chrono::steady_clock::now();
    for (const repro::ReproFrame& frame : replay->frames) {
      const auto targetTime =
          replayStart + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                            std::chrono::duration<double>(frame.timestampSeconds));
      while (std::chrono::steady_clock::now() < targetTime) {
        (void)pollRenderResult();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }

      ++processedFrameCount;
      const bool polledAtFrameStart = pollRenderResult();
      if (!asyncRenderer_.isBusy()) {
        (void)DrainWritebackAndReparseSource(&app_, selectTool_.get(), &liveSource);
        (void)app_.flushFrame();
      }

      if (frame.viewport.has_value()) {
        ApplyReproViewport(&viewport, *frame.viewport);
      }
      const bool preInputRenderRequested = maybeRequestGuiRender();

      const Vector2d mouseScreen(frame.mouseX, frame.mouseY);
      const Vector2d mouseDoc = frame.mouseDocX.has_value() && frame.mouseDocY.has_value()
                                    ? Vector2d(*frame.mouseDocX, *frame.mouseDocY)
                                    : viewport.screenToDocument(mouseScreen);
      const bool nowHeld = (frame.mouseButtonMask & 1) != 0;
      bool frameHadAction = false;
      bool frameHadLeftMouseDown = false;
      bool cancelledForPendingClick = false;
      if (!frame.actions.empty() && asyncRenderer_.isBusy()) {
        asyncRenderer_.cancelInFlight();
        cancelledForPendingClick = true;
      }
      if (!asyncRenderer_.isBusy()) {
        for (const repro::ReproAction& action : frame.actions) {
          if (!applyReproAction(action, &error)) {
            return MakeErrorResult("action failed while replaying .rnr frame " +
                                   std::to_string(frame.index) + ": " + error);
          }
          frameHadAction = true;
        }
        if (frameHadAction) {
          (void)app_.flushFrame();
          syncSourceTextFromDocumentIfChanged();
        }
      }
      for (const repro::ReproEvent& event : frame.events) {
        if (event.kind == repro::ReproEvent::Kind::MouseDown && event.mouseButton == 0) {
          ++leftMouseDownCount;
          frameHadLeftMouseDown = true;
          pendingClick = PendingClick{
              .documentPoint = mouseDoc,
              .modifiers =
                  MouseModifiers{
                      .shift = (frame.modifiers & 2) != 0,
                      .option = (frame.modifiers & 4) != 0,
                      .pixelsPerDocUnit = viewport.pixelsPerDocUnit(),
                  },
              .leftMouseDownOrdinal = leftMouseDownCount,
          };
        }
      }
      if (comparePresentedAfterLeftMouseDown > 0 && !comparedTargetPresentedFrame) {
        if (frameHadLeftMouseDown && leftMouseDownCount == comparePresentedAfterLeftMouseDown) {
          targetLeftMouseDownFrameOffset = 0;
        } else if (targetLeftMouseDownFrameOffset >= 0) {
          ++targetLeftMouseDownFrameOffset;
        }
      }

      if (pendingClick.has_value() && !pendingClick->dispatched && asyncRenderer_.isBusy()) {
        asyncRenderer_.cancelInFlight();
        cancelledForPendingClick = true;
      }

      if (!asyncRenderer_.isBusy()) {
        if (pendingClick.has_value() && !pendingClick->dispatched) {
          replayMouseDown(pendingClick->documentPoint, pendingClick->modifiers);
          pendingClick->dispatched = true;
        }

        for (const repro::ReproEvent& event : frame.events) {
          if (event.kind == repro::ReproEvent::Kind::MouseUp && event.mouseButton == 0) {
            replayMouseUp(mouseDoc);
            ++mouseUpCount;
          }
        }

        if ((selectTool_->isDragging() || penTool_.isDraggingAnchor()) && nowHeld &&
            leftButtonHeld) {
          MouseModifiers modifiers;
          modifiers.shift = (frame.modifiers & 2) != 0;
          modifiers.option = (frame.modifiers & 4) != 0;
          modifiers.pixelsPerDocUnit = viewport.pixelsPerDocUnit();
          replayMouseMove(mouseDoc, /*buttonHeld=*/true, modifiers);
        }
        (void)app_.flushFrame();
      } else {
        for (const repro::ReproEvent& event : frame.events) {
          if (event.kind == repro::ReproEvent::Kind::MouseUp && event.mouseButton == 0) {
            ++mouseUpCount;
          }
        }
      }

      leftButtonHeld = nowHeld;
      const bool postInputRenderRequested = maybeRequestGuiRender();
      const DisplayFrameSnapshot presentedFrame = currentDisplayFrame();
      const Box2d viewBox = DocumentViewBoxOr(
          app_.document().document(), Box2d::FromXYWH(0.0, 0.0, canvasWidth_, canvasHeight_));
      const std::optional<svg::RendererBitmap> presentedBitmap =
          displayTextures_.composeDisplayFrame(presentedFrame, viewBox,
                                               app_.document().document().canvasSize());

      json frameJson{
          {"frame_index", frame.index},
          {"mouse_doc", VectorToJson(mouseDoc)},
          {"actions", ReproActionsJson(frame.actions)},
          {"events", ReproEventsJson(frame.events)},
          {"had_action", frameHadAction},
          {"left_mouse_down_ordinal", frameHadLeftMouseDown ? leftMouseDownCount : 0},
          {"left_mouse_down_compare_offset", targetLeftMouseDownFrameOffset >= 0
                                                 ? json(targetLeftMouseDownFrameOffset)
                                                 : json(nullptr)},
          {"worker_busy_after_frame", asyncRenderer_.isBusy()},
          {"pending_click",
           pendingClick.has_value()
               ? json{{"left_mouse_down_ordinal", pendingClick->leftMouseDownOrdinal},
                      {"dispatched", pendingClick->dispatched}}
               : json(nullptr)},
          {"polled_render_result_at_frame_start", polledAtFrameStart},
          {"pre_input_render_requested", preInputRenderRequested},
          {"post_input_render_requested", postInputRenderRequested},
          {"cancelled_render_for_pending_click", cancelledForPendingClick},
          {"selection", selectedElementJson()},
          {"presented_frame", DisplayFrameJson(presentedFrame)},
          {"presented_frame_bitmap",
           presentedBitmap.has_value() ? BitmapSummary(*presentedBitmap) : json(nullptr)},
      };

      const bool shouldComparePresentedFrame =
          comparePresentedAfterLeftMouseDown > 0 && !comparedTargetPresentedFrame &&
          targetLeftMouseDownFrameOffset == comparePresentedFrameOffsetAfterLeftMouseDown;
      if (shouldComparePresentedFrame && includeDisplayDiff) {
        if (!waitUntilIdleAndRecord()) {
          return MakeErrorResult(
              "timed out waiting for async renderer before presented-frame "
              "comparison");
        }
        if (pendingClick.has_value() && !pendingClick->dispatched) {
          selectTool_->onMouseDown(app_, pendingClick->documentPoint, pendingClick->modifiers);
          pendingClick->dispatched = true;
          (void)app_.flushFrame();
        }

        std::vector<CapturedRenderResult> renderResults;
        if (!renderCurrentFrame(&renderResults, &error)) {
          return MakeErrorResult("final render failed for presented-frame comparison: " + error);
        }
        ++renderedFrameCount;
        const DisplayFrameSnapshot* finalDisplayFrame = nullptr;
        for (const CapturedRenderResult& result : renderResults) {
          finalDisplayFrame = &result.displayFrame;
        }
        if (finalDisplayFrame != nullptr) {
          const std::optional<svg::RendererBitmap> finalPresentedBitmap =
              displayTextures_.composeDisplayFrame(*finalDisplayFrame, viewBox,
                                                   app_.document().document().canvasSize());
          frameJson["eventual_final_presented_frame"] = DisplayFrameJson(*finalDisplayFrame);
          frameJson["eventual_final_presented_frame_bitmap"] =
              finalPresentedBitmap.has_value() ? BitmapSummary(*finalPresentedBitmap)
                                               : json(nullptr);
          if (finalPresentedBitmap.has_value()) {
            json diff = BitmapDiffSummary(presentedBitmap, *finalPresentedBitmap);
            AddBitmapDiffArtifacts(&diff, presentedBitmap, *finalPresentedBitmap,
                                   "replay_rnr_gui_frame_" + std::to_string(frame.index));
            frameJson["presented_frame_diff_from_eventual_final"] = std::move(diff);
          } else {
            frameJson["presented_frame_diff_from_eventual_final"] = json{
                {"available", false},
                {"actual_bitmap",
                 presentedBitmap.has_value() ? BitmapSummary(*presentedBitmap) : json(nullptr)},
                {"expected_bitmap", nullptr},
            };
          }
        }
        out.body["stopped_early"] = true;
        comparedTargetPresentedFrame = true;
      }

      if (includeFrameResults && storedFrameResults < maxFrameResults) {
        out.body["frames"].push_back(std::move(frameJson));
        ++storedFrameResults;
      }

      if (shouldComparePresentedFrame) {
        break;
      }
      if (stopAfterMouseUps > 0 && mouseUpCount >= stopAfterMouseUps) {
        out.body["stopped_early"] = true;
        break;
      }
    }

    out.body["processed_frame_count"] = processedFrameCount;
    out.body["skipped_idle_frame_count"] = skippedIdleFrameCount;
    out.body["rendered_frame_count"] = renderedFrameCount;
    out.body["stored_frame_result_count"] = storedFrameResults;
    out.body["mouse_up_count"] = mouseUpCount;
    out.body["left_mouse_down_count"] = leftMouseDownCount;
    out.body["final_selection"] = selectedElementJson();
    out.body["attached_image_count"] = out.images.size();
    return out;
  }

  bool leftButtonHeld = false;
  int processedFrameCount = 0;
  int skippedIdleFrameCount = 0;
  int renderedFrameCount = 0;
  int storedFrameResults = 0;
  int mouseUpCount = 0;
  for (const repro::ReproFrame& frame : replay->frames) {
    bool frameNeedsRender = drainPendingWritebacks();
    const bool nowHeld = (frame.mouseButtonMask & 1) != 0;
    const bool viewportUnchanged =
        !frame.viewport.has_value() || ReproViewportMatches(viewport, *frame.viewport);
    if (!frameNeedsRender && !nowHeld && !leftButtonHeld && frame.actions.empty() &&
        frame.events.empty() && viewportUnchanged) {
      ++skippedIdleFrameCount;
      continue;
    }
    ++processedFrameCount;

    if (frame.viewport.has_value()) {
      ApplyReproViewport(&viewport, *frame.viewport);
      frameNeedsRender |= syncCanvasSize(viewport);
    }

    const Vector2d mouseScreen(frame.mouseX, frame.mouseY);
    const Vector2d mouseDoc = frame.mouseDocX.has_value() && frame.mouseDocY.has_value()
                                  ? Vector2d(*frame.mouseDocX, *frame.mouseDocY)
                                  : viewport.screenToDocument(mouseScreen);

    for (const repro::ReproAction& action : frame.actions) {
      if (!applyReproAction(action, &error)) {
        return MakeErrorResult("action failed while replaying .rnr frame " +
                               std::to_string(frame.index) + ": " + error);
      }
      frameNeedsRender = true;
    }

    MouseModifiers modifiers;
    modifiers.shift = (frame.modifiers & 2) != 0;
    modifiers.option = (frame.modifiers & 4) != 0;
    modifiers.pixelsPerDocUnit = viewport.pixelsPerDocUnit();
    for (const repro::ReproEvent& event : frame.events) {
      if (event.kind == repro::ReproEvent::Kind::MouseDown && event.mouseButton == 0) {
        replayMouseDown(mouseDoc, modifiers);
        frameNeedsRender = true;
      } else if (event.kind == repro::ReproEvent::Kind::MouseUp && event.mouseButton == 0) {
        replayMouseUp(mouseDoc);
        ++mouseUpCount;
        frameNeedsRender = true;
      }
    }

    if (nowHeld && leftButtonHeld) {
      replayMouseMove(mouseDoc, /*buttonHeld=*/true, modifiers);
      frameNeedsRender = true;
    }
    leftButtonHeld = nowHeld;
    frameNeedsRender |= app_.flushFrame();
    syncSourceTextFromDocumentIfChanged();

    if (renderEachFrame && frameNeedsRender) {
      const DisplayFrameSnapshot displayBeforeRender = currentDisplayFrame();
      const Box2d viewBox = DocumentViewBoxOr(
          app_.document().document(), Box2d::FromXYWH(0.0, 0.0, canvasWidth_, canvasHeight_));
      const std::optional<svg::RendererBitmap> displayBeforeRenderBitmap =
          includeDisplayDiff
              ? displayTextures_.composeDisplayFrame(displayBeforeRender, viewBox,
                                                     app_.document().document().canvasSize())
              : std::nullopt;
      std::vector<CapturedRenderResult> renderResults;
      if (!renderCurrentFrame(&renderResults, &error)) {
        return MakeErrorResult("render failed while replaying .rnr frame " +
                               std::to_string(frame.index) + ": " + error);
      }
      ++renderedFrameCount;
      if (includeFrameResults && storedFrameResults < maxFrameResults) {
        const DisplayFrameSnapshot* finalDisplayFrame = nullptr;
        for (const CapturedRenderResult& result : renderResults) {
          finalDisplayFrame = &result.displayFrame;
        }
        std::optional<svg::RendererBitmap> finalDisplayBitmap;
        if (finalDisplayFrame != nullptr) {
          finalDisplayBitmap = displayTextures_.composeDisplayFrame(
              *finalDisplayFrame, viewBox, app_.document().document().canvasSize());
        }

        json frameJson{
            {"frame_index", frame.index},
            {"mouse_doc", VectorToJson(mouseDoc)},
            {"actions", ReproActionsJson(frame.actions)},
            {"events", ReproEventsJson(frame.events)},
            {"selection", selectedElementJson()},
            {"display_before_render", DisplayFrameJson(displayBeforeRender)},
            {"eventual_final_presented_frame",
             finalDisplayFrame != nullptr ? DisplayFrameJson(*finalDisplayFrame) : json(nullptr)},
            {"eventual_final_presented_frame_bitmap",
             finalDisplayBitmap.has_value() ? BitmapSummary(*finalDisplayBitmap) : json(nullptr)},
            {"stages", RenderResultsJson(renderResults, &out, capture,
                                         "replay_rnr/frame_" + std::to_string(frame.index))},
        };
        if (includeDisplayDiff && finalDisplayBitmap.has_value()) {
          json diff = BitmapDiffSummary(displayBeforeRenderBitmap, *finalDisplayBitmap);
          AddBitmapDiffArtifacts(
              &diff, displayBeforeRenderBitmap, *finalDisplayBitmap,
              "replay_rnr_frame_" + std::to_string(frame.index) + "_display_before_render");
          frameJson["display_before_render_diff_from_final"] = std::move(diff);
        }
        out.body["frames"].push_back(std::move(frameJson));
        ++storedFrameResults;
      }
    }

    if (stopAfterMouseUps > 0 && mouseUpCount >= stopAfterMouseUps) {
      out.body["stopped_early"] = true;
      break;
    }
  }

  if (!renderEachFrame) {
    std::vector<CapturedRenderResult> renderResults;
    if (!renderCurrentFrame(&renderResults, &error)) {
      return MakeErrorResult("final render failed after .rnr replay: " + error);
    }
    ++renderedFrameCount;
    if (includeFrameResults && storedFrameResults < maxFrameResults) {
      out.body["frames"].push_back(json{
          {"frame_index", nullptr},
          {"mouse_doc", nullptr},
          {"selection", selectedElementJson()},
          {"stages", RenderResultsJson(renderResults, &out, capture, "replay_rnr/final")},
      });
    }
  }

  out.body["processed_frame_count"] = processedFrameCount;
  out.body["skipped_idle_frame_count"] = skippedIdleFrameCount;
  out.body["rendered_frame_count"] = renderedFrameCount;
  out.body["stored_frame_result_count"] = storedFrameResults;
  out.body["mouse_up_count"] = mouseUpCount;
  out.body["final_selection"] = selectedElementJson();
  out.body["attached_image_count"] = out.images.size();
  return out;
}

void EditorControlSession::appendRnrFrame(const Vector2d& documentPoint, int mouseButtonMask,
                                          int modifierMask, std::vector<repro::ReproEvent> events) {
  if (!rnrRecording_.active) {
    return;
  }

  const Vector2d screenPoint = currentRecordingScreenPoint(documentPoint);
  repro::ReproFrame frame;
  frame.index = rnrRecording_.nextFrameIndex++;
  frame.timestampSeconds = rnrRecording_.timestampSeconds;
  frame.deltaMs = rnrRecording_.frameDeltaMs;
  frame.mouseX = screenPoint.x;
  frame.mouseY = screenPoint.y;
  frame.mouseDocX = documentPoint.x;
  frame.mouseDocY = documentPoint.y;
  frame.mouseButtonMask = mouseButtonMask;
  frame.modifiers = modifierMask;
  frame.viewport = currentReproViewport();
  frame.events = std::move(events);
  rnrRecording_.file.frames.push_back(std::move(frame));
  rnrRecording_.timestampSeconds += rnrRecording_.frameDeltaMs / 1000.0;
}

void EditorControlSession::appendRnrActionFrame(repro::ReproAction action) {
  if (!rnrRecording_.active) {
    return;
  }

  const repro::ReproViewport viewport = currentReproViewport();
  const Vector2d documentPoint(viewport.panDocX, viewport.panDocY);
  const Vector2d screenPoint = currentRecordingScreenPoint(documentPoint);

  repro::ReproFrame frame;
  frame.index = rnrRecording_.nextFrameIndex++;
  frame.timestampSeconds = rnrRecording_.timestampSeconds;
  frame.deltaMs = rnrRecording_.frameDeltaMs;
  frame.mouseX = screenPoint.x;
  frame.mouseY = screenPoint.y;
  frame.mouseDocX = documentPoint.x;
  frame.mouseDocY = documentPoint.y;
  frame.mouseButtonMask = 0;
  frame.modifiers = 0;
  frame.viewport = viewport;
  frame.actions.push_back(std::move(action));
  rnrRecording_.file.frames.push_back(std::move(frame));
  rnrRecording_.timestampSeconds += rnrRecording_.frameDeltaMs / 1000.0;
}

repro::ReproViewport EditorControlSession::currentReproViewport() const {
  const Box2d fallback = Box2d::FromXYWH(0.0, 0.0, canvasWidth_, canvasHeight_);
  const Box2d viewBox =
      app_.hasDocument() ? DocumentViewBoxOr(app_.document().document(), fallback) : fallback;
  const Vector2d viewBoxSize = viewBox.size();
  const double deviceScaleX =
      viewBoxSize.x > 0.0 ? static_cast<double>(canvasWidth_) / viewBoxSize.x : 1.0;
  const double zoom = devicePixelRatio_ > 0.0 ? deviceScaleX / devicePixelRatio_ : deviceScaleX;
  const Vector2d paneSize(viewBoxSize.x * zoom, viewBoxSize.y * zoom);
  const Vector2d panDoc = (viewBox.topLeft + viewBox.bottomRight) * 0.5;
  const Vector2d panScreen = paneSize * 0.5;

  repro::ReproViewport viewport;
  viewport.paneOriginX = 0.0;
  viewport.paneOriginY = 0.0;
  viewport.paneSizeW = paneSize.x;
  viewport.paneSizeH = paneSize.y;
  viewport.devicePixelRatio = devicePixelRatio_;
  viewport.zoom = zoom;
  viewport.panDocX = panDoc.x;
  viewport.panDocY = panDoc.y;
  viewport.panScreenX = panScreen.x;
  viewport.panScreenY = panScreen.y;
  viewport.viewBoxX = viewBox.topLeft.x;
  viewport.viewBoxY = viewBox.topLeft.y;
  viewport.viewBoxW = viewBox.width();
  viewport.viewBoxH = viewBox.height();
  return viewport;
}

Vector2d EditorControlSession::currentRecordingScreenPoint(const Vector2d& documentPoint) const {
  const repro::ReproViewport viewport = currentReproViewport();
  const Vector2d panDoc(viewport.panDocX, viewport.panDocY);
  const Vector2d panScreen(viewport.panScreenX, viewport.panScreenY);
  return panScreen + (documentPoint - panDoc) * viewport.zoom;
}

std::optional<std::filesystem::path> EditorControlSession::resolveRnrSvgPath(
    const std::filesystem::path& rnrPath, std::string_view recordingSvgPath) const {
  const std::filesystem::path direct(recordingSvgPath);
  std::error_code ec;
  if (std::filesystem::exists(direct, ec)) {
    return direct;
  }

  ec.clear();
  const std::filesystem::path alongside = rnrPath.parent_path() / direct;
  if (std::filesystem::exists(alongside, ec)) {
    return alongside;
  }

  ec.clear();
  const std::filesystem::path fromCurrent = std::filesystem::path(".") / direct;
  if (std::filesystem::exists(fromCurrent, ec)) {
    return fromCurrent;
  }

  return std::nullopt;
}

bool EditorControlSession::setActiveToolForReplay(std::string_view tool, std::string* error) {
  if (tool == "select") {
    if (activeReplayTool_ == ActiveReplayTool::Pen && penTool_.commitOpenPath(app_)) {
      (void)app_.flushFrame();
      syncSourceTextFromDocumentIfChanged();
    }
    activeReplayTool_ = ActiveReplayTool::Select;
    return true;
  }
  if (tool == "pen") {
    activeReplayTool_ = ActiveReplayTool::Pen;
    return true;
  }

  *error = "tool must be 'select' or 'pen'";
  return false;
}

bool EditorControlSession::applyStylePropertyForReplay(std::string_view propertyName,
                                                       std::string_view propertyValue) {
  if (propertyName == "fill") {
    app_.setActiveFill(propertyValue);
  } else if (propertyName == "stroke") {
    app_.setActiveStroke(propertyValue);
  } else if (propertyName == "stroke-width") {
    char* end = nullptr;
    const std::string value(propertyValue);
    const double strokeWidth = std::strtod(value.c_str(), &end);
    if (end != value.c_str()) {
      app_.setActiveStrokeWidth(strokeWidth);
    }
  }

  return app_.setStylePropertyOnSelection(propertyName, propertyValue);
}

bool EditorControlSession::applyReproAction(const repro::ReproAction& action, std::string* error) {
  switch (action.kind) {
    case repro::ReproAction::Kind::SetActiveTool: return setActiveToolForReplay(action.tool, error);
    case repro::ReproAction::Kind::SetStyleProperty:
      std::ignore = applyStylePropertyForReplay(action.propertyName, action.propertyValue);
      return true;
    case repro::ReproAction::Kind::CommitPenPath:
      std::ignore = penTool_.commitOpenPath(app_);
      return true;
  }

  *error = "unknown replay action";
  return false;
}

void EditorControlSession::replayMouseDown(const Vector2d& documentPoint,
                                           MouseModifiers modifiers) {
  if (activeReplayTool_ == ActiveReplayTool::Pen) {
    penTool_.onMouseDown(app_, documentPoint, modifiers);
  } else {
    selectTool_->onMouseDown(app_, documentPoint, modifiers);
  }
}

void EditorControlSession::replayMouseMove(const Vector2d& documentPoint, bool buttonHeld,
                                           MouseModifiers modifiers) {
  if (activeReplayTool_ == ActiveReplayTool::Pen) {
    penTool_.onMouseMove(app_, documentPoint, buttonHeld, modifiers);
  } else {
    selectTool_->onMouseMove(app_, documentPoint, buttonHeld, modifiers);
  }
}

void EditorControlSession::replayMouseUp(const Vector2d& documentPoint) {
  if (activeReplayTool_ == ActiveReplayTool::Pen) {
    penTool_.onMouseUp(app_, documentPoint);
  } else {
    selectTool_->onMouseUp(app_, documentPoint);
  }
}

}  // namespace donner::editor::mcp
