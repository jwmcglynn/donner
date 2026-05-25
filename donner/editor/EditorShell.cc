#define IMGUI_DEFINE_MATH_OPERATORS
#include "donner/editor/EditorShell.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "GLFW/glfw3.h"
#include "donner/editor/DocumentSave.h"
#include "donner/editor/DragCoalesce.h"
#include "donner/editor/FocusView.h"
#include "donner/editor/KeyboardShortcutPolicy.h"
#include "donner/editor/SelectionTransformHandles.h"
#include "donner/editor/SourceSelection.h"
#include "donner/editor/SourceSync.h"
#include "donner/editor/TracyWrapper.h"
#include "donner/editor/XmlAutocomplete.h"
#include "donner/editor/gui/EditorWindow.h"
#include "donner/editor/repro/ReproRecorder.h"
#include "embed_resources/FiraCodeFont.h"
#include "embed_resources/RobotoFont.h"

namespace donner::editor {

namespace {

constexpr float kMinSourcePaneWidth = 240.0f;
constexpr float kMaxSourcePaneWidth = 900.0f;
constexpr float kSourcePaneSplitterThickness = 6.0f;
constexpr float kSourcePaneRevealHandleWidth = 10.0f;
constexpr float kSourcePaneCollapseThreshold = kMinSourcePaneWidth;
constexpr float kTreeViewHeightFraction = 0.33f;
constexpr float kKeyboardZoomStep = 1.5f;
constexpr float kRightPaneSplitterThickness = 6.0f;
constexpr float kLayerPanelSplitterThickness = 6.0f;
constexpr float kLayerPanelDragHandleHeight = 26.0f;
constexpr float kMinRightPaneWidth = 220.0f;
constexpr float kMaxRightPaneWidth = 900.0f;
constexpr float kMinInspectorPaneHeight = 96.0f;
constexpr float kMinLayerPanelHeight = 140.0f;
constexpr double kTrackpadPanPixelsPerScrollUnit = 10.0;
constexpr double kWheelZoomStep = 1.1;
constexpr int kMaxSaveSyncFlushPasses = 4;
constexpr float kReferenceChipPaddingX = 8.0f;
constexpr float kReferenceChipPaddingY = 5.0f;
constexpr float kReferenceChipRadius = 6.0f;
constexpr float kReferenceChipGapFromAabb = 30.0f;
constexpr float kReferenceChipMinFontSize = 14.0f;
constexpr float kReferenceChipFontStepDown = 1.0f;
constexpr std::string_view kRenderPaneContextMenuName = "Render Context Menu";

ImGuiMouseCursor CursorForTransformHandleIntent(const SelectionTransformHandleIntent& intent) {
  if (intent.kind == SelectionTransformHandleKind::Rotate) {
    return ImGuiMouseCursor_ResizeAll;
  }

  if (intent.kind != SelectionTransformHandleKind::Resize) {
    return ImGuiMouseCursor_Arrow;
  }

  switch (intent.corner) {
    case SelectionTransformCorner::TopLeft:
    case SelectionTransformCorner::BottomRight: return ImGuiMouseCursor_ResizeNWSE;
    case SelectionTransformCorner::TopRight:
    case SelectionTransformCorner::BottomLeft: return ImGuiMouseCursor_ResizeNESW;
  }
  return ImGuiMouseCursor_ResizeAll;
}

bool ContainsScreenPoint(const Box2d& rect, const ImVec2& point) {
  return point.x >= rect.topLeft.x && point.x <= rect.bottomRight.x && point.y >= rect.topLeft.y &&
         point.y <= rect.bottomRight.y;
}

float ClampSourcePaneWidthForWindow(float requestedWidth, float windowWidth) {
  const float sourcePaneUpperBound = std::max(
      0.0f, std::min(kMaxSourcePaneWidth, windowWidth - kMinRightPaneWidth - kMinRightPaneWidth));
  const float sourcePaneLowerBound = std::min(kMinSourcePaneWidth, sourcePaneUpperBound);
  return std::clamp(requestedWidth, sourcePaneLowerBound, sourcePaneUpperBound);
}

std::string ReferenceHighlightChipLabel(const ReferenceHighlightSummary& summary) {
  std::vector<std::string> parts;
  if (!summary.referencedElements.empty()) {
    parts.push_back("-> " + std::to_string(summary.referencedElements.size()));
  }
  if (!summary.referencingElements.empty()) {
    parts.push_back("<- " + std::to_string(summary.referencingElements.size()));
  }

  std::string label;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i != 0) {
      label += "  ";
    }
    label += parts[i];
  }
  return label;
}

float ReferenceHighlightChipFontSize() {
  return std::max(kReferenceChipMinFontSize, ImGui::GetFontSize() - kReferenceChipFontStepDown);
}

ImVec2 ReferenceHighlightChipTextSize(std::string_view label) {
  ImFont* font = ImGui::GetFont();
  return font->CalcTextSizeA(ReferenceHighlightChipFontSize(), FLT_MAX, -1.0f, label.data(),
                             label.data() + label.size());
}

void AddUniqueElements(std::vector<svg::SVGElement>* target,
                       std::span<const svg::SVGElement> elements) {
  for (const svg::SVGElement& element : elements) {
    const Entity entity = element.entityHandle().entity();
    const auto it = std::ranges::find_if(*target, [entity](const svg::SVGElement& existing) {
      return existing.entityHandle().entity() == entity;
    });
    if (it == target->end()) {
      target->push_back(element);
    }
  }
}

bool ContainsElement(std::span<const svg::SVGElement> elements, const svg::SVGElement& element) {
  return std::ranges::find(elements, element) != elements.end();
}

std::string ElementContextMenuLabel(const svg::SVGElement& element) {
  const std::string_view tagName = element.tagName().name;
  std::string label = "<";
  label.append(tagName.data(), tagName.size());
  label.push_back('>');

  const RcString id = element.id();
  const std::string_view idSv = id;
  if (!idSv.empty()) {
    label.push_back(' ');
    label.push_back('#');
    label.append(idSv.data(), idSv.size());
  }

  return label;
}

std::optional<std::string> LoadFile(const std::string& filename) {
  std::ifstream file(filename, std::ios::binary);
  if (!file) {
    return std::nullopt;
  }
  std::ostringstream out;
  out << file.rdbuf();
  return std::move(out).str();
}

std::string InitialDocumentSyncSource(const EditorShellOptions& options) {
  if (options.initialSource.has_value()) {
    return *options.initialSource;
  }
  return LoadFile(options.svgPath).value_or("");
}

std::string CanonicalizeForTextEditor(std::string_view source) {
  std::string result(source);
  if (!result.empty() && result.back() == '\n') {
    result.pop_back();
  }
  return result;
}

Box2d ResolveDocumentViewBox(svg::SVGDocument& document) {
  if (auto viewBox = document.svgElement().viewBox(); viewBox.has_value()) {
    return *viewBox;
  }
  const Vector2i intrinsic = document.canvasSize();
  if (intrinsic.x > 0 && intrinsic.y > 0) {
    return Box2d::FromXYWH(0.0, 0.0, static_cast<double>(intrinsic.x),
                           static_cast<double>(intrinsic.y));
  }
  return Box2d::FromXYWH(0.0, 0.0, 1.0, 1.0);
}

}  // namespace

EditorShell::EditorShell(gui::EditorWindow& window, EditorShellOptions options)
    : window_(window),
      options_(std::move(options)),
      app_(),
      selectTool_(),
      textEditor_(),
      textures_(window.geodeDevice()),
      renderCoordinator_(window.geodeDevice()),
      rotateCursorSet_(),
      documentSyncController_(InitialDocumentSyncSource(options_)),
      interactionController_(),
      inputBridge_(window_, kWheelZoomStep),
      layerInspectorPanel_(window.geodeDevice()),
      dialogPresenter_(options_.editorNoticeText) {
  std::optional<std::string> initialSource = options_.initialSource;
  if (!initialSource.has_value() && !options_.svgPath.empty()) {
    initialSource = LoadFile(options_.svgPath);
  }
  if (!initialSource.has_value()) {
    return;
  }

  textEditor_.setLanguageDefinition(TextEditor::LanguageDefinition::SVG());
  textEditor_.setText(*initialSource);
  textEditor_.resetTextChanged();
  textEditor_.setActiveAutocomplete(true);
  textEditor_.setAutocompleteProvider([](const TextEditor::AutocompleteRequest& request)
                                          -> std::optional<TextEditor::AutocompleteResponse> {
    XmlAutocompleteContext context =
        DetectXmlAutocompleteContext(request.source, request.cursorOffset);
    if (context.kind == XmlAutocompleteContextKind::Unknown) {
      return std::nullopt;
    }

    TextEditor::AutocompleteResponse response;
    response.replaceStartOffset = context.replaceStartOffset;
    response.replaceEndOffset = context.replaceEndOffset;
    for (const XmlAutocompleteSuggestion& suggestion : BuildXmlAutocompleteSuggestions(context)) {
      response.suggestions.push_back(TextEditor::AutocompleteSuggestion{
          .displayText = RcString(suggestion.displayText),
          .insertText = RcString(suggestion.insertText),
      });
    }
    return response;
  });

  ImGuiIO& io = ImGui::GetIO();
  ImFontConfig fontCfg;
  fontCfg.FontDataOwnedByAtlas = false;
  const double displayScale = window_.displayScale();
  std::ignore =
      io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(embedded::kRobotoRegularTtf.data()),
                                     static_cast<int>(embedded::kRobotoRegularTtf.size()),
                                     static_cast<float>(15.0 * displayScale), &fontCfg);
  uiFontBold_ =
      io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(embedded::kRobotoBoldTtf.data()),
                                     static_cast<int>(embedded::kRobotoBoldTtf.size()),
                                     static_cast<float>(15.0 * displayScale), &fontCfg);
  codeFont_ = io.Fonts->AddFontFromMemoryTTF(
      const_cast<unsigned char*>(embedded::kFiraCodeRegularTtf.data()),
      static_cast<int>(embedded::kFiraCodeRegularTtf.size()),
      static_cast<float>(14.0 * displayScale), &fontCfg);

  if (!app_.loadFromString(*initialSource)) {
    // Keep the shell alive so the user can still edit/fix the file from the source pane.
  }
  if (options_.initialPath.has_value()) {
    app_.setCurrentFilePath(*options_.initialPath);
  } else if (!options_.svgPath.empty()) {
    app_.setCurrentFilePath(options_.svgPath);
  }
  // Route the clean baseline through `textEditor_.getText()` so it matches
  // what `syncDirtyFromSource` will later compare against. `TextBuffer`
  // canonicalizes line endings (e.g. drops a trailing `\n`), so comparing
  // against the raw file bytes would leave the dirty flag latched on.
  app_.setCleanSourceText(textEditor_.getText());
  renderCoordinator_.refreshSelectionBoundsCache(app_);
  textures_.initialize();
  if (!rotateCursorSet_.initialize(window_.rawHandle(), window_.geodeDevice())) {
    std::fprintf(stderr, "[editor] custom rotate cursor unavailable; using fallback cursor\n");
  }

  // On-demand render loop: the main thread sleeps in `window.waitEvents()`
  // between user inputs, so the worker thread has to nudge it when a
  // render finishes — otherwise the fresh bitmap sits in `result_`
  // forever. Safe to capture `this` because `AsyncRenderer`'s lifetime
  // is strictly nested inside `RenderCoordinator`'s, which is a member
  // of `*this`.
  renderCoordinator_.asyncRenderer().setWakeCallback([this]() { window_.wakeEventLoop(); });

  if (options_.reproOutputPath.has_value()) {
    repro::ReproRecorderOptions recorderOptions;
    recorderOptions.outputPath = *options_.reproOutputPath;
    recorderOptions.svgPath = options_.svgPath;
    recorderOptions.svgSource = *initialSource;
    const auto winSize = window_.windowSize();
    recorderOptions.windowWidth = winSize.x;
    recorderOptions.windowHeight = winSize.y;
    recorderOptions.displayScale = window_.displayScale();
    reproRecorder_ = std::make_unique<repro::ReproRecorder>(std::move(recorderOptions));
    std::fprintf(stderr, "[repro] recording UI inputs to %s\n", options_.reproOutputPath->c_str());
  }

  valid_ = true;
}

