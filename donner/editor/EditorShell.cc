#include "donner/editor/EditorShell.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>

#include "GLFW/glfw3.h"
#include "donner/base/FileOffset.h"
#include "donner/base/xml/XMLNode.h"
#include "donner/editor/KeyboardShortcutPolicy.h"
#include "donner/editor/SourceSync.h"
#include "embed_resources/FiraCodeFont.h"
#include "embed_resources/RobotoFont.h"

namespace donner::editor {

namespace {

constexpr float kSourcePaneWidth = 560.0f;
constexpr float kInspectorPaneWidth = 320.0f;
constexpr float kTreeViewHeightFraction = 0.4f;
constexpr float kKeyboardZoomStep = 1.5f;
constexpr double kTrackpadPanPixelsPerScrollUnit = 10.0;
constexpr double kWheelZoomStep = 1.1;

std::optional<std::string> LoadFile(const std::string& filename) {
  std::ifstream file(filename, std::ios::binary);
  if (!file) {
    return std::nullopt;
  }
  std::ostringstream out;
  out << file.rdbuf();
  return std::move(out).str();
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

Coordinates FileOffsetToEditorCoordinates(const FileOffset& offset) {
  if (!offset.lineInfo.has_value()) {
    return Coordinates(0, 0);
  }
  return Coordinates(static_cast<int>(offset.lineInfo->line) - 1,
                     static_cast<int>(offset.lineInfo->offsetOnLine));
}

void HighlightElementSource(TextEditor& textEditor, const svg::SVGElement& element) {
  auto xmlNode = xml::XMLNode::TryCast(element.entityHandle());
  if (!xmlNode.has_value()) {
    return;
  }
  auto range = xmlNode->getNodeLocation();
  if (!range.has_value()) {
    return;
  }
  textEditor.selectAndFocus(FileOffsetToEditorCoordinates(range->start),
                            FileOffsetToEditorCoordinates(range->end));
}

}  // namespace

EditorShell::EditorShell(gui::EditorWindow& window, EditorShellOptions options)
    : window_(window),
      options_(std::move(options)),
      app_(),
      selectTool_(),
      textEditor_(),
      textures_(),
      renderCoordinator_(),
      documentSyncController_(LoadFile(options_.svgPath).value_or("")),
      interactionController_(),
      inputBridge_(window_, kWheelZoomStep),
      dialogPresenter_(options_.editorNoticeText) {
  std::optional<std::string> initialSource = options_.initialSource;
  if (!initialSource.has_value() && !options_.svgPath.empty()) {
    initialSource = LoadFile(options_.svgPath);
  }
  if (!initialSource.has_value()) {
    return;
  }

  selectTool_.setCompositedDragPreviewEnabled(options_.experimentalMode);

  textEditor_.setLanguageDefinition(TextEditor::LanguageDefinition::SVG());
  textEditor_.setText(*initialSource);
  textEditor_.resetTextChanged();
  textEditor_.setActiveAutocomplete(true);

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

  app_.setStructuredEditingEnabled(true);
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
  valid_ = true;
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
  documentSyncController_.resetForLoadedDocument(canonicalSource);
  app_.setCurrentFilePath(std::string(path));
  app_.setCleanSourceText(canonicalSource);
  lastHighlightedSelection_.reset();
  renderCoordinator_.resetForLoadedDocument();
  textures_.resetComposited();
  textures_.clearOverlay();
  renderCoordinator_.refreshSelectionBoundsCache(app_);
  dialogPresenter_.clearOpenFileError();
  return true;
}

void EditorShell::applyExperimentalModeChange(bool enabled) {
  options_.experimentalMode = enabled;
  selectTool_.setCompositedDragPreviewEnabled(enabled);
  renderCoordinator_.experimentalDragPresentation() = ExperimentalDragPresentation{};
  textures_.resetComposited();
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
  const bool sourcePaneFocused = textEditor_.isFocused();

  if (!anyPopupOpen && cmd && !shift && ImGui::IsKeyPressed(ImGuiKey_O, /*repeat=*/false)) {
    dialogPresenter_.requestOpenFile(app_.currentFilePath());
  }

  if (!anyPopupOpen && cmd && !shift && ImGui::IsKeyPressed(ImGuiKey_Q, /*repeat=*/false)) {
    glfwSetWindowShouldClose(window_.rawHandle(), GLFW_TRUE);
  }

  if (!sourcePaneFocused) {
    if (pressedZ && cmd && !shift) {
      if (app_.canUndo()) {
        app_.undo();
      }
    } else if (pressedZ && cmd && shift) {
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

  if (!anyPopupOpen && ImGui::IsKeyPressed(ImGuiKey_Escape, /*repeat=*/false) &&
      app_.hasSelection()) {
    app_.setSelection(std::nullopt);
  }

  const bool deleteKey = ImGui::IsKeyPressed(ImGuiKey_Delete, /*repeat=*/false) ||
                         ImGui::IsKeyPressed(ImGuiKey_Backspace, /*repeat=*/false);
  if (CanDeleteSelectedElementsFromShortcut(deleteKey, app_.hasSelection(), anyPopupOpen,
                                            sourcePaneFocused)) {
    const std::vector<svg::SVGElement> selected = app_.selectedElements();
    app_.setSelection(std::nullopt);
    for (const auto& element : selected) {
      if (auto target = captureAttributeWritebackTarget(element); target.has_value()) {
        app_.enqueueElementRemoveWriteback(EditorApp::CompletedElementRemoveWriteback{
            .target = *target,
        });
      }
      app_.applyMutation(EditorCommand::DeleteElementCommand(element));
    }
  }
}

void EditorShell::renderSourcePane(float paneOriginY, float paneHeight, ImFont* codeFont) {
  constexpr ImGuiWindowFlags kPaneFlags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                          ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar;
  ImGui::SetNextWindowPos(ImVec2(0.0f, paneOriginY), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(kSourcePaneWidth, paneHeight), ImGuiCond_Always);
  ImGui::Begin("Source", nullptr, kPaneFlags);
  ImGui::PushFont(codeFont);
  textEditor_.render("##source");
  ImGui::PopFont();
  documentSyncController_.handleTextEdits(app_, textEditor_, ImGui::GetIO().DeltaTime);
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

  if (!viewportInitialized_ && interactionController_.viewport().paneSize.x > 0.0 &&
      interactionController_.viewport().paneSize.y > 0.0 && app_.hasDocument()) {
    interactionController_.resetToActualSize();
    viewportInitialized_ = true;
  }

  renderCoordinator_.maybeRequestRender(app_, selectTool_, interactionController_.viewport(),
                                        options_.experimentalMode, textures_);

  ImGui::InvisibleButton("##render_canvas", contentRegion,
                         ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
  const bool paneHovered = ImGui::IsItemHovered();
  const Box2d paneRect = Box2d::FromXYWH(interactionController_.viewport().paneOrigin.x,
                                         interactionController_.viewport().paneOrigin.y,
                                         interactionController_.viewport().paneSize.x,
                                         interactionController_.viewport().paneSize.y);

  const bool spaceHeld = ImGui::IsKeyDown(ImGuiKey_Space);
  const bool middleDown = ImGui::IsMouseDown(ImGuiMouseButton_Middle);
  interactionController_.updatePanState(paneHovered, spaceHeld, middleDown,
                                        ImGui::IsMouseDown(ImGuiMouseButton_Left),
                                        ImGui::GetMousePos());

  const bool modalCapturingInput = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup);
  interactionController_.consumeScrollEvents(inputBridge_.events(), paneRect, modalCapturingInput,
                                             kWheelZoomStep, kTrackpadPanPixelsPerScrollUnit);

  const auto activeDragPreview = selectTool_.activeDragPreview();
  const auto displayedDragPreview =
      renderCoordinator_.experimentalDragPresentation().presentationPreview(activeDragPreview);

  RenderPanePresenterState paneState{
      .viewport = interactionController_.viewport(),
      .frameHistory = interactionController_.frameHistory(),
      .textures = textures_,
      .experimentalDragPresentation = renderCoordinator_.experimentalDragPresentation(),
      .activeDragPreview = activeDragPreview,
      .displayedDragPreview = displayedDragPreview,
      .selectionBoundsDoc =
          std::span<const Box2d>(renderCoordinator_.selectionBoundsCache().displayedBoundsDoc),
      .marqueeRectDoc = selectTool_.marqueeRect(),
      .contentRegion = Vector2d(contentRegion.x, contentRegion.y),
      .experimentalMode = options_.experimentalMode,
  };
  renderPanePresenter_.render(paneState);

  const auto screenToDocument = [&](const ImVec2& screenPoint) -> Vector2d {
    return interactionController_.viewport().screenToDocument(
        Vector2d(screenPoint.x, screenPoint.y));
  };

  const bool toolEligible = paneHovered && !interactionController_.panning() && !spaceHeld;
  if (toolEligible && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    MouseModifiers modifiers;
    modifiers.shift = ImGui::GetIO().KeyShift;
    interactionController_.bufferPendingClick(screenToDocument(ImGui::GetMousePos()), modifiers);
  }

  if (interactionController_.pendingClick().has_value() &&
      !renderCoordinator_.asyncRenderer().isBusy()) {
    selectTool_.onMouseDown(app_, interactionController_.pendingClick()->documentPoint,
                            interactionController_.pendingClick()->modifiers);
    renderCoordinator_.refreshSelectionBoundsCache(app_);
    renderCoordinator_.maybeRequestRender(app_, selectTool_, interactionController_.viewport(),
                                          options_.experimentalMode, textures_);
    renderCoordinator_.rasterizeOverlayForCurrentSelection(app_, interactionController_.viewport(),
                                                           textures_);
    interactionController_.clearPendingClick();
  }

  if (selectTool_.isDragging() || selectTool_.isMarqueeing()) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && !spaceHeld) {
      selectTool_.onMouseMove(app_, screenToDocument(ImGui::GetMousePos()), /*buttonHeld=*/true);
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
      const auto previewBeforeRelease = selectTool_.activeDragPreview();
      selectTool_.onMouseUp(app_, screenToDocument(ImGui::GetMousePos()));
      if (options_.experimentalMode && previewBeforeRelease.has_value()) {
        // Zero re-renders on drag release.
        //
        // Apply the `SetTransformCommand` mutation so the DOM reflects the
        // new position, but do NOT kick off a "settle" render and do NOT
        // call `maybeRequestRender`. The compositor's last-drag-frame
        // composited preview (drag layer bitmap at pre-drag position
        // composed with the final drag translate) is already visually
        // identical to the post-mutation DOM state (entity at old + drag
        // = entity at new). Firing a render here would only re-rasterize
        // the drag layer to achieve the same pixel output, costing a
        // visible "(rendering…)" flash for no user-visible benefit.
        //
        // The re-rasterization cost is deferred to the next interaction
        // that genuinely needs fresh state: a new drag on this entity
        // takes the fast path and re-rasterizes the drag layer with the
        // baked-in transform; a selection change pays the prepare cost;
        // a pan/zoom triggers a regular render. Whichever fires first
        // absorbs the work naturally under the banner of an action the
        // user initiated.
        if (!renderCoordinator_.asyncRenderer().isBusy() && app_.flushFrame()) {
          renderCoordinator_.refreshSelectionBoundsCache(app_);
        }

        // Preserve the drag translation visually so the display keeps
        // showing the element at its drop position (cached-bitmap +
        // drag-translate offset). Also tells `shouldPrewarm()` that we
        // consider the cache fresh for the new document version — without
        // this, the next main-loop tick would see `cachedVersion !=
        // currentVersion` and fire a prewarm render.
        const auto currentVersion = app_.document().currentFrameVersion();
        const Vector2i canvasSize = app_.document().document().canvasSize();
        renderCoordinator_.experimentalDragPresentation().recordPostDragSettledWithoutRender(
            *previewBeforeRelease, currentVersion, canvasSize);
      } else if (!renderCoordinator_.asyncRenderer().isBusy()) {
        renderCoordinator_.refreshSelectionBoundsCache(app_);
        renderCoordinator_.rasterizeOverlayForCurrentSelection(
            app_, interactionController_.viewport(), textures_);
      }
    }
  }

  if (options_.experimentalMode && !renderCoordinator_.asyncRenderer().isBusy() &&
      app_.hasDocument()) {
    renderCoordinator_.maybeRequestRender(app_, selectTool_, interactionController_.viewport(),
                                          options_.experimentalMode, textures_);
  }

  highlightSelectionSourceIfNeeded();
  ImGui::End();
}

void EditorShell::renderSidebars(float rightPaneX, float paneOriginY, float treePaneHeight,
                                 float inspectorPaneY, float inspectorPaneHeight,
                                 ImGuiWindowFlags paneFlags) {
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
  ImGui::SetNextWindowSize(ImVec2(kInspectorPaneWidth, treePaneHeight), ImGuiCond_Always);
  ImGui::Begin("Tree View", nullptr, paneFlags);
  TreeViewState treeState{
      .scrollTarget = selectionBeforeTree,
      .pendingScroll = treeviewPendingScroll_,
  };
  sidebarPresenter_.renderTreeView(liveAppForClicks, treeState);
  treeviewPendingScroll_ = treeState.pendingScroll;
  if (treeState.selectionChangedInTree) {
    treeSelectionOriginatedInTree_ = true;
    treeviewPendingScroll_ = false;
  }
  ImGui::End();
  lastTreeSelection_ = app_.selectedElement();

  ImGui::SetNextWindowPos(ImVec2(rightPaneX, inspectorPaneY), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(kInspectorPaneWidth, inspectorPaneHeight), ImGuiCond_Always);
  ImGui::Begin("Inspector", nullptr, paneFlags);
  sidebarPresenter_.renderInspector(interactionController_.viewport());
  ImGui::End();
}

void EditorShell::highlightSelectionSourceIfNeeded() {
  const auto& selectionNow = app_.selectedElement();
  if (selectionNow != lastHighlightedSelection_) {
    if (selectionNow.has_value()) {
      HighlightElementSource(textEditor_, *selectionNow);
    }
    lastHighlightedSelection_ = selectionNow;
  }
}

void EditorShell::runFrame() {
  interactionController_.noteFrameDelta(ImGui::GetIO().DeltaTime * 1000.0f);
  updateWindowTitle();

  renderCoordinator_.pollRenderResult(app_, interactionController_.viewport(), textures_);

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
  const float renderPaneWidth =
      std::max(0.0f, static_cast<float>(windowSize.x) - kSourcePaneWidth - kInspectorPaneWidth);
  const float rightPaneX = static_cast<float>(windowSize.x) - kInspectorPaneWidth;
  const float rightPaneGap = ImGui::GetStyle().ItemSpacing.y;
  const float rightPaneContentHeight = std::max(0.0f, paneHeight - rightPaneGap);
  const float treePaneHeight = rightPaneContentHeight * kTreeViewHeightFraction;
  const float inspectorPaneY = paneOriginY + treePaneHeight + rightPaneGap;
  const float inspectorPaneHeight = std::max(0.0f, paneHeight - treePaneHeight - rightPaneGap);
  const Vector2d renderPaneOrigin(kSourcePaneWidth, paneOriginY);
  const Vector2d renderPaneSize(renderPaneWidth, paneHeight);

  interactionController_.updatePaneLayout(
      renderPaneOrigin, renderPaneSize,
      app_.hasDocument() ? std::make_optional(ResolveDocumentViewBox(app_.document().document()))
                         : std::nullopt);

  handleGlobalShortcuts();

  MenuBarState menuState{
      .sourcePaneFocused = textEditor_.isFocused(),
      .canUndo = app_.canUndo(),
      .canRedo = app_.undoTimeline().entryCount() > 0,
      .experimentalMode = options_.experimentalMode,
      .canToggleCompositedRendering = CanToggleCompositedRendering(selectTool_),
  };
  const MenuBarActions menuActions = menuBarPresenter_.render(menuState, uiFontBold_);
  if (menuActions.openAbout) {
    dialogPresenter_.requestAbout();
  }
  if (menuActions.openFile) {
    dialogPresenter_.requestOpenFile(app_.currentFilePath());
  }
  if (menuActions.quit) {
    glfwSetWindowShouldClose(window_.rawHandle(), GLFW_TRUE);
  }
  if (menuActions.undo && app_.canUndo()) {
    app_.undo();
  }
  if (menuActions.redo) {
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
  if (menuActions.toggleCompositedRendering) {
    applyExperimentalModeChange(!options_.experimentalMode);
  }

  dialogPresenter_.render(
      [this](std::string_view path, std::string* error) { return tryOpenPath(path, error); });

  constexpr ImGuiWindowFlags kPaneFlags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                          ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar;
  renderSourcePane(paneOriginY, paneHeight, codeFont_);
  renderRenderPane(renderPaneOrigin, renderPaneSize, kPaneFlags);
  renderSidebars(rightPaneX, paneOriginY, treePaneHeight, inspectorPaneY, inspectorPaneHeight,
                 kPaneFlags);
}

}  // namespace donner::editor
