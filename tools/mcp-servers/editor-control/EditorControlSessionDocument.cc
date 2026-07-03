/// @file
/// EditorControlSession document/source tools: loading SVG files or source
/// bytes into the headless session and reading/patching the editable draft.

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/base/Box.h"
#include "donner/editor/AttributeWriteback.h"
#include "donner/editor/TextPatch.h"
#include "donner/editor/ViewportState.h"
#include "donner/svg/SVGDocument.h"
#include "nlohmann/json.hpp"
#include "tools/mcp-servers/editor-control/EditorControlSessionInternal.h"

namespace donner::editor::mcp {

namespace {

using nlohmann::json;

}  // namespace

ToolCallResult EditorControlSession::loadDocument(const json& arguments) {
  std::string error;
  std::string svgPath;
  if (!ReadRequiredString(arguments, "svg_path", &svgPath, &error)) {
    return MakeErrorResult(error);
  }

  std::ifstream file(svgPath, std::ios::binary);
  if (!file.is_open()) {
    return MakeErrorResult("failed to open SVG file: " + svgPath);
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();

  LoadOptions options;
  if (!ReadLoadOptions(arguments, &options, &error)) {
    return MakeErrorResult(error);
  }

  return loadSvgSource(buffer.str(), options, svgPath);
}

ToolCallResult EditorControlSession::loadSvg(const json& arguments) {
  std::string error;
  std::string source;
  std::string sourcePath;
  LoadOptions options;
  if (!ReadRequiredString(arguments, "svg_source", &source, &error) ||
      !ReadOptionalString(arguments, "source_path", "<memory>", &sourcePath, &error) ||
      !ReadLoadOptions(arguments, &options, &error)) {
    return MakeErrorResult(error);
  }

  return loadSvgSource(source, options, sourcePath);
}

json EditorControlSession::sourceStateJson() const {
  return json{
      {"source_path", currentSourcePath_},
      {"source_revision", sourceRevision_},
      {"loaded_source_revision", loadedSourceRevision_},
      {"preview_stale", sourceRevision_ != loadedSourceRevision_},
      {"source_length", currentSourceText_.size()},
      {"source_hash", ReproContentHash(currentSourceText_)},
      {"last_parse_error", lastParseError_.has_value() ? json(*lastParseError_) : json(nullptr)},
  };
}

bool EditorControlSession::loadCurrentSourceText(const LoadOptions& options,
                                                 std::string_view sourcePath,
                                                 bool resetRenderVersion, json* loadInfo,
                                                 std::string* error) {
  if (!waitUntilIdle(error)) {
    return false;
  }

  if (!app_.loadFromString(currentSourceText_)) {
    *error = "failed to parse SVG source";
    return false;
  }

  currentSourcePath_ = std::string(sourcePath);
  app_.setCurrentFilePath(std::string(sourcePath));
  app_.setCleanSourceText(currentSourceText_);
  app_.flushFrame();

  selectTool_ = std::make_unique<SelectTool>();
  penTool_.cancel();
  activeReplayTool_ = ActiveReplayTool::Select;
  displayPresentation_ = CompositedPresentation{};
  displayTextures_.reset();
  rnrRecording_ = RnrRecordingState{};

  const Box2d viewBox = DocumentViewBoxOr(
      app_.document().document(), Box2d::FromXYWH(0, 0, kDefaultCanvasWidth, kDefaultCanvasHeight));
  canvasWidth_ = options.canvasWidth > 0
                     ? options.canvasWidth
                     : std::max(1, static_cast<int>(std::ceil(viewBox.width())));
  canvasHeight_ = options.canvasHeight > 0
                      ? options.canvasHeight
                      : std::max(1, static_cast<int>(std::ceil(viewBox.height())));
  devicePixelRatio_ = options.devicePixelRatio > 0.0 ? options.devicePixelRatio : 1.0;
  app_.document().document().setCanvasSize(canvasWidth_, canvasHeight_);

  if (resetRenderVersion) {
    nextRenderVersion_ = 1;
  }
  loadedSourceRevision_ = sourceRevision_;
  lastParseError_.reset();

  *loadInfo = json{
      {"ok", true},
      {"source_path", std::string(sourcePath)},
      {"canvas", {{"width", canvasWidth_}, {"height", canvasHeight_}}},
      {"device_pixel_ratio", devicePixelRatio_},
      {"document_view_box", BoxToJson(viewBox)},
      {"selection", selectedElementJson()},
      {"source", sourceStateJson()},
  };
  return true;
}

ToolCallResult EditorControlSession::loadSvgSource(std::string_view source,
                                                   const LoadOptions& options,
                                                   std::string_view sourcePath) {
  const std::string previousSourcePath = currentSourcePath_;
  const std::string previousSourceText = currentSourceText_;
  const std::uint64_t previousSourceRevision = sourceRevision_;
  const std::uint64_t previousLoadedSourceRevision = loadedSourceRevision_;
  const std::optional<std::string> previousParseError = lastParseError_;

  currentSourcePath_ = std::string(sourcePath);
  currentSourceText_ = std::string(source);
  ++sourceRevision_;

  ToolCallResult out;
  std::string error;
  if (!loadCurrentSourceText(options, sourcePath, /*resetRenderVersion=*/true, &out.body, &error)) {
    currentSourcePath_ = previousSourcePath;
    currentSourceText_ = previousSourceText;
    sourceRevision_ = previousSourceRevision;
    loadedSourceRevision_ = previousLoadedSourceRevision;
    lastParseError_ = previousParseError;
    return MakeErrorResult(error);
  }

  if (options.renderAfterLoad) {
    std::vector<CapturedRenderResult> renderResults;
    if (!renderCurrentFrame(&renderResults, &error)) {
      return MakeErrorResult(error);
    }
    out.body["render_stages"] =
        RenderResultsJson(renderResults, &out, options.captureOptions, "load");
  }
  return out;
}

ToolCallResult EditorControlSession::getSvgSource(const json& arguments) const {
  std::string error;
  int requestedOffset = 0;
  if (!ReadNonNegativeIntMember(arguments, "offset", &requestedOffset, &error)) {
    return MakeErrorResult(error);
  }

  const std::size_t offset = static_cast<std::size_t>(requestedOffset);
  if (offset > currentSourceText_.size()) {
    return MakeErrorResult("offset is past the end of the SVG source");
  }

  std::size_t length = currentSourceText_.size() - offset;
  if (arguments.contains("length") && !arguments["length"].is_null()) {
    int requestedLength = 0;
    if (!ReadNonNegativeIntMember(arguments, "length", &requestedLength, &error)) {
      return MakeErrorResult(error);
    }
    length =
        std::min(static_cast<std::size_t>(requestedLength), currentSourceText_.size() - offset);
  }

  ToolCallResult out;
  out.body = json{
      {"ok", true},
      {"source", sourceStateJson()},
      {"offset", offset},
      {"length", length},
      {"text", currentSourceText_.substr(offset, length)},
  };
  return out;
}

ToolCallResult EditorControlSession::editSvgSource(const json& arguments) {
  if (rnrRecording_.active) {
    return MakeErrorResult("editing SVG source while an .rnr recording is active is not supported");
  }

  std::string error;
  (void)drainPendingWritebacks();

  bool allowParseFailure = true;
  bool renderAfterEdit = true;
  std::uint64_t expectedSourceRevision = sourceRevision_;
  std::string sourcePath;
  if (!ReadOptionalBool(arguments, "allow_parse_failure", true, &allowParseFailure, &error) ||
      !ReadOptionalBool(arguments, "render_after_edit", true, &renderAfterEdit, &error) ||
      !ReadOptionalUint64(arguments, "expected_source_revision", sourceRevision_,
                          &expectedSourceRevision, &error) ||
      !ReadOptionalString(arguments, "source_path", currentSourcePath_, &sourcePath, &error)) {
    return MakeErrorResult(error);
  }
  if (expectedSourceRevision != sourceRevision_) {
    return MakeErrorResult("expected_source_revision does not match the current SVG source");
  }

  LoadOptions options;
  options.canvasWidth = canvasWidth_;
  options.canvasHeight = canvasHeight_;
  options.devicePixelRatio = devicePixelRatio_ > 0.0 ? devicePixelRatio_ : 1.0;
  options.renderAfterLoad = renderAfterEdit;
  if (!ReadOptionalInt(arguments, "canvas_width", options.canvasWidth, &options.canvasWidth,
                       &error) ||
      !ReadOptionalInt(arguments, "canvas_height", options.canvasHeight, &options.canvasHeight,
                       &error) ||
      !ReadOptionalDouble(arguments, "device_pixel_ratio", options.devicePixelRatio,
                          &options.devicePixelRatio, &error) ||
      !ReadCaptureOptions(arguments, true, &options.captureOptions, &error)) {
    return MakeErrorResult(error);
  }

  const auto replaceIt = arguments.find("replace_source");
  const auto editsIt = arguments.find("edits");
  if ((replaceIt == arguments.end() || replaceIt->is_null()) &&
      (editsIt == arguments.end() || editsIt->is_null()) && currentSourceText_.empty()) {
    return MakeErrorResult("edit_svg_source requires replace_source or edits for an empty session");
  }

  std::string nextSource = currentSourceText_;
  if (replaceIt != arguments.end() && !replaceIt->is_null()) {
    if (!replaceIt->is_string()) {
      return MakeErrorResult("argument must be a string: replace_source");
    }
    nextSource = replaceIt->get<std::string>();
  }

  if (editsIt != arguments.end() && !editsIt->is_null()) {
    if (!editsIt->is_array()) {
      return MakeErrorResult("argument must be an array: edits");
    }
    int editIndex = 0;
    for (const json& edit : *editsIt) {
      if (!edit.is_object()) {
        return MakeErrorResult("each edit must be an object");
      }
      if (!edit.contains("offset")) {
        return MakeErrorResult("each edit requires offset");
      }

      int requestedOffset = 0;
      int requestedDeleteCount = 0;
      std::string insert;
      if (!ReadNonNegativeIntMember(edit, "offset", &requestedOffset, &error) ||
          !ReadNonNegativeIntMember(edit, "delete_count", &requestedDeleteCount, &error) ||
          !ReadOptionalString(edit, "insert", "", &insert, &error)) {
        return MakeErrorResult("edit " + std::to_string(editIndex) + ": " + error);
      }

      const std::size_t offset = static_cast<std::size_t>(requestedOffset);
      const std::size_t deleteCount = static_cast<std::size_t>(requestedDeleteCount);
      if (offset > nextSource.size()) {
        return MakeErrorResult("edit " + std::to_string(editIndex) +
                               ": offset is past the end of the SVG source");
      }
      if (deleteCount > nextSource.size() - offset) {
        return MakeErrorResult("edit " + std::to_string(editIndex) +
                               ": delete_count extends past the end of the SVG source");
      }
      nextSource.replace(offset, deleteCount, insert);
      ++editIndex;
    }
  }

  const std::string previousSourcePath = currentSourcePath_;
  const std::string previousSourceText = currentSourceText_;
  const std::uint64_t previousSourceRevision = sourceRevision_;
  const std::uint64_t previousLoadedSourceRevision = loadedSourceRevision_;
  const std::optional<std::string> previousParseError = lastParseError_;

  currentSourcePath_ = sourcePath;
  currentSourceText_ = std::move(nextSource);
  ++sourceRevision_;

  ToolCallResult out;
  json loadInfo;
  if (!loadCurrentSourceText(options, sourcePath, /*resetRenderVersion=*/true, &loadInfo, &error)) {
    lastParseError_ = error;
    if (!allowParseFailure) {
      currentSourcePath_ = previousSourcePath;
      currentSourceText_ = previousSourceText;
      sourceRevision_ = previousSourceRevision;
      loadedSourceRevision_ = previousLoadedSourceRevision;
      lastParseError_ = previousParseError;
      return MakeErrorResult(error);
    }

    out.body = json{
        {"ok", true},
        {"parsed", false},
        {"preview_stale", true},
        {"parse_error", error},
        {"source", sourceStateJson()},
        {"attached_image_count", 0},
    };
    return out;
  }

  out.body = std::move(loadInfo);
  out.body["parsed"] = true;
  out.body["preview_stale"] = false;
  if (options.renderAfterLoad) {
    std::vector<CapturedRenderResult> renderResults;
    if (!renderCurrentFrame(&renderResults, &error)) {
      return MakeErrorResult(error);
    }
    out.body["render_stages"] =
        RenderResultsJson(renderResults, &out, options.captureOptions, "edit_svg_source");
  }
  out.body["attached_image_count"] = out.images.size();
  return out;
}

}  // namespace donner::editor::mcp