EditorShell::~EditorShell() {
  if (reproRecorder_) {
    if (!reproRecorder_->flush()) {
      std::fprintf(stderr, "[repro] flush failed — recording lost\n");
    }
  }
}

void EditorShell::overrideViewportForReplay(const ViewportState& viewport) {
  pendingViewportReplayOverride_ = viewport;
}

std::optional<std::string> EditorShell::selectedElementLabelForReadback() const {
  const std::optional<svg::SVGElement>& selected = app_.selectedElement();
  if (!selected.has_value()) {
    return std::nullopt;
  }

  const std::string_view tagName = selected->tagName().name;
  std::string label = "<";
  label.append(tagName.data(), tagName.size());
  label.push_back('>');

  const RcString id = selected->id();
  const std::string_view idSv = id;
  if (!idSv.empty()) {
    label.push_back(' ');
    label.push_back('#');
    label.append(idSv.data(), idSv.size());
  }
  return label;
}

LayerInspectorStatusReadback EditorShell::layerInspectorStatusForReadback() const {
  const auto& viewport = interactionController_.viewport();
  const Vector2i viewportDesiredCanvas = viewport.desiredCanvasSize();
  const bool workerBusy = renderCoordinator_.asyncRenderer().isBusy();
  const Vector2i documentCanvas = (!workerBusy && app_.hasDocument())
                                      ? app_.document().document().canvasSize()
                                      : renderCoordinator_.asyncRenderer().lastDocumentCanvasSize();
  const Vector2i compositorCanvas = renderCoordinator_.asyncRenderer().compositorState().canvasSize;
  const CanvasFreshness freshness =
      ClassifyCanvasFreshness(viewportDesiredCanvas, documentCanvas, compositorCanvas);
  LayerInspectorStatusReadback readback{
      .canvasFreshness = freshness,
      .statusSuffix = std::string(CanvasFreshnessStatusSuffix(freshness)),
      .viewportDesiredCanvas = viewportDesiredCanvas,
      .documentCanvas = documentCanvas,
      .compositorCanvas = compositorCanvas,
      .metadataOnlyMissCount = textures_.metadataOnlyMissCount(),
      .duplicateLiveTextureCount = textures_.duplicateLiveTextureCount(),
      .overlayDimsPx = Vector2i(textures_.overlayWidth(), textures_.overlayHeight()),
      .overlayTextureHandle = static_cast<std::uint64_t>(textures_.overlayTexture()),
  };
  const std::optional<SelectTool::ActiveDragPreview> liveActiveDragPreview =
      selectTool_.activeDragPreview();
  const std::optional<SelectTool::ActiveDragPreview> activeDragPreview =
      renderCoordinator_.compositedPresentation().activePreviewForPresentation(
          liveActiveDragPreview);
  const std::optional<SelectTool::ActiveDragPreview> displayedDragPreview =
      renderCoordinator_.compositedPresentation().presentationPreview(activeDragPreview);
  readback.tiles.reserve(textures_.tiles().size());
  for (const GlTextureCache::TileView& tile : textures_.tiles()) {
    Vector2d presentedDragTranslationDoc = tile.dragTranslationDoc;
    if (tile.isDragTarget && activeDragPreview.has_value() && displayedDragPreview.has_value() &&
        activeDragPreview->entity == displayedDragPreview->entity) {
      presentedDragTranslationDoc +=
          activeDragPreview->translation - displayedDragPreview->translation;
    }
    readback.tiles.push_back(LayerInspectorStatusReadback::Tile{
        .id = tile.id,
        .kind = tile.kind,
        .generation = tile.generation,
        .bitmapDimsPx = tile.bitmapDimsPx,
        .rasterCanvasSize = tile.rasterCanvasSize,
        .canvasOffsetDoc = tile.canvasOffsetDoc,
        .bitmapDimsDoc = tile.bitmapDimsDoc,
        .dragTranslationDoc = tile.dragTranslationDoc,
        .presentedDragTranslationDoc = presentedDragTranslationDoc,
        .textureHandle = static_cast<std::uint64_t>(tile.texture),
        .metadataOnly = tile.metadataOnly,
        .isDragTarget = tile.isDragTarget,
    });
  }
  return readback;
}

bool EditorShell::tryOpenPath(std::string_view path, std::string* error) {
  auto contents = LoadFile(std::string(path));
  if (!contents.has_value()) {
    *error = "Could not open file.";
    return false;
  }
  if (!app_.loadFromString(*contents)) {
    *error = "Failed to parse SVG.";
    return false;
  }

  textEditor_.setText(*contents);
  textEditor_.resetTextChanged();
  const std::string canonicalSource = textEditor_.getText();
  app_.setCurrentFilePath(std::string(path));
  resetPresentationForLoadedDocument(canonicalSource);
  return true;
}

void EditorShell::resetPresentationForLoadedDocument(std::string_view canonicalSource) {
  documentSyncController_.resetForLoadedDocument(std::string(canonicalSource));
  app_.setCleanSourceText(canonicalSource);
  lastHighlightedSelection_.clear();
  lastTreeSelection_.reset();
  textEditor_.clearFocusPartition();
  lastPostedScreenPoint_.reset();
  preserveSourceEditFocusCursor_ = false;
  sourceSelectionOriginatedInText_ = false;
  sourceFocusOriginatedInStyle_ = false;
  referenceHighlightActive_ = false;
  referenceHighlightChipHovered_ = false;
  referenceHighlightSummary_ = ReferenceHighlightSummary{};
  lastReferenceHighlightSelection_.clear();
  renderContextMenuDocumentPoint_.reset();
  renderContextMenuHitElement_.reset();
  renderContextMenuOpenRequested_ = false;
  treeSelectionOriginatedInTree_ = false;
  treeviewPendingScroll_ = false;
  renderCoordinator_.resetForLoadedDocument();
  textures_.resetComposited();
  textures_.clearOverlay();
  renderCoordinator_.refreshSelectionBoundsCache(app_);
  dialogPresenter_.clearOpenFileError();
  dialogPresenter_.clearSaveFileError();
}

bool EditorShell::synchronizeSourceBeforeSave(std::string* error) {
  documentSyncController_.handleTextEdits(app_, textEditor_, /*deltaSeconds=*/1.0f);

  if (renderCoordinator_.asyncRenderer().isBusy()) {
    *error = "Cannot save while the renderer is applying pending edits.";
    return false;
  }

  for (int pass = 0; pass < kMaxSaveSyncFlushPasses; ++pass) {
    if (!app_.flushFrame()) {
      break;
    }
    renderCoordinator_.refreshSelectionBoundsCache(app_);
    documentSyncController_.applyPendingWritebacks(app_, selectTool_, textEditor_);
    documentSyncController_.handleTextEdits(app_, textEditor_, /*deltaSeconds=*/1.0f);
  }

  if (!app_.document().queue().empty()) {
    *error = "Cannot save while document edits are still pending.";
    return false;
  }

  return true;
}

bool EditorShell::trySavePath(std::string_view path, std::string* error) {
  if (path.empty()) {
    *error = "Choose a file path.";
    return false;
  }
  if (!app_.hasDocument()) {
    *error = "No SVG document is loaded.";
    return false;
  }
  if (!synchronizeSourceBeforeSave(error)) {
    return false;
  }
  if (!app_.document().document().hasSourceStore()) {
    *error = "The current document has no XML source store.";
    return false;
  }

  const DocumentSaveResult result = SaveSourceToPath(std::filesystem::path(std::string(path)),
                                                     app_.document().document().source());
  if (!result.ok()) {
    *error = result.message;
    return false;
  }

  app_.setCurrentFilePath(std::string(path));
  app_.setCleanSourceText(textEditor_.getText());
  documentSyncController_.resetForLoadedDocument(textEditor_.getText());
  dialogPresenter_.clearSaveFileError();
  updateWindowTitle();
  return true;
}

