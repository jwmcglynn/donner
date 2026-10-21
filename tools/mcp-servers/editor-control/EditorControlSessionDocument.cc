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
#include "donner/editor/SourceEditIntent.h"
#include "donner/editor/SourceSync.h"
#include "donner/editor/TextPatch.h"
#include "donner/editor/ViewportState.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/resources/FontManager.h"
#include "nlohmann/json.hpp"
#include "tools/mcp-servers/editor-control/EditorControlSessionInternal.h"

namespace donner::editor::mcp {

namespace {

using nlohmann::json;

/// Whether \p arguments has \p key with a non-null value.
bool HasNonNull(const json& arguments, std::string_view key) {
  const auto it = arguments.find(key);
  return it != arguments.end() && !it->is_null();
}

/// Parse `edit_svg_source`'s `dispatch` argument.
bool ParseSourceEditDispatch(std::string_view dispatch, bool* sourcePane, std::string* error) {
  *sourcePane = dispatch == "source_pane";
  if (!*sourcePane && dispatch != "reload") {
    *error = "dispatch must be 'reload' or 'source_pane'";
    return false;
  }
  return true;
}

/// The reason of \p diagnostic as JSON, or null.
json DiagnosticReasonJson(const std::optional<ParseDiagnostic>& diagnostic) {
  return diagnostic.has_value() ? json(std::string(diagnostic->reason.str())) : json();
}

/// Source-pane intent kind for an edit that removes \p deleteCount bytes and inserts \p insert.
SourceEditIntentKind IntentKindFor(std::size_t deleteCount, std::string_view insert) {
  if (deleteCount == 0) {
    return SourceEditIntentKind::Insert;
  }
  return insert.empty() ? SourceEditIntentKind::Delete : SourceEditIntentKind::Replace;
}

/// Read one `edit_svg_source` edit and check it against \p source.
bool ReadDraftEdit(const json& edit, std::string_view source, SourceEditIntent* intent,
                   std::string* error) {
  int requestedOffset = 0;
  int requestedDeleteCount = 0;
  std::string insert;
  if (!edit.is_object() || !edit.contains("offset")) {
    *error = "each edit must be an object with an offset";
    return false;
  }
  if (!ReadNonNegativeIntMember(edit, "offset", &requestedOffset, error) ||
      !ReadNonNegativeIntMember(edit, "delete_count", &requestedDeleteCount, error) ||
      !ReadOptionalString(edit, "insert", "", &insert, error)) {
    return false;
  }

  const std::size_t offset = static_cast<std::size_t>(requestedOffset);
  const std::size_t deleteCount = static_cast<std::size_t>(requestedDeleteCount);
  if (offset > source.size()) {
    *error = "offset is past the end of the SVG source";
    return false;
  }
  if (deleteCount > source.size() - offset) {
    *error = "delete_count extends past the end of the SVG source";
    return false;
  }
  *intent = SourceEditIntent{.offset = offset,
                             .removedLength = deleteCount,
                             .replacement = insert,
                             .kind = IntentKindFor(deleteCount, insert)};
  return true;
}

/// Apply `edit_svg_source` edits in order to \p source, recording each as a source edit intent.
bool ApplyDraftEdits(const json& edits, std::string* source, std::vector<SourceEditIntent>* intents,
                     std::string* error) {
  if (!edits.is_array()) {
    *error = "argument must be an array: edits";
    return false;
  }
  std::size_t editIndex = 0;
  for (const json& edit : edits) {
    SourceEditIntent intent;
    if (!ReadDraftEdit(edit, *source, &intent, error)) {
      *error = "edit " + std::to_string(editIndex) + ": " + *error;
      return false;
    }
    source->replace(intent.offset, intent.removedLength, intent.replacement);
    intents->push_back(std::move(intent));
    ++editIndex;
  }
  return true;
}

}  // namespace

void EditorControlSession::installFontCatalogOnDocument() {
  svg::SVGDocument& document = app_.document().document();
  document.withWriteAccess([this](svg::DocumentWriteAccess& access) {
    Registry& registry = access.registry();
    svg::FontManager& fontManager = registry.ctx().contains<svg::FontManager>()
                                        ? registry.ctx().get<svg::FontManager>()
                                        : registry.ctx().emplace<svg::FontManager>(registry);
    fontManager.setFontProvider(&fontCatalog_);
  });
}

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
  installFontCatalogOnDocument();

