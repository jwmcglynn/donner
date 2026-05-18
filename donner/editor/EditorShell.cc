#define IMGUI_DEFINE_MATH_OPERATORS
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
#include "donner/editor/DragCoalesce.h"
#include "donner/editor/KeyboardShortcutPolicy.h"
#include "donner/editor/SourceSelection.h"
#include "donner/editor/SourceSync.h"
#include "donner/editor/TracyWrapper.h"
#include "donner/editor/gui/EditorWindow.h"
#include "donner/editor/repro/ReproRecorder.h"
#include "embed_resources/FiraCodeFont.h"
#include "embed_resources/RobotoFont.h"

namespace donner::editor {

namespace {

constexpr float kSourcePaneWidth = 560.0f;
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
  lastPostedScreenPoint_.reset();
  renderCoordinator_.resetForLoadedDocument();
  textures_.resetComposited();
  textures_.clearOverlay();
  renderCoordinator_.refreshSelectionBoundsCache(app_);
  dialogPresenter_.clearOpenFileError();
  return true;
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
                                        textures_);

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

  // Design doc 0033 §M8 — click→drag handoff doesn't wait for raster.
  //
  // Fast path: if the user clicks inside the bounds of the currently-
  // selected element, we can start the re-drag IMMEDIATELY without
  // gating on `!isBusy()`. The check uses `SelectionBoundsCache::
  // displayedBoundsDoc` — populated on idle frames — so the call
  // doesn't touch the registry the worker is mid-mutating. The
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
    const bool tookFastRedrag =
        cacheMatchesSelection && selectTool_.tryStartRedragOnSelected(
                                     app_, pendingClick.documentPoint, pendingClick.modifiers,
                                     boundsCache.displayedBoundsDoc);
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
          app_, interactionController_.viewport(), textures_, selectTool_.marqueeRect());
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
        selectTool_.onMouseMove(app_, screenToDocument(currentScreen), /*buttonHeld=*/true);
        lastPostedScreenPoint_ = currentScreen;
        if (!renderCoordinator_.asyncRenderer().isBusy() && app_.flushFrame()) {
          renderCoordinator_.refreshSelectionBoundsCache(app_);
        }
      }
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
      const auto previewBeforeRelease = selectTool_.activeDragPreview();
      selectTool_.onMouseUp(app_, screenToDocument(ImGui::GetMousePos()));
      lastPostedScreenPoint_.reset();
      if (previewBeforeRelease.has_value()) {
        // The DOM was already updated every drag frame via
        // `SelectTool::onMouseMove` → `applyMutation`, so drag release
        // needs to do nothing beyond recording undo history (already done
        // in `onMouseUp`). The compositor has the dragged entity's DOM
        // position baked in; its cached bitmap is reused via an internal
        // compose-offset delta for pure-translation drags (see
        // `CompositorController::rasterizeLayer` + fast path), which
        // means the display is byte-for-byte identical to a fresh render
        // of the mutated DOM at identity composition. No "settling" hack
        // needed — the compositor view IS the settled view.
        if (!renderCoordinator_.asyncRenderer().isBusy() && app_.flushFrame()) {
          renderCoordinator_.refreshSelectionBoundsCache(app_);
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

  const auto activeDragPreview = selectTool_.activeDragPreview();
  const auto displayedDragPreview =
      renderCoordinator_.compositedPresentation().presentationPreview(activeDragPreview);
  RenderPanePresenterState paneState{
      .viewport = interactionController_.viewport(),
      .frameHistory = interactionController_.frameHistory(),
      .textures = textures_,
      .activeDragPreview = activeDragPreview,
      .displayedDragPreview = displayedDragPreview,
      .contentRegion = Vector2d(contentRegion.x, contentRegion.y),
  };
  renderPanePresenter_.render(paneState);

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
  const auto& selectionNow = app_.selectedElement();
  if (selectionNow != lastHighlightedSelection_) {
    if (selectionNow.has_value()) {
      std::ignore = HighlightElementSource(textEditor_, *selectionNow);
    }
    lastHighlightedSelection_ = selectionNow;
    return true;
  }

  return false;
}

void EditorShell::runFrame() {
  ZoneScopedN("EditorShell::runFrame");
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
  rightPaneWidth_ =
      std::clamp(rightPaneWidth_, kMinRightPaneWidth,
                 std::max(kMinRightPaneWidth,
                          std::min(kMaxRightPaneWidth, static_cast<float>(windowSize.x) -
                                                           kSourcePaneWidth - kMinRightPaneWidth)));
  const float renderPaneWidth =
      std::max(0.0f, static_cast<float>(windowSize.x) - kSourcePaneWidth - rightPaneWidth_);
  const float rightPaneX = static_cast<float>(windowSize.x) - rightPaneWidth_;
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
      .tightBoundedSegmentsEnabled =
          renderCoordinator_.asyncRenderer().tightBoundedSegmentsEnabled(),
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
  if (menuActions.toggleTightBoundedSegments) {
    auto& asyncRenderer = renderCoordinator_.asyncRenderer();
    asyncRenderer.setTightBoundedSegmentsEnabled(!asyncRenderer.tightBoundedSegmentsEnabled());
  }

  dialogPresenter_.render(
      [this](std::string_view path, std::string* error) { return tryOpenPath(path, error); });

  constexpr ImGuiWindowFlags kPaneFlags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                          ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar;
  std::ignore = highlightSelectionSourceIfNeeded();
  renderSourcePane(paneOriginY, paneHeight, codeFont_);
  renderRenderPane(renderPaneOrigin, renderPaneSize, kPaneFlags);
  renderSidebars(rightPaneX, rightPaneWidth_, paneOriginY, rightSidebarLayout, kPaneFlags);
  if (highlightSelectionSourceIfNeeded()) {
    window_.wakeEventLoop();
  }
  renderRightPaneSplitter(static_cast<float>(windowSize.x), paneOriginY, paneHeight);
  if (!layerPanelDetached_) {
    renderLayerPanelSplitter(rightPaneX, rightPaneWidth_, rightSidebarLayout);
  }
  renderFloatingLayerPanel();
}

}  // namespace donner::editor