void EditorShell::requestSaveAs(std::string error) {
  dialogPresenter_.requestSaveFile(app_.currentFilePath(), std::move(error));
}

void EditorShell::requestSave() {
  if (!app_.currentFilePath().has_value()) {
    requestSaveAs();
    return;
  }

  std::string error;
  if (!trySavePath(*app_.currentFilePath(), &error)) {
    requestSaveAs(std::move(error));
  }
}

void EditorShell::requestRevert() {
  if (!app_.hasDocument() || !app_.isDirty() || app_.cleanSourceText().empty()) {
    return;
  }

  const std::string source(app_.cleanSourceText());
  if (!app_.revertToCleanSource()) {
    return;
  }

  textEditor_.setText(source);
  textEditor_.resetTextChanged();
  resetPresentationForLoadedDocument(textEditor_.getText());
  updateWindowTitle();
  window_.wakeEventLoop();
}

void EditorShell::updateWindowTitle() {
  std::string title;
  if (app_.isDirty()) {
    title += "● ";
  }
  if (app_.currentFilePath().has_value()) {
    title += std::filesystem::path(*app_.currentFilePath()).filename().string();
  } else {
    title += "untitled";
  }
  title += " - Donner SVG Editor";
  if (title != lastWindowTitle_) {
    window_.setTitle(title);
    lastWindowTitle_ = std::move(title);
  }
}

void EditorShell::handleGlobalShortcuts() {
  const bool anyPopupOpen = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup);
  const bool cmd = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper;
  const bool shift = ImGui::GetIO().KeyShift;
  const bool pressedZ = ImGui::IsKeyPressed(ImGuiKey_Z, /*repeat=*/false);
  const bool pressedEnter = ImGui::IsKeyPressed(ImGuiKey_Enter, /*repeat=*/false) ||
                            ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, /*repeat=*/false);
  const bool sourcePaneFocused = sourcePaneVisible_ && textEditor_.isFocused();

  if (!anyPopupOpen && cmd && !shift && ImGui::IsKeyPressed(ImGuiKey_O, /*repeat=*/false)) {
    dialogPresenter_.requestOpenFile(app_.currentFilePath());
  }

  if (!anyPopupOpen && cmd && !shift && ImGui::IsKeyPressed(ImGuiKey_Q, /*repeat=*/false)) {
    glfwSetWindowShouldClose(window_.rawHandle(), GLFW_TRUE);
  }

  if (!anyPopupOpen && cmd && ImGui::IsKeyPressed(ImGuiKey_S, /*repeat=*/false)) {
    if (shift) {
      requestSaveAs();
    } else {
      requestSave();
    }
  }

  if (!sourcePaneFocused) {
    if (pressedZ && cmd && !shift) {
      if (app_.canUndo()) {
        app_.undo();
      }
    } else if (pressedZ && cmd && shift && app_.canRedo()) {
      app_.redo();
    }
  }

  if (!anyPopupOpen && cmd &&
      (ImGui::IsKeyPressed(ImGuiKey_Equal, /*repeat=*/false) ||
       ImGui::IsKeyPressed(ImGuiKey_KeypadAdd, /*repeat=*/false))) {
    interactionController_.applyZoom(kKeyboardZoomStep,
                                     interactionController_.viewport().paneCenter());
  }
  if (!anyPopupOpen && cmd &&
      (ImGui::IsKeyPressed(ImGuiKey_Minus, /*repeat=*/false) ||
       ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract, /*repeat=*/false))) {
    interactionController_.applyZoom(1.0 / kKeyboardZoomStep,
                                     interactionController_.viewport().paneCenter());
  }
  if (!anyPopupOpen && cmd && ImGui::IsKeyPressed(ImGuiKey_0, /*repeat=*/false)) {
    interactionController_.resetToActualSize();
  }

  if (CanToggleSourceFocusModeFromShortcut(pressedEnter, cmd, anyPopupOpen)) {
    toggleSourceFocusMode();
  }

  if (!anyPopupOpen && ImGui::IsKeyPressed(ImGuiKey_Escape, /*repeat=*/false) &&
      app_.hasSelection()) {
    app_.setSelection(std::nullopt);
  }

  const bool deleteKey = ImGui::IsKeyPressed(ImGuiKey_Delete, /*repeat=*/false) ||
                         ImGui::IsKeyPressed(ImGuiKey_Backspace, /*repeat=*/false);
  if (CanDeleteSelectedElementsFromShortcut(deleteKey, app_.hasSelection(), anyPopupOpen,
                                            sourcePaneFocused)) {
    std::ignore = app_.deleteSelectionWithUndo(textEditor_.getText());
  }
}

void EditorShell::renderSourcePane(float paneOriginY, float paneHeight, float paneWidth,
                                   ImFont* codeFont) {
  constexpr ImGuiWindowFlags kPaneFlags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                          ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar;
  ImGui::SetNextWindowPos(ImVec2(0.0f, paneOriginY), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(paneWidth, paneHeight), ImGuiCond_Always);
  ImGui::Begin("Source", nullptr, kPaneFlags);
  ImGui::PushFont(codeFont);
  textEditor_.setSourceFocusModeContextMenu(sourceFocusMode_);
  textEditor_.render("##source");
  updateSourceHoverPreview();
  if (textEditor_.takeSourceFocusModeContextMenuToggleRequest()) {
    toggleSourceFocusMode();
  }
  syncSelectionFromSourceCursorIfNeeded();
  ImGui::PopFont();
  const bool sourceEditShouldPreserveCursor =
      textEditor_.isTextChanged() && sourceFocusMode_ && textEditor_.isCursorInsideFocusRange();
  if (textEditor_.isTextChanged()) {
    preserveSourceEditFocusCursor_ = sourceEditShouldPreserveCursor;
  }
  const std::vector<svg::SVGElement> selectionBeforeTextSync = app_.selectedElements();
  documentSyncController_.handleTextEdits(app_, textEditor_, ImGui::GetIO().DeltaTime);
  if (sourceEditShouldPreserveCursor && app_.selectedElements() != selectionBeforeTextSync) {
    sourceSelectionOriginatedInText_ = true;
  }
  updateSourceHoverPreview();
  ImGui::End();
}