  currentSourcePath_ = std::string(sourcePath);
  app_.setCurrentFilePath(std::string(sourcePath));
  app_.setCleanSourceText(currentSourceText_);
  app_.flushFrame();

  selectTool_ = std::make_unique<SelectTool>();
  penTool_.cancel();
  textTool_.cancel();
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

ToolCallResult EditorControlSession::getSvgSource(const json& arguments) {
  (void)drainPendingWritebacks();

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

std::optional<std::string> EditorControlSession::sourcePaneDispatchError(
    bool replaceSource, bool edits, bool allowParseFailure) const {
  if (!allowParseFailure) {
    // Like the source pane, each keystroke commits its bytes even when it leaves them malformed.
    return "dispatch 'source_pane' always keeps malformed edits; allow_parse_failure must be true";
  }
  if (replaceSource) {
    return "dispatch 'source_pane' applies edits; replace_source is not supported";
  }
  if (!edits) {
    return "dispatch 'source_pane' requires edits";
  }
  // Keystrokes continue from the document's source bytes, which a failed keystroke still updates.
  if (!app_.hasDocument() || !app_.document().document().hasSourceStore() ||
      app_.document().document().source() != currentSourceText_) {
    return "dispatch 'source_pane' requires a document whose source matches the draft";
  }
  return std::nullopt;
}

void EditorControlSession::applyCanvasOptions(const LoadOptions& options) {
  canvasWidth_ = options.canvasWidth > 0 ? options.canvasWidth : canvasWidth_;
  canvasHeight_ = options.canvasHeight > 0 ? options.canvasHeight : canvasHeight_;
  devicePixelRatio_ = options.devicePixelRatio > 0.0 ? options.devicePixelRatio : 1.0;
  app_.document().document().setCanvasSize(canvasWidth_, canvasHeight_);
}

void EditorControlSession::recordSourcePanePreviewState(
    bool documentMatchesDraft, const std::optional<ParseDiagnostic>& sourceDiagnostic,
    const std::optional<ParseDiagnostic>& parseDiagnostic) {
  // A failed incremental edit still commits its bytes but keeps the last valid DOM, so the preview
  // is current only when the bytes match and no source diagnostic is pending.
  if (documentMatchesDraft && !sourceDiagnostic.has_value()) {
    loadedSourceRevision_ = sourceRevision_;
    lastParseError_.reset();
    return;
  }
  const std::optional<ParseDiagnostic>& pending =
      sourceDiagnostic.has_value() ? sourceDiagnostic : parseDiagnostic;
  if (pending.has_value()) {
    lastParseError_ = std::string(pending->reason.str());
  }
}

ToolCallResult EditorControlSession::dispatchSourcePaneEdits(
    const std::vector<SourceEditIntent>& intents, const LoadOptions& options) {
  std::string error;
  if (intents.empty()) {
    return MakeErrorResult("dispatch 'source_pane' requires at least one edit");
  }
  if (!waitUntilIdle(&error)) {
    return MakeErrorResult(error);
  }

  // Each edit is one source-pane keystroke: dispatch it through the same incremental path the
  // text editor uses, then flush it the way the next editor frame would.
  std::string previousSourceText = currentSourceText_;
  std::optional<std::string> lastWritebackSourceText;
  json dispatched = json::array();
  for (const SourceEditIntent& intent : intents) {
    std::string editedSource = previousSourceText;
    editedSource.replace(intent.offset, intent.removedLength, intent.replacement);
    const DispatchSourceTextChangeResult result = DispatchSourceEditIntents(
        app_, {intent}, editedSource, &previousSourceText, &lastWritebackSourceText);
    // Incremental edits apply immediately; a queued command here is a whole-document reparse.
    const bool documentReplaceQueued = !app_.document().queue().empty();
    (void)app_.flushFrame();
    dispatched.push_back(json{{"offset", intent.offset},
                              {"delete_count", intent.removedLength},
                              {"insert", intent.replacement},
                              {"dispatched_mutation", result.dispatchedMutation},
                              {"document_replace_queued", documentReplaceQueued}});
  }

  currentSourceText_ = std::move(previousSourceText);
  ++sourceRevision_;
  applyCanvasOptions(options);

  svg::SVGDocument& document = app_.document().document();
  const bool documentMatchesDraft =
      document.hasSourceStore() && document.source() == currentSourceText_;
  const std::optional<ParseDiagnostic> sourceDiagnostic = document.xmlDocument().sourceDiagnostic();
  const std::optional<ParseDiagnostic> parseDiagnostic = app_.document().lastParseError();
  recordSourcePanePreviewState(documentMatchesDraft, sourceDiagnostic, parseDiagnostic);
  ToolCallResult out;
  out.body = json{
      {"ok", true},
      {"dispatch", "source_pane"},
      {"edits", std::move(dispatched)},
      {"document_matches_draft", documentMatchesDraft},
      {"source_diagnostic", DiagnosticReasonJson(sourceDiagnostic)},
      {"parse_diagnostic", DiagnosticReasonJson(parseDiagnostic)},
      {"source", sourceStateJson()},
  };

  if (options.renderAfterLoad) {
    std::vector<CapturedRenderResult> renderResults;
    if (!renderCurrentFrame(&renderResults, &error)) {
      return MakeErrorResult(error);
    }
    out.body["render_stages"] = RenderResultsJson(renderResults, &out, options.captureOptions,
                                                  "edit_svg_source/source_pane");
  }
  out.body["attached_image_count"] = out.images.size();
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
  std::string dispatch;
  if (!ReadOptionalString(arguments, "dispatch", "reload", &dispatch, &error) ||
      !ReadOptionalBool(arguments, "allow_parse_failure", true, &allowParseFailure, &error) ||
      !ReadOptionalBool(arguments, "render_after_edit", true, &renderAfterEdit, &error) ||
      !ReadOptionalUint64(arguments, "expected_source_revision", sourceRevision_,
                          &expectedSourceRevision, &error) ||
      !ReadOptionalString(arguments, "source_path", currentSourcePath_, &sourcePath, &error)) {
    return MakeErrorResult(error);
  }
  if (expectedSourceRevision != sourceRevision_) {
    return MakeErrorResult("expected_source_revision does not match the current SVG source");
  }
  bool sourcePaneDispatch = false;
  if (!ParseSourceEditDispatch(dispatch, &sourcePaneDispatch, &error)) {
    return MakeErrorResult(error);
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
  const bool hasReplaceSource = HasNonNull(arguments, "replace_source");
  const bool hasEdits = HasNonNull(arguments, "edits");
  if (!hasReplaceSource && !hasEdits && currentSourceText_.empty()) {
    return MakeErrorResult("edit_svg_source requires replace_source or edits for an empty session");
  }

  if (sourcePaneDispatch) {
    if (std::optional<std::string> sourcePaneError =
            sourcePaneDispatchError(hasReplaceSource, hasEdits, allowParseFailure)) {
      return MakeErrorResult(*sourcePaneError);
    }
  }

  std::string nextSource = currentSourceText_;
  std::vector<SourceEditIntent> intents;
  if (hasReplaceSource) {
    if (!replaceIt->is_string()) {
      return MakeErrorResult("argument must be a string: replace_source");
    }
    nextSource = replaceIt->get<std::string>();
  }

  if (hasEdits && !ApplyDraftEdits(*editsIt, &nextSource, &intents, &error)) {
    return MakeErrorResult(error);
  }

  if (sourcePaneDispatch) {
    return dispatchSourcePaneEdits(intents, options);
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