void EditorShell::renderRenderPane(const Vector2d& renderPaneOrigin, const Vector2d& renderPaneSize,
                                   ImGuiWindowFlags paneFlags) {
  ImGui::SetNextWindowPos(
      ImVec2(static_cast<float>(renderPaneOrigin.x), static_cast<float>(renderPaneOrigin.y)),
      ImGuiCond_Always);
  ImGui::SetNextWindowSize(
      ImVec2(static_cast<float>(renderPaneSize.x), static_cast<float>(renderPaneSize.y)),
      ImGuiCond_Always);
  ImGui::Begin("Render", nullptr, paneFlags);

  const ImVec2 contentRegion = ImGui::GetContentRegionAvail();
  const ImVec2 paneOriginImGui = ImGui::GetCursorScreenPos();
  interactionController_.updatePaneLayout(
      Vector2d(paneOriginImGui.x, paneOriginImGui.y), Vector2d(contentRegion.x, contentRegion.y),
      app_.hasDocument() ? std::make_optional(ResolveDocumentViewBox(app_.document().document()))
                         : std::nullopt);
  interactionController_.updateDevicePixelRatio(window_.contentScale().x);

  if (pendingViewportReplayOverride_.has_value()) {
    interactionController_.viewport() = *pendingViewportReplayOverride_;
    pendingViewportReplayOverride_.reset();
    viewportInitialized_ = true;
  }

  if (!viewportInitialized_ && interactionController_.viewport().paneSize.x > 0.0 &&
      interactionController_.viewport().paneSize.y > 0.0 && app_.hasDocument()) {
    interactionController_.resetToActualSize();
    viewportInitialized_ = true;
  }

  refreshReferenceHighlightSummaryIfNeeded();
  const std::string referenceChipLabel = ReferenceHighlightChipLabel(referenceHighlightSummary_);
  const std::optional<Box2d> referenceChipRect =
      referenceHighlightChipScreenRect(referenceChipLabel);
  const bool referenceChipHovered = referenceChipRect.has_value() &&
                                    ContainsScreenPoint(*referenceChipRect, ImGui::GetMousePos());

  ImGui::InvisibleButton("##render_canvas", contentRegion,
                         ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
  const bool paneHovered = ImGui::IsItemHovered();
  const bool canvasHovered = paneHovered && !referenceChipHovered;
  const Box2d paneRect = Box2d::FromXYWH(interactionController_.viewport().paneOrigin.x,
                                         interactionController_.viewport().paneOrigin.y,
                                         interactionController_.viewport().paneSize.x,
                                         interactionController_.viewport().paneSize.y);

  const bool spaceHeld = ImGui::IsKeyDown(ImGuiKey_Space);
  const bool middleDown = ImGui::IsMouseDown(ImGuiMouseButton_Middle);
  interactionController_.updatePanState(canvasHovered, spaceHeld, middleDown,
                                        ImGui::IsMouseDown(ImGuiMouseButton_Left),
                                        ImGui::GetMousePos());

  const bool modalCapturingInput =
      referenceChipHovered || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup);
  interactionController_.consumeScrollEvents(inputBridge_.events(), paneRect, modalCapturingInput,
                                             kWheelZoomStep, kTrackpadPanPixelsPerScrollUnit);

  const auto screenToDocument = [&](const ImVec2& screenPoint) -> Vector2d {
    return interactionController_.viewport().screenToDocument(
        Vector2d(screenPoint.x, screenPoint.y));
  };

  if (canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right, /*repeat=*/false)) {
    openRenderPaneContextMenu(screenToDocument(ImGui::GetMousePos()));
  }

  const bool toolEligible = canvasHovered && !interactionController_.panning() && !spaceHeld;
  const auto cachedHandleIntentAt = [&](const Vector2d& documentPoint) {
    const auto& boundsCache = renderCoordinator_.selectionBoundsCache();
    if (boundsCache.lastSelection != app_.selectedElements()) {
      return SelectionTransformHandleIntent{};
    }
    return HitTestSelectionTransformHandles(boundsCache.displayedBoundsDoc, documentPoint,
                                            interactionController_.viewport().pixelsPerDocUnit());
  };
  if (toolEligible && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    const SelectionTransformHandleIntent hoverIntent =
        cachedHandleIntentAt(screenToDocument(ImGui::GetMousePos()));
    if (hoverIntent.kind != SelectionTransformHandleKind::None) {
      if (hoverIntent.kind == SelectionTransformHandleKind::Rotate &&
          rotateCursorSet_.setRotateCursor(hoverIntent.corner)) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
      } else {
        rotateCursorSet_.clearIfActive();
        ImGui::SetMouseCursor(CursorForTransformHandleIntent(hoverIntent));
      }
    } else {
      rotateCursorSet_.clearIfActive();
    }
  } else if (!toolEligible) {
    rotateCursorSet_.clearIfActive();
  }
  if (toolEligible && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    preserveSourceEditFocusCursor_ = false;
    MouseModifiers modifiers;
    modifiers.shift = ImGui::GetIO().KeyShift;
    modifiers.option = ImGui::GetIO().KeyAlt;
    modifiers.pixelsPerDocUnit = interactionController_.viewport().pixelsPerDocUnit();
    interactionController_.bufferPendingClick(screenToDocument(ImGui::GetMousePos()), modifiers);
  }

  // Design doc 0033 §M8 — click→drag handoff doesn't wait for raster.
  //
  // Fast path: if the user clicks inside the bounds of the currently-
  // selected element and outside cached later-painted bounds, we can
  // start the re-drag IMMEDIATELY without gating on `!isBusy()`. The
  // check uses `SelectionBoundsCache` — populated on idle frames — so
  // the call doesn't touch the registry the worker is mid-mutating. The
  // previous M8 attempt failed because it called the live
  // `SnapshotSelectionWorldBounds` during the busy window; the
  // cache-based check fixes the race.
  //
  // Slow path: anything else (selection change, marquee, shift-click,
  // empty cache, multi-select) still waits for `!isBusy()` and goes
  // through the full `onMouseDown` flow. The follow-up registry-
  // reading work (`refreshSelectionBoundsCache`, overlay rasterize,
  // render request) is deferred to the next idle frame for both
  // paths, so the user sees the click acknowledged immediately even
  // when the chrome catches up a frame later.
  if (interactionController_.pendingClick().has_value()) {
    const auto& pendingClick = *interactionController_.pendingClick();
    const auto& boundsCache = renderCoordinator_.selectionBoundsCache();
    const bool cacheMatchesSelection = boundsCache.lastSelection == app_.selectedElements();
    const SelectionTransformHandleIntent pendingHandleIntent =
        cacheMatchesSelection ? cachedHandleIntentAt(pendingClick.documentPoint)
                              : SelectionTransformHandleIntent{};
    const bool tookFastRedrag =
        cacheMatchesSelection && pendingHandleIntent.kind == SelectionTransformHandleKind::None &&
        selectTool_.tryStartRedragOnSelected(app_, pendingClick.documentPoint,
                                             pendingClick.modifiers, boundsCache.displayedBoundsDoc,
                                             boundsCache.displayedOccludingBoundsDoc);
    if (tookFastRedrag) {
      lastPostedScreenPoint_.reset();
      interactionController_.clearPendingClick();
      pendingClickFollowupAfterIdle_ = true;
    } else if (!renderCoordinator_.asyncRenderer().isBusy()) {
      // Slow path: full `onMouseDown` (hitTest + selection change +
      // possible drag start). Race-safe only when the worker is idle.
      lastPostedScreenPoint_.reset();
      selectTool_.onMouseDown(app_, pendingClick.documentPoint, pendingClick.modifiers);
      renderCoordinator_.refreshSelectionBoundsCache(app_);
      renderCoordinator_.maybeRequestRender(app_, selectTool_, interactionController_.viewport(),
                                            textures_);
      renderCoordinator_.rasterizeOverlayForCurrentSelection(
          app_, interactionController_.viewport(), textures_, selectTool_.marqueeRect(),
          RenderCoordinator::OverlayUploadMode::MatchDisplayedVersion,
          selectTool_.activeDragPreview(), selectTool_.activeTransformBoundsPreview());
      interactionController_.clearPendingClick();
    } else {
      // Worker is busy with a (likely-stale) prewarm render at the
      // previous canvas size or zoom. Cancel it so the next idle frame
      // can run the slow-path mouseDown immediately, rather than
      // waiting up to seconds for the in-flight prewarm to finish at
      // high zoom. The render in flight is dispensable — it was a
      // selection prewarm, not a drag, and the click is about to
      // supersede the selection state anyway.
      renderCoordinator_.asyncRenderer().cancelInFlight();
    }
  }

  // After-idle follow-up for the M8 fast-path click. This reads the
  // live registry, so it must wait for the worker to land. Keep the
  // follow-up to cache refresh only: posting a render here would run
  // before this same frame's drag move is applied, leaving the overlay
  // one interaction step behind the composited pixels during re-drag.
  if (pendingClickFollowupAfterIdle_ && !renderCoordinator_.asyncRenderer().isBusy()) {
    renderCoordinator_.refreshSelectionBoundsCache(app_);
    pendingClickFollowupAfterIdle_ = false;
  }

  if (selectTool_.isDragging() || selectTool_.isMarqueeing()) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && !spaceHeld) {
      const ImVec2 currentScreen = ImGui::GetMousePos();
      if (ShouldPostDragMove<ImVec2>(currentScreen, lastPostedScreenPoint_,
                                     renderCoordinator_.asyncRenderer().isBusy())) {
        MouseModifiers modifiers;
        modifiers.shift = ImGui::GetIO().KeyShift;
        modifiers.option = ImGui::GetIO().KeyAlt;
        modifiers.pixelsPerDocUnit = interactionController_.viewport().pixelsPerDocUnit();
        selectTool_.onMouseMove(app_, screenToDocument(currentScreen), /*buttonHeld=*/true,
                                modifiers);
        lastPostedScreenPoint_ = currentScreen;
        if (!renderCoordinator_.asyncRenderer().isBusy() && app_.flushFrame()) {
          renderCoordinator_.refreshSelectionBoundsCache(app_);
          renderCoordinator_.rasterizeOverlayForCurrentSelection(
              app_, interactionController_.viewport(), textures_, selectTool_.marqueeRect(),
              RenderCoordinator::OverlayUploadMode::Immediate, selectTool_.activeDragPreview(),
              selectTool_.activeTransformBoundsPreview());
        }
      }
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
      const auto previewBeforeRelease = selectTool_.activeDragPreview();
      const bool previewHadVisualChange =
          previewBeforeRelease.has_value() &&
          (!previewBeforeRelease->documentFromCachedDocument.isIdentity() ||
           previewBeforeRelease->translation != Vector2d::Zero());
      selectTool_.onMouseUp(app_, screenToDocument(ImGui::GetMousePos()));
      lastPostedScreenPoint_.reset();
      if (previewBeforeRelease.has_value()) {
        if (previewHadVisualChange) {
          renderCoordinator_.compositedPresentation().beginSettling(
              previewBeforeRelease, app_.document().currentFrameVersion());
        }
        if (!renderCoordinator_.asyncRenderer().isBusy() &&
            (app_.flushFrame() || previewHadVisualChange)) {
          renderCoordinator_.compositedPresentation().beginSettling(
              previewBeforeRelease, app_.document().currentFrameVersion());
          renderCoordinator_.refreshSelectionBoundsCache(app_);
          renderCoordinator_.rasterizeOverlayForCurrentSelection(
              app_, interactionController_.viewport(), textures_, selectTool_.marqueeRect(),
              RenderCoordinator::OverlayUploadMode::Immediate);
        }
      } else if (!renderCoordinator_.asyncRenderer().isBusy()) {
        renderCoordinator_.refreshSelectionBoundsCache(app_);
        renderCoordinator_.rasterizeOverlayForCurrentSelection(
            app_, interactionController_.viewport(), textures_, selectTool_.marqueeRect());
      }
    }
  }

  if (!renderCoordinator_.asyncRenderer().isBusy() && app_.hasDocument()) {
    renderCoordinator_.maybeRequestRender(app_, selectTool_, interactionController_.viewport(),
                                          textures_);
  }

  const auto liveActiveDragPreview = selectTool_.activeDragPreview();
  const auto activeDragPreview =
      renderCoordinator_.compositedPresentation().activePreviewForPresentation(
          liveActiveDragPreview);
  const auto displayedDragPreview =
      renderCoordinator_.compositedPresentation().presentationPreview(activeDragPreview);
  RenderPanePresenterState paneState{
      .viewport = interactionController_.viewport(),
      .frameHistory = interactionController_.frameHistory(),
      .textures = textures_,
      .activeDragPreview = activeDragPreview,
      .displayedDragPreview = displayedDragPreview,
      .overlayDragPreview = renderCoordinator_.presentedOverlayDragPreview(),
      .contentRegion = Vector2d(contentRegion.x, contentRegion.y),
      .suppressedLayerEntity = renderCoordinator_.suppressedCompositedLayerEntity(app_),
      .suppressDragTargetTiles = renderCoordinator_.selectedElementIsDisplayNone(app_),
  };
  renderPanePresenter_.render(paneState);
  renderReferenceHighlightChip();
  renderRenderPaneContextMenu();

  ImGui::End();
}

void EditorShell::renderSidebars(float rightPaneX, float rightPaneWidth, float paneOriginY,
                                 const RightSidebarLayout& layout, ImGuiWindowFlags paneFlags) {
  const auto& selectionBeforeTree = app_.selectedElement();
  if (selectionBeforeTree != lastTreeSelection_) {
    treeviewPendingScroll_ = selectionBeforeTree.has_value() && !treeSelectionOriginatedInTree_;
  }
  treeSelectionOriginatedInTree_ = false;

  // Refresh the sidebar snapshot only when the async renderer is idle —
  // during render the worker thread may be mutating registry state the
  // snapshot walk would read. The snapshot persists across the busy window
  // so the panes keep showing their last-known content instead of flashing
  // to "(rendering…)" placeholders.
  const bool rendererBusy = renderCoordinator_.asyncRenderer().isBusy();
  if (!rendererBusy) {
    sidebarPresenter_.refreshSnapshot(app_);
  }
  EditorApp* liveAppForClicks = rendererBusy ? nullptr : &app_;

  ImGui::SetNextWindowPos(ImVec2(rightPaneX, paneOriginY), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(rightPaneWidth, layout.treePaneHeight), ImGuiCond_Always);
  ImGui::Begin("Tree View", nullptr, paneFlags);
  TreeViewState treeState{
      .scrollTarget = selectionBeforeTree,
      .pendingScroll = treeviewPendingScroll_,
  };
  sidebarPresenter_.renderTreeView(liveAppForClicks, treeState);
  treeviewPendingScroll_ = treeState.pendingScroll;
  if (treeState.selectionChangedInTree) {
    preserveSourceEditFocusCursor_ = false;
    treeSelectionOriginatedInTree_ = true;
    treeviewPendingScroll_ = false;
  }
  ImGui::End();
  lastTreeSelection_ = app_.selectedElement();

  ImGui::SetNextWindowPos(ImVec2(rightPaneX, layout.inspectorPaneY), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(rightPaneWidth, layout.inspectorPaneHeight), ImGuiCond_Always);
  ImGui::Begin("Inspector", nullptr, paneFlags);
  sidebarPresenter_.renderInspector(interactionController_.viewport());
  ImGui::End();

  if (layerPanelDetached_) {
    return;
  }

  ImGui::SetNextWindowPos(ImVec2(rightPaneX, layout.layerPanelPaneY), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(rightPaneWidth, layout.layerPanelHeight), ImGuiCond_Always);
  ImGui::Begin("Layers##docked_layers", nullptr, paneFlags);
  renderDockedLayerPanelDragHandle();
  if (!layerPanelDetached_) {
    renderLayerPanelContents();
  }
  ImGui::End();
}

void EditorShell::renderLayerPanelContents() {
  const auto compositeTiles = renderCoordinator_.asyncRenderer().compositorCompositeTiles();
  const auto compositorState = renderCoordinator_.asyncRenderer().compositorState();
  const auto workerCompositorEntity = renderCoordinator_.asyncRenderer().workerCompositorEntity();
  const auto& viewport = interactionController_.viewport();
  const Vector2i viewportDesiredCanvas = viewport.desiredCanvasSize();
  // `SVGDocument::canvasSize()` walks the registry (ComputedAbsoluteTransform /
  // SizedElement / ViewBox) — racy against the worker's
  // `prepareDocumentForRendering` which rebuilds those components in place.
  // When the worker is busy we have to read the cached value the worker
  // stamped at the end of its last completed render; reading live trips a
  // SIGSEGV inside `LayoutSystem::calculateCanvasScaledDocumentSize` when
  // the entt sparse-set page is mid-rebuild.
  const bool workerBusy = renderCoordinator_.asyncRenderer().isBusy();
  const Vector2i documentCanvas = (!workerBusy && app_.hasDocument())
                                      ? app_.document().document().canvasSize()
                                      : renderCoordinator_.asyncRenderer().lastDocumentCanvasSize();
  const auto fastPath = renderCoordinator_.asyncRenderer().compositorFastPathCountersForTesting();
  layerInspectorPanel_.render(compositeTiles, compositorState, workerCompositorEntity,
                              viewport.zoom, viewport.devicePixelRatio, viewportDesiredCanvas,
                              documentCanvas, fastPath);
}

void EditorShell::renderSourcePaneSplitter(float windowWidth, float paneOriginY, float paneHeight,
                                           float sourcePaneWidth) {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  constexpr ImGuiWindowFlags kSplitterFlags =
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground;

  if (sourcePaneVisible_ && sourcePaneWidth > 0.0f) {
    const float splitterLeft = sourcePaneWidth - kSourcePaneSplitterThickness * 0.5f;
    ImGui::SetNextWindowPos(ImVec2(splitterLeft, paneOriginY), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(kSourcePaneSplitterThickness, paneHeight), ImGuiCond_Always);
    ImGui::Begin("##source_pane_splitter", nullptr, kSplitterFlags);
    ImGui::InvisibleButton("##source_pane_splitter_handle",
                           ImVec2(kSourcePaneSplitterThickness, paneHeight));
    if (ImGui::IsItemActive()) {
      const float nextWidth = sourcePaneWidth + ImGui::GetIO().MouseDelta.x;
      if (nextWidth < kSourcePaneCollapseThreshold) {
        setSourcePaneVisible(false);
      } else {
        sourcePaneWidth_ = ClampSourcePaneWidthForWindow(nextWidth, windowWidth);
        window_.wakeEventLoop();
      }
    }
  } else {
    ImGui::SetNextWindowPos(ImVec2(0.0f, paneOriginY), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(kSourcePaneRevealHandleWidth, paneHeight), ImGuiCond_Always);
    ImGui::Begin("##source_pane_reveal_handle", nullptr, kSplitterFlags);
    ImGui::InvisibleButton("##source_pane_reveal_handle",
                           ImVec2(kSourcePaneRevealHandleWidth, paneHeight));
    if (ImGui::IsItemActive()) {
      const float nextWidth = std::max(0.0f, ImGui::GetMousePos().x);
      if (nextWidth >= kSourcePaneCollapseThreshold) {
        sourcePaneWidth_ = ClampSourcePaneWidthForWindow(nextWidth, windowWidth);
        setSourcePaneVisible(true);
      }
    }
  }

  if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
  }

  const bool splitterActive = ImGui::IsItemActive();
  const bool splitterHovered = ImGui::IsItemHovered();
  const ImU32 color = ImGui::GetColorU32(splitterActive    ? ImGuiCol_SeparatorActive
                                         : splitterHovered ? ImGuiCol_SeparatorHovered
                                                           : ImGuiCol_Separator);
  const ImVec2 itemMin = ImGui::GetItemRectMin();
  const ImVec2 itemMax = ImGui::GetItemRectMax();
  if (sourcePaneVisible_) {
    ImGui::GetWindowDrawList()->AddRectFilled(itemMin, itemMax, color);
  } else if (splitterHovered || splitterActive) {
    ImGui::GetWindowDrawList()->AddRectFilled(itemMin, ImVec2(itemMin.x + 2.0f, itemMax.y), color);
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
}

void EditorShell::renderRightPaneSplitter(float windowWidth, float paneOriginY, float paneHeight) {
  const float splitterCenterX = windowWidth - rightPaneWidth_;
  const float splitterLeft = splitterCenterX - kRightPaneSplitterThickness * 0.5f;

  ImGui::SetNextWindowPos(ImVec2(splitterLeft, paneOriginY), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(kRightPaneSplitterThickness, paneHeight), ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  constexpr ImGuiWindowFlags kSplitterFlags =
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground;
  ImGui::Begin("##right_pane_splitter", nullptr, kSplitterFlags);
  ImGui::InvisibleButton("##right_pane_splitter_handle",
                         ImVec2(kRightPaneSplitterThickness, paneHeight));
  if (ImGui::IsItemActive()) {
    const float deltaX = ImGui::GetIO().MouseDelta.x;
    // Dragging the splitter LEFT (negative deltaX) widens the right pane.
    rightPaneWidth_ = std::clamp(rightPaneWidth_ - deltaX, kMinRightPaneWidth, kMaxRightPaneWidth);
  }
  if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
}

void EditorShell::renderLayerPanelSplitter(float rightPaneX, float rightPaneWidth,
                                           const RightSidebarLayout& layout) {
  ImGui::SetNextWindowPos(ImVec2(rightPaneX, layout.layerPanelSplitterY), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(rightPaneWidth, kLayerPanelSplitterThickness), ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  constexpr ImGuiWindowFlags kSplitterFlags =
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground;
  ImGui::Begin("##layer_panel_splitter", nullptr, kSplitterFlags);
  ImGui::InvisibleButton("##layer_panel_splitter_handle",
                         ImVec2(rightPaneWidth, kLayerPanelSplitterThickness));
  if (ImGui::IsItemActive()) {
    layerPanelHeightFraction_ = ResizeLayerPanelHeightFraction(
        layerPanelHeightFraction_, layout.lowerPaneHeight, layout.minLayerPanelHeight,
        layout.maxLayerPanelHeight, ImGui::GetIO().MouseDelta.y);
  }
  if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
  }

  const bool splitterActive = ImGui::IsItemActive();
  const bool splitterHovered = ImGui::IsItemHovered();
  const ImU32 color = ImGui::GetColorU32(splitterActive    ? ImGuiCol_SeparatorActive
                                         : splitterHovered ? ImGuiCol_SeparatorHovered
                                                           : ImGuiCol_Separator);
  ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                            color);
  ImGui::End();
  ImGui::PopStyleVar(2);
}

void EditorShell::renderDockedLayerPanelDragHandle() {
  const ImGuiStyle& style = ImGui::GetStyle();
  const ImVec2 start = ImGui::GetCursorScreenPos();
  const ImVec2 size(ImGui::GetContentRegionAvail().x, kLayerPanelDragHandleHeight);
  ImGui::InvisibleButton("##layer_panel_detach_handle", size);

  const bool handleActive = ImGui::IsItemActive();
  const bool handleHovered = ImGui::IsItemHovered();
  if (handleHovered || handleActive) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
  }

  const ImU32 background = ImGui::GetColorU32(handleActive    ? ImGuiCol_HeaderActive
                                              : handleHovered ? ImGuiCol_HeaderHovered
                                                              : ImGuiCol_Header);
  const ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);
  const ImVec2 end(start.x + size.x, start.y + size.y);
  ImDrawList* drawList = ImGui::GetWindowDrawList();
  drawList->AddRectFilled(start, end, background);
  drawList->AddText(ImVec2(start.x + style.FramePadding.x, start.y + style.FramePadding.y),
                    textColor, "Layers");

  const char* handleText = "::";
  const ImVec2 handleTextSize = ImGui::CalcTextSize(handleText);
  drawList->AddText(
      ImVec2(end.x - handleTextSize.x - style.FramePadding.x, start.y + style.FramePadding.y),
      textColor, handleText);

  if (handleActive && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
    layerPanelDetached_ = true;
    layerPanelDetachDragActive_ = true;
    layerPanelFloatingNeedsPlacement_ = true;
    layerPanelFloatingPos_ = ImGui::GetWindowPos();
    layerPanelFloatingSize_ = ImGui::GetWindowSize();
  }
}

void EditorShell::renderFloatingLayerPanel() {
  if (!layerPanelDetached_) {
    return;
  }

  if (layerPanelDetachDragActive_) {
    const ImGuiIO& io = ImGui::GetIO();
    layerPanelFloatingPos_.x += io.MouseDelta.x;
    layerPanelFloatingPos_.y += io.MouseDelta.y;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      layerPanelDetachDragActive_ = false;
    }
  }

  if (layerPanelFloatingNeedsPlacement_ || layerPanelDetachDragActive_) {
    ImGui::SetNextWindowPos(layerPanelFloatingPos_, ImGuiCond_Always);
  }
  if (layerPanelFloatingNeedsPlacement_) {
    ImGui::SetNextWindowSize(layerPanelFloatingSize_, ImGuiCond_Always);
  }

  bool layerPanelOpen = true;
  constexpr ImGuiWindowFlags kFloatingFlags = ImGuiWindowFlags_NoCollapse;
  ImGui::Begin("Layers##floating_layers", &layerPanelOpen, kFloatingFlags);
  layerPanelFloatingNeedsPlacement_ = false;
  layerPanelFloatingPos_ = ImGui::GetWindowPos();
  layerPanelFloatingSize_ = ImGui::GetWindowSize();

  if (!layerPanelOpen || ImGui::Button("Dock")) {
    layerPanelDetached_ = false;
    layerPanelDetachDragActive_ = false;
    layerPanelFloatingNeedsPlacement_ = false;
    ImGui::End();
    return;
  }

  renderLayerPanelContents();
  ImGui::End();
}

bool EditorShell::highlightSelectionSourceIfNeeded() {
  const auto& selectionNow = app_.selectedElements();
  const bool preserveSourceEditCursor =
      preserveSourceEditFocusCursor_ && sourceFocusMode_ && textEditor_.isCursorInsideFocusRange();
  if (selectionNow != lastHighlightedSelection_) {
    referenceHighlightActive_ = false;
    referenceHighlightChipHovered_ = false;
    referenceHighlightSummary_ = ReferenceHighlightSummary{};
    lastReferenceHighlightSelection_.clear();
    if (sourceSelectionOriginatedInText_ || preserveSourceEditCursor) {
      if (!sourceFocusOriginatedInStyle_) {
        updateSourceFocusView(/*scrollToSelection=*/false);
      }
      sourceSelectionOriginatedInText_ = false;
      preserveSourceEditFocusCursor_ = false;
    } else if (!selectionNow.empty()) {
      updateSourceFocusView(/*scrollToSelection=*/true);
    } else {
      textEditor_.clearFocusPartition();
    }
    lastHighlightedSelection_ = selectionNow;
    return true;
  }

  if (preserveSourceEditFocusCursor_ &&
      (!sourceFocusMode_ || !textEditor_.isCursorInsideFocusRange())) {
    preserveSourceEditFocusCursor_ = false;
  }

  return false;
}

std::optional<StyleFocus> EditorShell::styleFocusAtSourceOffset(std::size_t sourceOffset) const {
  std::optional<StyleFocus> styleFocus =
      ComputeStyleFocusAtSourceOffset(app_.document().document(), sourceOffset);
  if (!styleFocus.has_value() && sourceOffset > 0) {
    styleFocus = ComputeStyleFocusAtSourceOffset(app_.document().document(), sourceOffset - 1);
  }

  return styleFocus;
}

std::vector<svg::SVGElement> EditorShell::sourceHoverElements() const {
  if (!app_.hasDocument() || textEditor_.isTextChanged() || app_.document().hasPendingMutations()) {
    return {};
  }

  const std::optional<Coordinates> hoverPosition = textEditor_.hoveredTextPosition();
  if (!hoverPosition.has_value()) {
    return {};
  }

  const std::string documentSource = CanonicalizeForTextEditor(app_.document().document().source());
  const std::string editorSource = textEditor_.getText();
  if (editorSource != documentSource) {
    return {};
  }

  const std::size_t hoverOffset = textEditor_.getByteOffsetAtCoordinates(*hoverPosition);
  if (std::optional<StyleFocus> styleFocus = styleFocusAtSourceOffset(hoverOffset)) {
    return ExcludeSelectedSourceHoverElements(std::move(styleFocus->impactedElements),
                                              app_.selectedElements());
  }

  std::optional<svg::SVGElement> element =
      FindElementNearSourceOffset(app_.document().document(), editorSource, hoverOffset);
  if (!element.has_value()) {
    return {};
  }

  return ExcludeSelectedSourceHoverElements({*element}, app_.selectedElements());
}

std::vector<SourceByteRange> EditorShell::sourceHoverRangesForElements(
    const std::vector<svg::SVGElement>& elements) const {
  if (elements.empty() || !app_.hasDocument() || textEditor_.isTextChanged()) {
    return {};
  }

  const std::string documentSource = CanonicalizeForTextEditor(app_.document().document().source());
  if (textEditor_.getText() != documentSource) {
    return {};
  }

  std::vector<SourceByteRange> ranges;
  ranges.reserve(elements.size());
  for (const svg::SVGElement& element : elements) {
    if (std::optional<SourceByteRange> range = ElementSourceByteRange(element, documentSource)) {
      ranges.push_back(*range);
    }
  }
  return ranges;
}

std::vector<svg::SVGElement> EditorShell::referenceHighlightElements() const {
  std::vector<svg::SVGElement> elements;
  if (!referenceHighlightActive_ && !referenceHighlightChipHovered_) {
    return elements;
  }

  elements.reserve(referenceHighlightSummary_.totalCount());
  AddUniqueElements(&elements, referenceHighlightSummary_.referencedElements);
  AddUniqueElements(&elements, referenceHighlightSummary_.referencingElements);
  return elements;
}

std::vector<svg::SVGElement> EditorShell::combinedSourcePreviewElements() const {
  std::vector<svg::SVGElement> elements = sourceHoverElements();
  AddUniqueElements(&elements, referenceHighlightElements());
  return elements;
}

void EditorShell::updateSourceHoverPreview() {
  if (renderCoordinator_.asyncRenderer().isBusy()) {
    const bool overlayChanged = renderCoordinator_.setSourceHoverElements({});
    const bool sourceChanged = textEditor_.clearHoverSourceRanges();
    if (overlayChanged || sourceChanged) {
      window_.wakeEventLoop();
    }
    return;
  }

  const std::vector<svg::SVGElement> hoverElements = combinedSourcePreviewElements();
  const bool overlayChanged = renderCoordinator_.setSourceHoverElements(hoverElements);
  const bool sourceChanged =
      textEditor_.setHoverSourceRanges(sourceHoverRangesForElements(hoverElements));
  if (overlayChanged || sourceChanged) {
    window_.wakeEventLoop();
  }
}

void EditorShell::refreshReferenceHighlightSummaryIfNeeded() {
  const std::vector<svg::SVGElement>& selection = app_.selectedElements();
  if (selection == lastReferenceHighlightSelection_) {
    return;
  }

  referenceHighlightActive_ = false;
  referenceHighlightChipHovered_ = false;
  referenceHighlightSummary_ = ReferenceHighlightSummary{};
  if (!app_.hasDocument() || selection.empty()) {
    lastReferenceHighlightSelection_ = selection;
    return;
  }

  if (renderCoordinator_.asyncRenderer().isBusy()) {
    window_.wakeEventLoop();
    return;
  }

  lastReferenceHighlightSelection_ = selection;
  referenceHighlightSummary_ = ComputeReferenceHighlightSummary(
      app_.document().document(), std::span<const svg::SVGElement>(selection));
}

void EditorShell::applyReferenceHighlightPreview() {
  if (renderCoordinator_.asyncRenderer().isBusy()) {
    window_.wakeEventLoop();
    return;
  }

  const std::vector<svg::SVGElement> previewElements = combinedSourcePreviewElements();
  const bool overlayChanged = renderCoordinator_.setSourceHoverElements(previewElements);
  const bool sourceChanged =
      textEditor_.setHoverSourceRanges(sourceHoverRangesForElements(previewElements));
  if (overlayChanged) {
    renderCoordinator_.rasterizeOverlayForCurrentSelection(
        app_, interactionController_.viewport(), textures_, selectTool_.marqueeRect(),
        RenderCoordinator::OverlayUploadMode::Immediate, selectTool_.activeDragPreview(),
        selectTool_.activeTransformBoundsPreview());
  }
  if (overlayChanged || sourceChanged) {
    window_.wakeEventLoop();
  }
}

void EditorShell::setReferenceHighlightChipHovered(bool hovered) {
  if (referenceHighlightChipHovered_ == hovered) {
    return;
  }

  referenceHighlightChipHovered_ = hovered;
  applyReferenceHighlightPreview();
}

std::optional<Box2d> EditorShell::referenceHighlightChipScreenRect(std::string_view label) const {
  if (label.empty() || referenceHighlightSummary_.totalCount() <= 1 ||
      renderCoordinator_.selectionBoundsCache().lastSelection != app_.selectedElements() ||
      renderCoordinator_.selectionBoundsCache().displayedBoundsDoc.empty()) {
    return std::nullopt;
  }

  const Box2d selectionBounds =
      CombinedSelectionBounds(renderCoordinator_.selectionBoundsCache().displayedBoundsDoc);
  const Vector2d topLeft =
      interactionController_.viewport().documentToScreen(selectionBounds.topLeft);
  const Box2d imageRect = interactionController_.viewport().imageScreenRect();
  const ImVec2 textSize = ReferenceHighlightChipTextSize(label);
  const double width = static_cast<double>(textSize.x + 2.0f * kReferenceChipPaddingX);
  const double height = static_cast<double>(textSize.y + 2.0f * kReferenceChipPaddingY);
  const double maxX = std::max(imageRect.topLeft.x, imageRect.bottomRight.x - width);
  const double x = std::clamp(topLeft.x, imageRect.topLeft.x, maxX);
  double y = topLeft.y - height - kReferenceChipGapFromAabb;
  if (y < imageRect.topLeft.y) {
    y = topLeft.y + kReferenceChipGapFromAabb;
  }
  const double maxY = std::max(imageRect.topLeft.y, imageRect.bottomRight.y - height);
  y = std::clamp(y, imageRect.topLeft.y, maxY);
  return Box2d::FromXYWH(x, y, width, height);
}

void EditorShell::renderReferenceHighlightChip() {
  refreshReferenceHighlightSummaryIfNeeded();
  const std::string label = ReferenceHighlightChipLabel(referenceHighlightSummary_);
  const std::optional<Box2d> rect = referenceHighlightChipScreenRect(label);
  if (!rect.has_value()) {
    setReferenceHighlightChipHovered(false);
    return;
  }

  const ImVec2 mouse = ImGui::GetMousePos();
  const bool hovered = ContainsScreenPoint(*rect, mouse);
  setReferenceHighlightChipHovered(hovered);
  if (hovered) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left, /*repeat=*/false)) {
      referenceHighlightActive_ = !referenceHighlightActive_;
      applyReferenceHighlightPreview();
    }
  }

  ImDrawList* drawList = ImGui::GetWindowDrawList();
  const ImU32 bg = referenceHighlightActive_ ? IM_COL32(0, 135, 170, 245)
                   : hovered                 ? IM_COL32(48, 70, 78, 245)
                                             : IM_COL32(34, 48, 54, 235);
  drawList->AddRectFilled(
      ImVec2(static_cast<float>(rect->topLeft.x), static_cast<float>(rect->topLeft.y)),
      ImVec2(static_cast<float>(rect->bottomRight.x), static_cast<float>(rect->bottomRight.y)), bg,
      kReferenceChipRadius);
  drawList->AddText(ImGui::GetFont(), ReferenceHighlightChipFontSize(),
                    ImVec2(static_cast<float>(rect->topLeft.x) + kReferenceChipPaddingX,
                           static_cast<float>(rect->topLeft.y) + kReferenceChipPaddingY),
                    IM_COL32(255, 255, 255, 255), label.c_str(), label.c_str() + label.size());
}

void EditorShell::openRenderPaneContextMenu(const Vector2d& documentPoint) {
  renderContextMenuDocumentPoint_ = documentPoint;
  renderContextMenuHitElement_.reset();

  if (app_.hasDocument() && !renderCoordinator_.asyncRenderer().isBusy()) {
    if (std::optional<svg::SVGGeometryElement> hit = app_.hitTest(documentPoint)) {
      renderContextMenuHitElement_ = *hit;
    }
  }

  renderContextMenuOpenRequested_ = true;
  window_.wakeEventLoop();
}

void EditorShell::renderRenderPaneContextMenu() {
  if (renderContextMenuOpenRequested_) {
    ImGui::OpenPopup(kRenderPaneContextMenuName.data());
    renderContextMenuOpenRequested_ = false;
  }

  if (!ImGui::BeginPopup(kRenderPaneContextMenuName.data())) {
    return;
  }

  bool selectionChanged = false;
  const bool rendererBusy = renderCoordinator_.asyncRenderer().isBusy();
  if (!app_.hasDocument()) {
    ImGui::BeginDisabled();
    ImGui::MenuItem("No document loaded");
    ImGui::EndDisabled();
  } else if (rendererBusy && !renderContextMenuHitElement_.has_value()) {
    ImGui::BeginDisabled();
    ImGui::MenuItem("Renderer busy");
    ImGui::EndDisabled();
  } else if (renderContextMenuHitElement_.has_value()) {
    const svg::SVGElement hitElement = *renderContextMenuHitElement_;
    const std::string label = ElementContextMenuLabel(hitElement);
    ImGui::TextUnformatted(label.c_str(), label.c_str() + label.size());
    ImGui::Separator();

    const bool alreadySelected =
        ContainsElement(std::span<const svg::SVGElement>(app_.selectedElements()), hitElement);
    if (ImGui::MenuItem("Select Element")) {
      app_.setSelection(hitElement);
      selectionChanged = true;
    }
    if (ImGui::MenuItem("Add to Selection", nullptr, false, !alreadySelected)) {
      app_.addToSelection(hitElement);
      selectionChanged = true;
    }
  } else {
    ImGui::BeginDisabled();
    ImGui::MenuItem("No element under cursor");
    ImGui::EndDisabled();
  }

  ImGui::Separator();
  if (ImGui::MenuItem("Clear Selection", nullptr, false, app_.hasSelection())) {
    app_.clearSelection();
    selectionChanged = true;
  }
  if (ImGui::MenuItem("Delete Selection", nullptr, false, app_.hasSelection())) {
    selectionChanged = app_.deleteSelectionWithUndo(textEditor_.getText());
  }
  if (referenceHighlightSummary_.totalCount() > 1) {
    if (ImGui::MenuItem("Highlight Refs", nullptr, referenceHighlightActive_)) {
      referenceHighlightActive_ = !referenceHighlightActive_;
      applyReferenceHighlightPreview();
    }
  }
  if (ImGui::MenuItem("Show Source", nullptr, sourcePaneVisible_)) {
    setSourcePaneVisible(!sourcePaneVisible_);
  }
  if (ImGui::MenuItem("Source Focus Mode", nullptr, sourceFocusMode_)) {
    toggleSourceFocusMode();
  }

  if (selectionChanged) {
    referenceHighlightActive_ = false;
    referenceHighlightChipHovered_ = false;
    referenceHighlightSummary_ = ReferenceHighlightSummary{};
    lastReferenceHighlightSelection_.clear();
    updateSourceHoverPreview();
  }

  if (selectionChanged && !rendererBusy) {
    renderCoordinator_.refreshSelectionBoundsCache(app_);
    renderCoordinator_.rasterizeOverlayForCurrentSelection(
        app_, interactionController_.viewport(), textures_, selectTool_.marqueeRect(),
        RenderCoordinator::OverlayUploadMode::Immediate, selectTool_.activeDragPreview(),
        selectTool_.activeTransformBoundsPreview());
    window_.wakeEventLoop();
  }

  ImGui::EndPopup();
}

std::optional<StyleFocus> EditorShell::styleFocusAtSourceCursor() {
  if (!app_.hasDocument() || textEditor_.isTextChanged() || app_.document().hasPendingMutations()) {
    return std::nullopt;
  }
  const std::string documentSource = CanonicalizeForTextEditor(app_.document().document().source());
  if (textEditor_.getText() != documentSource) {
    return std::nullopt;
  }

  const std::size_t cursorOffset =
      textEditor_.getByteOffsetAtCoordinates(textEditor_.getCursorPosition());
  return styleFocusAtSourceOffset(cursorOffset);
}

void EditorShell::applyStyleFocus(StyleFocus styleFocus) {
  applySourcePartition(std::move(styleFocus.partition));
  sourceFocusOriginatedInStyle_ = true;
  sourceSelectionOriginatedInText_ = false;
  if (app_.selectedElements() != styleFocus.impactedElements) {
    app_.setSelection(std::move(styleFocus.impactedElements));
    sourceSelectionOriginatedInText_ = app_.selectedElements() != lastHighlightedSelection_;
  }
}

void EditorShell::syncSelectionFromSourceCursorIfNeeded() {
  const bool sourceCursorNavigationActive =
      textEditor_.isFocused() || textEditor_.didMouseChangeCursorPosition();
  if (!textEditor_.isCursorPositionChanged() || !sourceCursorNavigationActive ||
      !app_.hasDocument() || textEditor_.isTextChanged() || textEditor_.hasSelection()) {
    return;
  }

  const std::string documentSource = CanonicalizeForTextEditor(app_.document().document().source());
  if (textEditor_.getText() != documentSource) {
    return;
  }

  std::optional<StyleFocus> styleFocus = styleFocusAtSourceCursor();
  if (styleFocus.has_value()) {
    applyStyleFocus(std::move(*styleFocus));
    window_.wakeEventLoop();
    return;
  }

  if (sourceFocusOriginatedInStyle_ && textEditor_.isCursorInsideFocusRange()) {
    return;
  }

  std::optional<svg::SVGElement> element =
      FindElementAtSourceCursor(app_.document().document(), textEditor_);
  if (!element.has_value()) {
    return;
  }

  if (app_.selectedElements().size() == 1u && app_.selectedElements().front() == *element) {
    sourceSelectionOriginatedInText_ = false;
    if (sourceFocusOriginatedInStyle_) {
      updateSourceFocusView(/*scrollToSelection=*/false);
      window_.wakeEventLoop();
    }
    return;
  }

  app_.setSelection(*element);
  sourceSelectionOriginatedInText_ = app_.selectedElements() != lastHighlightedSelection_;
  updateSourceFocusView(/*scrollToSelection=*/false);
  window_.wakeEventLoop();
}

void EditorShell::applySourcePartition(FocusPartition partition) {
  if (!sourceFocusMode_) {
    partition.fullColor.clear();
    partition.dimmed.clear();
    partition.hidden.clear();
  }

  textEditor_.setFocusPartition(partition);
}

void EditorShell::updateSourceFocusView(bool scrollToSelection) {
  sourceFocusOriginatedInStyle_ = false;
  if (!app_.hasDocument() || app_.selectedElements().empty()) {
    textEditor_.clearFocusPartition();
    return;
  }

  const svg::SVGElement selected = *app_.selectedElement();
  applySourcePartition(ComputeFocusPartition(app_.document().document(), app_.selectedElements()));
  if (scrollToSelection) {
    std::ignore = HighlightElementSource(textEditor_, selected);
  }
}

void EditorShell::setSourceFocusMode(bool enabled) {
  sourceFocusMode_ = enabled;
  if (sourceFocusOriginatedInStyle_) {
    if (std::optional<StyleFocus> styleFocus = styleFocusAtSourceCursor()) {
      applyStyleFocus(std::move(*styleFocus));
      window_.wakeEventLoop();
      return;
    }
  }
  updateSourceFocusView(/*scrollToSelection=*/true);
  window_.wakeEventLoop();
}

void EditorShell::toggleSourceFocusMode() {
  setSourceFocusMode(!sourceFocusMode_);
}

void EditorShell::setSourcePaneVisible(bool visible) {
  if (sourcePaneVisible_ == visible) {
    return;
  }

  sourcePaneVisible_ = visible;
  if (!sourcePaneVisible_) {
    std::ignore = textEditor_.clearHoverSourceRanges();
    if (renderCoordinator_.setSourceHoverElements({}) &&
        !renderCoordinator_.asyncRenderer().isBusy()) {
      renderCoordinator_.rasterizeOverlayForCurrentSelection(
          app_, interactionController_.viewport(), textures_, selectTool_.marqueeRect(),
          RenderCoordinator::OverlayUploadMode::Immediate, selectTool_.activeDragPreview(),
          selectTool_.activeTransformBoundsPreview());
    }
  }
  window_.wakeEventLoop();
}

void EditorShell::runFrame() {
  ZoneScopedN("EditorShell::runFrame");
  textures_.advancePresentationFrame();
  layerInspectorPanel_.advancePresentationFrame();
  if (reproRecorder_) {
    // Snapshot before any widget consumes input events. ImGui's IO
    // state for the frame has been populated by
    // `ImGui_ImplGlfw_NewFrame` (called in `window_.beginFrame()`
    // upstream of `runFrame`); nothing below has touched it yet.
    //
    // The viewport snapshot is the OUTCOME of any previous frame's
    // viewport mutation (pinch-zoom, keyboard zoom, pan). Capturing
    // it every frame is what lets `RnrReplayTest`'s
    // `ApplyRecordedViewport` reconstruct zoom changes during
    // playback, even for gestures (macOS trackpad pinch via
    // `PinchEventMonitor`) that bypass ImGui's input boundary and
    // therefore aren't visible to the recorder as discrete events.
    const ViewportState& vp = interactionController_.viewport();
    repro::FrameContext frameContext;
    frameContext.viewport = repro::ReproViewport{
        .paneOriginX = vp.paneOrigin.x,
        .paneOriginY = vp.paneOrigin.y,
        .paneSizeW = vp.paneSize.x,
        .paneSizeH = vp.paneSize.y,
        .devicePixelRatio = vp.devicePixelRatio,
        .zoom = vp.zoom,
        .panDocX = vp.panDocPoint.x,
        .panDocY = vp.panDocPoint.y,
        .panScreenX = vp.panScreenPoint.x,
        .panScreenY = vp.panScreenPoint.y,
        .viewBoxX = vp.documentViewBox.topLeft.x,
        .viewBoxY = vp.documentViewBox.topLeft.y,
        .viewBoxW = vp.documentViewBox.size().x,
        .viewBoxH = vp.documentViewBox.size().y,
    };
    reproRecorder_->snapshotFrame(frameContext);
  }
  interactionController_.noteFrameDelta(ImGui::GetIO().DeltaTime * 1000.0f);
  updateWindowTitle();

  renderCoordinator_.pollRenderResult(app_, interactionController_.viewport(), textures_,
                                      &interactionController_.frameHistory());

  if (!renderCoordinator_.asyncRenderer().isBusy()) {
    if (app_.flushFrame()) {
      renderCoordinator_.refreshSelectionBoundsCache(app_);
    }
  }

  documentSyncController_.syncParseErrorMarkers(app_, textEditor_);
  documentSyncController_.applyPendingWritebacks(app_, selectTool_, textEditor_);

  const Vector2i windowSize = window_.windowSize();
  const float menuBarHeight = ImGui::GetFrameHeight();
  const float paneOriginY = menuBarHeight;
  const float paneHeight = std::max(0.0f, static_cast<float>(windowSize.y) - menuBarHeight);
  const EditorMainPaneLayout mainPaneLayout = ComputeEditorMainPaneLayout({
      .windowWidth = static_cast<float>(windowSize.x),
      .sourcePaneVisible = sourcePaneVisible_,
      .sourcePaneWidth = sourcePaneWidth_,
      .minSourcePaneWidth = kMinSourcePaneWidth,
      .maxSourcePaneWidth = kMaxSourcePaneWidth,
      .rightPaneWidth = rightPaneWidth_,
      .minRightPaneWidth = kMinRightPaneWidth,
      .maxRightPaneWidth = kMaxRightPaneWidth,
      .minRenderPaneWidth = kMinRightPaneWidth,
  });
  rightPaneWidth_ = mainPaneLayout.rightPaneWidth;
  const float rightPaneX = mainPaneLayout.rightPaneX;
  const float rightPaneGap = ImGui::GetStyle().ItemSpacing.y;
  const RightSidebarLayout rightSidebarLayout = ComputeRightSidebarLayout({
      .paneOriginY = paneOriginY,
      .paneHeight = paneHeight,
      .rightPaneGap = rightPaneGap,
      .treeViewHeightFraction = kTreeViewHeightFraction,
      .layerPanelHeightFraction = layerPanelHeightFraction_,
      .layerPanelDetached = layerPanelDetached_,
      .layerPanelSplitterThickness = kLayerPanelSplitterThickness,
      .minLayerPanelHeight = kMinLayerPanelHeight,
      .minInspectorPaneHeight = kMinInspectorPaneHeight,
  });
  layerPanelHeightFraction_ = rightSidebarLayout.layerPanelHeightFraction;
  const Vector2d renderPaneOrigin(mainPaneLayout.renderPaneX, paneOriginY);
  const Vector2d renderPaneSize(mainPaneLayout.renderPaneWidth, paneHeight);

  interactionController_.updatePaneLayout(
      renderPaneOrigin, renderPaneSize,
      app_.hasDocument() ? std::make_optional(ResolveDocumentViewBox(app_.document().document()))
                         : std::nullopt);

  handleGlobalShortcuts();

  MenuBarState menuState{
      .sourcePaneFocused = sourcePaneVisible_ && textEditor_.isFocused(),
      .canSave = app_.hasDocument(),
      .canRevert = app_.hasDocument() && app_.isDirty() && !app_.cleanSourceText().empty(),
      .canUndo = app_.canUndo(),
      .canRedo = app_.canRedo(),
      .sourceFocusMode = sourceFocusMode_,
  };
  const MenuBarActions menuActions = menuBarPresenter_.render(menuState, uiFontBold_);
  if (menuActions.openAbout) {
    dialogPresenter_.requestAbout();
  }
  if (menuActions.openFile) {
    dialogPresenter_.requestOpenFile(app_.currentFilePath());
  }
  if (menuActions.saveFile) {
    requestSave();
  }
  if (menuActions.saveFileAs) {
    requestSaveAs();
  }
  if (menuActions.revertFile) {
    requestRevert();
  }
  if (menuActions.quit) {
    glfwSetWindowShouldClose(window_.rawHandle(), GLFW_TRUE);
  }
  if (menuActions.undo && app_.canUndo()) {
    app_.undo();
  }
  if (menuActions.redo && app_.canRedo()) {
    app_.redo();
  }
  if (menuActions.cut) {
    textEditor_.cut();
  }
  if (menuActions.copy) {
    textEditor_.copy();
  }
  if (menuActions.paste) {
    textEditor_.paste();
  }
  if (menuActions.selectAll) {
    textEditor_.selectAll();
  }
  if (menuActions.zoomIn) {
    interactionController_.applyZoom(kKeyboardZoomStep,
                                     interactionController_.viewport().paneCenter());
  }
  if (menuActions.zoomOut) {
    interactionController_.applyZoom(1.0 / kKeyboardZoomStep,
                                     interactionController_.viewport().paneCenter());
  }
  if (menuActions.actualSize) {
    interactionController_.resetToActualSize();
  }
  if (menuActions.toggleSourceFocusMode) {
    toggleSourceFocusMode();
  }

  dialogPresenter_.render(
      [this](std::string_view path, std::string* error) { return tryOpenPath(path, error); },
      [this](std::string_view path, std::string* error) { return trySavePath(path, error); });

  constexpr ImGuiWindowFlags kPaneFlags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                          ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar;
  std::ignore = highlightSelectionSourceIfNeeded();
  if (sourcePaneVisible_) {
    renderSourcePane(paneOriginY, paneHeight, mainPaneLayout.sourcePaneWidth, codeFont_);
  }
  renderRenderPane(renderPaneOrigin, renderPaneSize, kPaneFlags);
  renderSidebars(rightPaneX, rightPaneWidth_, paneOriginY, rightSidebarLayout, kPaneFlags);
  if (highlightSelectionSourceIfNeeded()) {
    window_.wakeEventLoop();
  }
  renderSourcePaneSplitter(static_cast<float>(windowSize.x), paneOriginY, paneHeight,
                           mainPaneLayout.sourcePaneWidth);
  renderRightPaneSplitter(static_cast<float>(windowSize.x), paneOriginY, paneHeight);
  if (!layerPanelDetached_) {
    renderLayerPanelSplitter(rightPaneX, rightPaneWidth_, rightSidebarLayout);
  }
  renderFloatingLayerPanel();
  if (documentSyncController_.nextTextSyncWakeSeconds().has_value() ||
      textEditor_.nextFlashWakeSeconds().has_value()) {
    window_.wakeEventLoop();
  }
}

}  // namespace donner::editor
