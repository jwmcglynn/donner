/// @file
///
/// `//donner/editor:editor` — the full-featured Donner editor binary.
///
/// This is the advanced editor application. It wires `EditorApp`,
/// `SelectTool`, `OverlayRenderer`, and `TextEditor` into an ImGui
/// shell with a live source pane and a click-and-drag render pane.
///
/// Run with:
///
/// ```sh
/// bazel run //donner/editor -- donner_splash.svg
/// ```
///
/// For the minimal viewer demo (no tools, no overlay chrome, just a
/// rendered SVG and a text pane) see `//examples:svg_viewer`.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "donner/base/FileOffset.h"
#include "donner/base/ParseDiagnostic.h"
#include "donner/base/xml/XMLNode.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/EditorCommand.h"
#include "donner/editor/Notice.h"
#include "donner/editor/OverlayRenderer.h"
#include "donner/editor/SelectTool.h"
#include "donner/editor/TextBuffer.h"
#include "donner/editor/TextEditor.h"
#include "donner/editor/TracyWrapper.h"
#include "donner/svg/SVGGeometryElement.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/renderer/Renderer.h"

#include "glad/glad.h"

extern "C" {
#include "GLFW/glfw3.h"
}

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

namespace {

constexpr int kInitialWindowWidth = 1280;
constexpr int kInitialWindowHeight = 800;
constexpr float kSourcePaneWidth = 560.0f;
constexpr float kMinZoom = 0.1f;
constexpr float kMaxZoom = 32.0f;
constexpr float kZoomStep = 1.1f;  // Per wheel notch.
constexpr std::size_t kFrameHistoryCapacity = 120;  // 2s @ 60fps.
constexpr float kTargetFrameMs = 1000.0f / 60.0f;
constexpr float kFrameGraphWidth = 240.0f;
constexpr float kFrameGraphHeight = 32.0f;

/// Fixed-size ring buffer of recent frame times (in milliseconds). Used by
/// the diagnostic frame-time graph drawn in the Render pane header.
struct FrameHistory {
  std::array<float, kFrameHistoryCapacity> deltaMs{};
  std::size_t writeIndex = 0;
  std::size_t samples = 0;

  void push(float ms) {
    deltaMs[writeIndex] = ms;
    writeIndex = (writeIndex + 1) % kFrameHistoryCapacity;
    if (samples < kFrameHistoryCapacity) {
      ++samples;
    }
  }

  [[nodiscard]] float latest() const {
    if (samples == 0) {
      return 0.0f;
    }
    const std::size_t idx =
        (writeIndex + kFrameHistoryCapacity - 1) % kFrameHistoryCapacity;
    return deltaMs[idx];
  }

  [[nodiscard]] float max() const {
    float m = 0.0f;
    for (std::size_t i = 0; i < samples; ++i) {
      m = std::max(m, deltaMs[i]);
    }
    return m;
  }
};

/// Render a compact bar graph of the recent frame-time history plus the
/// most recent frame's ms / FPS as text. Each bar is one sample; red if
/// over the 16.67ms/60fps budget, green otherwise. A horizontal line
/// marks the target budget. Samples read left-to-right in chronological
/// order (oldest on the left).
void RenderFrameGraph(const FrameHistory& history) {
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();

  // Background panel + budget line.
  const ImVec2 bottomRight(origin.x + kFrameGraphWidth, origin.y + kFrameGraphHeight);
  dl->AddRectFilled(origin, bottomRight, IM_COL32(30, 30, 30, 255));

  // Vertical scale: the higher of (2× budget) and the history max, so
  // the budget line sits at the 50% mark in steady-state and the graph
  // autoscales when frames go long.
  const float scaleMs = std::max(kTargetFrameMs * 2.0f, history.max());

  // Budget line — horizontal guide at kTargetFrameMs.
  const float budgetY = origin.y + kFrameGraphHeight - (kTargetFrameMs / scaleMs) * kFrameGraphHeight;
  dl->AddLine(ImVec2(origin.x, budgetY), ImVec2(bottomRight.x, budgetY),
              IM_COL32(255, 255, 255, 80), 1.0f);

  // Bars — oldest sample on the left. Each sample occupies
  // `kFrameGraphWidth / kFrameHistoryCapacity` pixels (2px at default
  // size), which is wide enough to eyeball individual frame spikes.
  const float barWidth = kFrameGraphWidth / static_cast<float>(kFrameHistoryCapacity);
  for (std::size_t i = 0; i < history.samples; ++i) {
    const std::size_t readIdx =
        (history.writeIndex + kFrameHistoryCapacity - history.samples + i) %
        kFrameHistoryCapacity;
    const float ms = history.deltaMs[readIdx];
    const float clamped = std::min(ms, scaleMs);
    const float barHeight = (clamped / scaleMs) * kFrameGraphHeight;
    const bool overBudget = ms > kTargetFrameMs;
    const ImU32 color =
        overBudget ? IM_COL32(220, 60, 60, 255) : IM_COL32(80, 200, 100, 255);

    const float x = origin.x + static_cast<float>(i) * barWidth;
    dl->AddRectFilled(ImVec2(x, origin.y + kFrameGraphHeight - barHeight),
                      ImVec2(x + barWidth, origin.y + kFrameGraphHeight), color);
  }

  // Advance the ImGui cursor past the graph so subsequent widgets lay out
  // to the right / below without overlapping.
  ImGui::Dummy(ImVec2(kFrameGraphWidth, kFrameGraphHeight));
  ImGui::SameLine();
  const float latestMs = history.latest();
  const float fps = latestMs > 0.0f ? 1000.0f / latestMs : 0.0f;
  const ImU32 textColor =
      latestMs > kTargetFrameMs ? IM_COL32(220, 60, 60, 255) : IM_COL32(255, 255, 255, 255);
  ImGui::PushStyleColor(ImGuiCol_Text, textColor);
  ImGui::Text("%.2f ms / %.1f FPS", latestMs, fps);
  ImGui::PopStyleColor();
}

/// Pan + zoom state for the render pane. At zoom 1.0 and pan (0, 0), the
/// rendered bitmap exactly fills the pane's content region. Zoom scales
/// the image around the cursor; pan offsets its origin in pane-local
/// pixels.
struct ViewTransform {
  float zoom = 1.0f;
  ImVec2 pan = ImVec2(0.0f, 0.0f);

  void reset() {
    zoom = 1.0f;
    pan = ImVec2(0.0f, 0.0f);
  }
};

void GlfwErrorCallback(int error, const char* description) {
  std::cerr << "GLFW error " << error << ": " << description << "\n";
}

/// Build a `TextEditor::ErrorMarkers` map from a parser diagnostic. The
/// map is keyed by 1-based line number (`TextEditor`'s convention) and
/// values are the human-readable reason. Diagnostics without resolved
/// line info land on line 1 so the user always sees *something*.
donner::editor::TextEditor::ErrorMarkers ParseErrorToMarkers(
    const donner::ParseDiagnostic& diag) {
  donner::editor::TextEditor::ErrorMarkers markers;
  const auto& start = diag.range.start;
  const int line = start.lineInfo.has_value() ? static_cast<int>(start.lineInfo->line) : 1;
  markers.emplace(line, std::string(std::string_view(diag.reason)));
  return markers;
}

/// Convert a donner `FileOffset` (1-based line) into a `TextEditor`
/// `Coordinates` (0-based line).
donner::editor::Coordinates FileOffsetToEditorCoordinates(const donner::FileOffset& offset) {
  if (!offset.lineInfo.has_value()) {
    return donner::editor::Coordinates(0, 0);
  }
  return donner::editor::Coordinates(static_cast<int>(offset.lineInfo->line) - 1,
                                     static_cast<int>(offset.lineInfo->offsetOnLine));
}

/// Render a small inspector line that describes the currently-selected
/// element (tag, id, world bounds, local transform). No-op if nothing
/// is selected. All reads go through public SVGElement APIs so the
/// editor doesn't touch the ECS registry directly.
void RenderInspector(const donner::editor::EditorApp& app) {
  if (!app.hasSelection()) {
    ImGui::TextDisabled("Nothing selected. Click an element to inspect.");
    return;
  }

  const donner::svg::SVGElement& selected = *app.selectedElement();

  // Tag name + id via the public `SVGElement` API. `RcString` converts
  // to `std::string_view` but isn't null-terminated, so use the `%.*s`
  // (length + data) form for ImGui::Text.
  const std::string_view tagSv = selected.tagName().name;
  const donner::RcString idStr = selected.id();
  const std::string_view idSv = idStr;
  if (!idSv.empty()) {
    ImGui::Text("Selected: <%.*s id=\"%.*s\">", static_cast<int>(tagSv.size()), tagSv.data(),
                static_cast<int>(idSv.size()), idSv.data());
  } else {
    ImGui::Text("Selected: <%.*s>", static_cast<int>(tagSv.size()), tagSv.data());
  }

  // World-space bounds via `SVGGeometryElement::worldBounds()` (the
  // same public API OverlayRenderer uses for the chrome rect).
  if (selected.isa<donner::svg::SVGGeometryElement>()) {
    if (auto bounds = selected.cast<donner::svg::SVGGeometryElement>().worldBounds();
        bounds.has_value()) {
      ImGui::Text("Bounds: (%.1f, %.1f) %.1f × %.1f", bounds->topLeft.x, bounds->topLeft.y,
                  bounds->width(), bounds->height());
    }
  }

  // Local (parent-from-entity) transform via the public `SVGGraphicsElement`
  // API — the same quantity SelectTool drags and UndoTimeline snapshots.
  if (selected.isa<donner::svg::SVGGraphicsElement>()) {
    const donner::Transform2d xform =
        selected.cast<donner::svg::SVGGraphicsElement>().transform();
    ImGui::Text("Transform: [%.3f %.3f %.3f %.3f  %.2f %.2f]", xform.data[0], xform.data[1],
                xform.data[2], xform.data[3], xform.data[4], xform.data[5]);
  }
}

/// Highlight an element's XML source span in the text editor. No-op if
/// the element doesn't carry XML source offsets.
void HighlightElementSource(donner::editor::TextEditor& textEditor,
                            const donner::svg::SVGElement& element) {
  // `XMLNode::TryCast` is a public donner base API that wraps the same
  // handle as the SVGElement and exposes XML source-location metadata.
  // It still takes an EntityHandle, but since we're calling it with a
  // handle that already came from a public SVG-side API we're not
  // reaching into the ECS from the editor.
  auto xmlNode = donner::xml::XMLNode::TryCast(element.entityHandle());
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

std::string LoadFile(const std::string& filename) {
  std::ifstream file(filename, std::ios::binary);
  if (!file) {
    std::cerr << "Could not open file " << filename << "\n";
    return {};
  }
  std::ostringstream out;
  out << file.rdbuf();
  return std::move(out).str();
}

}  // namespace

int main(int argc, char** argv) {
  if (const char* bwd = std::getenv("BUILD_WORKING_DIRECTORY")) {
    std::filesystem::current_path(bwd);
  }

  if (argc != 2) {
    std::cerr << "Usage: donner-editor <filename>\n";
    return 1;
  }

  const std::string svgPath = argv[1];
  const std::string initialSource = LoadFile(svgPath);
  if (initialSource.empty()) {
    return 1;
  }

  // ---------------------------------------------------------------------------
  // GLFW + OpenGL
  // ---------------------------------------------------------------------------
  glfwSetErrorCallback(GlfwErrorCallback);
  if (!glfwInit()) {
    std::cerr << "Failed to initialize GLFW\n";
    return 1;
  }

  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);

  GLFWwindow* window = glfwCreateWindow(kInitialWindowWidth, kInitialWindowHeight,
                                        "Donner Editor", nullptr, nullptr);
  if (!window) {
    std::cerr << "Failed to create GLFW window\n";
    glfwTerminate();
    return 1;
  }
  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);

  if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
    std::cerr << "Failed to initialize OpenGL loader\n";
    glfwDestroyWindow(window);
    glfwTerminate();
    return 1;
  }

  // ---------------------------------------------------------------------------
  // ImGui
  // ---------------------------------------------------------------------------
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  // imgui.ini persistence is disabled per the editor security policy in
  // docs/design_docs/editor.md.
  io.IniFilename = nullptr;
  ImGui::StyleColorsDark();
  ImGui_ImplGlfw_InitForOpenGL(window, true);
  ImGui_ImplOpenGL3_Init("#version 330");

  // ---------------------------------------------------------------------------
  // Editor state
  // ---------------------------------------------------------------------------
  donner::editor::EditorApp app;
  if (!app.loadFromString(initialSource)) {
    std::cerr << "Failed to parse SVG file: " << svgPath << "\n";
    // Keep running with an empty document so the user can still fix it
    // in the source pane.
  }

  donner::editor::SelectTool selectTool;

  donner::editor::TextEditor textEditor;
  textEditor.setLanguageDefinition(donner::editor::TextEditor::LanguageDefinition::SVG());
  textEditor.setText(initialSource);

  donner::svg::Renderer renderer;
  GLuint texture = 0;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  std::uint64_t lastRenderedVersion = 0;
  std::optional<donner::svg::SVGElement> lastRenderedSelection;
  std::optional<donner::svg::SVGElement> lastHighlightedSelection;
  donner::Vector2i lastRenderedCanvasSize{0, 0};
  int textureWidth = 0;
  int textureHeight = 0;

  // Track the source pane's parse error so we only push markers into
  // `TextEditor` when the diagnostic state actually changes (error
  // appeared, error cleared, or error moved to a different line).
  // `kNoErrorLine` is the sentinel for "no current error".
  constexpr int kNoErrorLine = -1;
  int lastShownErrorLine = kNoErrorLine;
  std::string lastShownErrorReason;
  // Surface the initial parse failure (if any) so the user sees the
  // marker on the very first frame.
  if (auto initialErr = app.document().lastParseError(); initialErr.has_value()) {
    textEditor.setErrorMarkers(ParseErrorToMarkers(*initialErr));
    lastShownErrorLine =
        initialErr->range.start.lineInfo.has_value()
            ? static_cast<int>(initialErr->range.start.lineInfo->line)
            : 1;
    lastShownErrorReason = std::string(std::string_view(initialErr->reason));
  }

  ViewTransform view;
  bool panning = false;
  ImVec2 lastPanMouse(0.0f, 0.0f);
  FrameHistory frameHistory;

  // ---------------------------------------------------------------------------
  // Main loop
  // ---------------------------------------------------------------------------
  while (!glfwWindowShouldClose(window)) {
    ZoneScopedN("main_loop");

    // Capture the previous frame's duration before any other ImGui
    // bookkeeping; this is the wall-clock time the user actually waited.
    frameHistory.push(ImGui::GetIO().DeltaTime * 1000.0f);

    glfwPollEvents();

    // Drain the command queue (ReplaceDocument from text edits,
    // SetTransform from SelectTool drags).
    {
      ZoneScopedN("flushFrame");
      app.flushFrame();
    }

    // Sync the source pane's error markers with the document's most
    // recent parse diagnostic. The diagnostic is set by ReplaceDocument
    // when re-parsing fails (typed-mid-edit syntax errors). We diff
    // against the previous frame to avoid copying the marker map every
    // frame.
    {
      const auto& parseError = app.document().lastParseError();
      if (parseError.has_value()) {
        const int line = parseError->range.start.lineInfo.has_value()
                             ? static_cast<int>(parseError->range.start.lineInfo->line)
                             : 1;
        const std::string_view reasonSv = parseError->reason;
        if (line != lastShownErrorLine || reasonSv != lastShownErrorReason) {
          textEditor.setErrorMarkers(ParseErrorToMarkers(*parseError));
          lastShownErrorLine = line;
          lastShownErrorReason.assign(reasonSv);
        }
      } else if (lastShownErrorLine != kNoErrorLine) {
        textEditor.setErrorMarkers({});
        lastShownErrorLine = kNoErrorLine;
        lastShownErrorReason.clear();
      }
    }

    // Re-render whenever the document version changes, the selection
    // changes, OR the canvas size changed (the latter happens when
    // the render pane is resized — the main loop pokes the document
    // below before this check).
    const auto currentVersion = app.document().currentFrameVersion();
    const auto& currentSelection = app.selectedElement();
    const bool selectionChanged = currentSelection != lastRenderedSelection;
    const donner::Vector2i currentCanvasSize =
        app.hasDocument() ? app.document().document().canvasSize() : donner::Vector2i{0, 0};
    if ((currentVersion != lastRenderedVersion || selectionChanged ||
         currentCanvasSize != lastRenderedCanvasSize) &&
        app.hasDocument()) {
      ZoneScopedN("render");
      {
        ZoneScopedN("renderer.draw");
        renderer.draw(app.document().document());
      }
      {
        ZoneScopedN("overlay");
        donner::editor::OverlayRenderer::drawChrome(renderer, app);
      }
      const donner::svg::RendererBitmap bitmap = [&] {
        ZoneScopedN("takeSnapshot");
        return renderer.takeSnapshot();
      }();
      if (!bitmap.empty()) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(bitmap.rowBytes / 4u));
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, bitmap.dimensions.x, bitmap.dimensions.y, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, bitmap.pixels.data());
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        textureWidth = bitmap.dimensions.x;
        textureHeight = bitmap.dimensions.y;
      }
      lastRenderedVersion = currentVersion;
      lastRenderedSelection = currentSelection;
      lastRenderedCanvasSize = currentCanvasSize;
      (void)selectionChanged;
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    // Main menu bar. Kept deliberately minimal — Help → About is the
    // only required entry for this milestone, but the bar exists so
    // future menus (File, Edit, Window, etc.) can slot in without
    // restructuring.
    bool openAboutPopup = false;
    if (ImGui::BeginMainMenuBar()) {
      if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("About Donner Editor")) {
          openAboutPopup = true;
        }
        ImGui::EndMenu();
      }
      ImGui::EndMainMenuBar();
    }
    if (openAboutPopup) {
      ImGui::OpenPopup("About Donner Editor");
    }

    // About popup — displays the embedded open-source notice. Required
    // for every distributed build to satisfy the attribution obligations
    // of the imgui, glfw, tracy, skia, freetype, and harfbuzz licenses.
    // Text comes from `//third_party/licenses:notice_editor` embedded
    // via `//donner/editor:notice`.
    {
      const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
      ImGui::SetNextWindowSize(ImVec2(displaySize.x * 0.7f, displaySize.y * 0.8f),
                               ImGuiCond_Appearing);
      ImGui::SetNextWindowPos(ImVec2(displaySize.x * 0.5f, displaySize.y * 0.5f),
                              ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
      if (ImGui::BeginPopupModal("About Donner Editor", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextUnformatted("Donner Editor — in-tree SVG editor built on donner/svg");
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextUnformatted("Open-source license notices:");
        ImGui::Spacing();
        if (ImGui::BeginChild("##notice_scroll",
                              ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing()),
                              /*border=*/true)) {
          const auto& notice = donner::embedded::kEditorNoticeText;
          ImGui::TextUnformatted(reinterpret_cast<const char*>(notice.data()),
                                 reinterpret_cast<const char*>(notice.data() + notice.size()));
        }
        ImGui::EndChild();
        if (ImGui::Button("Close")) {
          ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
      }
    }

    // Global keyboard shortcuts. ImGui translates Cmd on macOS and Ctrl
    // elsewhere into `Super`/`Ctrl` key mods, so we accept either.
    {
      const bool cmd = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper;
      const bool shift = ImGui::GetIO().KeyShift;
      const bool pressedZ = ImGui::IsKeyPressed(ImGuiKey_Z, /*repeat=*/false);

      if (pressedZ && cmd && !shift) {
        // Cmd+Z: undo
        if (app.canUndo()) {
          app.undo();
        }
      } else if (pressedZ && cmd && shift) {
        // Cmd+Shift+Z: redo (break chain + undo-of-undo)
        app.redo();
      }

      // Escape: clear the current selection. Modal popups capture Escape
      // first (to close themselves), so this only fires when no popup is
      // active.
      if (!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup) &&
          ImGui::IsKeyPressed(ImGuiKey_Escape, /*repeat=*/false) && app.hasSelection()) {
        app.setSelection(std::nullopt);
      }

      // Delete / Backspace: detach the selected element from the tree
      // via a `DeleteElement` command. The command flows through the
      // queue like any other mutation and the actual detach happens on
      // the next `flushFrame`. Undo for this action is not supported
      // (see `EditorCommand::Kind::DeleteElement`).
      const bool deleteKey = ImGui::IsKeyPressed(ImGuiKey_Delete, /*repeat=*/false) ||
                             ImGui::IsKeyPressed(ImGuiKey_Backspace, /*repeat=*/false);
      if (deleteKey && app.hasSelection() &&
          !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup) &&
          !ImGui::GetIO().WantCaptureKeyboard) {
        const donner::svg::SVGElement element = *app.selectedElement();
        app.setSelection(std::nullopt);
        app.applyMutation(donner::editor::EditorCommand::DeleteElementCommand(element));
      }
    }

    int windowWidth = 0;
    int windowHeight = 0;
    glfwGetWindowSize(window, &windowWidth, &windowHeight);

    // Windows are locked in place: positions and sizes are re-asserted
    // every frame with `ImGuiCond_Always`, and the NoMove / NoResize /
    // NoCollapse flags prevent any in-frame attempts to drag them
    // around (otherwise a click-and-drag on the image surface falls
    // through to the parent window and the user ends up moving the
    // pane instead of the editor target).
    const ImGuiWindowFlags kPaneFlags =
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;

    // Source pane.
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(kSourcePaneWidth, static_cast<float>(windowHeight)),
                             ImGuiCond_Always);
    ImGui::Begin("Source", nullptr, kPaneFlags);
    textEditor.render("##source");
    if (textEditor.isTextChanged()) {
      app.applyMutation(donner::editor::EditorCommand::ReplaceDocumentCommand(
          textEditor.getText()));
      // `isTextChanged` is sticky until explicitly reset — without this
      // the next frame would re-submit another ReplaceDocument command
      // and clobber any in-progress drag state.
      textEditor.resetTextChanged();
    }
    ImGui::End();

    // Render pane + SelectTool interaction.
    ImGui::SetNextWindowPos(ImVec2(kSourcePaneWidth, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(
        ImVec2(static_cast<float>(windowWidth) - kSourcePaneWidth, static_cast<float>(windowHeight)),
        ImGuiCond_Always);
    ImGui::Begin("Render", nullptr, kPaneFlags);

    // Inspector header — always-visible summary of what's selected.
    RenderInspector(app);
    ImGui::Text("Zoom: %.0f%%  Pan: (%.0f, %.0f)  (wheel=zoom, space+drag=pan, Cmd+0=reset)",
                view.zoom * 100.0f, view.pan.x, view.pan.y);
    RenderFrameGraph(frameHistory);
    ImGui::Separator();

    // Base canvas size = pane content region. Everything past this point
    // works in pane-local coordinates where (0,0) is the top-left of the
    // render area below the inspector header. Zoom scales the bitmap at
    // display time; pan offsets it.
    const ImVec2 contentRegion = ImGui::GetContentRegionAvail();
    const ImVec2 paneOrigin = ImGui::GetCursorScreenPos();
    const int baseW = static_cast<int>(contentRegion.x);
    const int baseH = static_cast<int>(contentRegion.y);

    if (app.hasDocument() && baseW > 0 && baseH > 0) {
      const donner::Vector2i currentSize = app.document().document().canvasSize();
      if (currentSize.x != baseW || currentSize.y != baseH) {
        app.document().document().setCanvasSize(baseW, baseH);
      }
    }

    // Reserve the full content area as an invisible button so ImGui
    // routes mouse events (wheel, drag, click) to this pane even when
    // the cursor is over empty space around the image.
    ImGui::InvisibleButton("##render_canvas", contentRegion,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    const bool paneHovered = ImGui::IsItemHovered();

    // Space+drag → pan. Middle-click+drag is a common alternative; both
    // work. `panning` is a one-frame edge-detected state so the first
    // frame of a drag captures the starting mouse position.
    const bool spaceHeld = ImGui::IsKeyDown(ImGuiKey_Space);
    const bool middleDown = ImGui::IsMouseDown(ImGuiMouseButton_Middle);
    if (paneHovered && (spaceHeld || middleDown) &&
        ImGui::IsMouseDown(ImGuiMouseButton_Left) && !panning) {
      panning = true;
      lastPanMouse = ImGui::GetMousePos();
    } else if (middleDown && !panning) {
      panning = true;
      lastPanMouse = ImGui::GetMousePos();
    }
    if (panning) {
      const ImVec2 now = ImGui::GetMousePos();
      view.pan.x += now.x - lastPanMouse.x;
      view.pan.y += now.y - lastPanMouse.y;
      lastPanMouse = now;
      if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
          !ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
        panning = false;
      }
    }

    // Mouse wheel → zoom around the cursor. To keep the point under the
    // cursor stationary across the zoom, compute the pre-zoom pane-local
    // coordinate of the cursor, change the zoom, then shift the pan so
    // the same bitmap pixel lands at the same pane-local coordinate.
    const float wheel = ImGui::GetIO().MouseWheel;
    if (paneHovered && wheel != 0.0f && !panning) {
      const ImVec2 mouse = ImGui::GetMousePos();
      const ImVec2 mousePaneLocal(mouse.x - paneOrigin.x, mouse.y - paneOrigin.y);
      const ImVec2 bitmapUnderCursor((mousePaneLocal.x - view.pan.x) / view.zoom,
                                     (mousePaneLocal.y - view.pan.y) / view.zoom);
      const float factor = wheel > 0.0f ? kZoomStep : 1.0f / kZoomStep;
      const float newZoom = std::clamp(view.zoom * factor, kMinZoom, kMaxZoom);
      view.pan.x = mousePaneLocal.x - bitmapUnderCursor.x * newZoom;
      view.pan.y = mousePaneLocal.y - bitmapUnderCursor.y * newZoom;
      view.zoom = newZoom;
    }

    // Keyboard shortcut for reset view.
    if (ImGui::IsKeyPressed(ImGuiKey_0) &&
        (ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper)) {
      view.reset();
    }

    if (textureWidth > 0 && textureHeight > 0) {
      const ImVec2 imageOrigin(paneOrigin.x + view.pan.x, paneOrigin.y + view.pan.y);
      const ImVec2 imageSize(static_cast<float>(textureWidth) * view.zoom,
                             static_cast<float>(textureHeight) * view.zoom);

      // Draw the image via the foreground draw list so it composites
      // into the pane without interfering with the InvisibleButton's
      // event consumption.
      ImGui::GetWindowDrawList()->AddImage(
          static_cast<ImTextureID>(static_cast<std::uintptr_t>(texture)), imageOrigin,
          ImVec2(imageOrigin.x + imageSize.x, imageOrigin.y + imageSize.y));

      // Screen → pane-local → canvas → document. The canvas→document
      // step is baked into the renderer's documentFromCanvasTransform
      // (it accounts for viewBox scaling), and the pane-local → canvas
      // step divides out the editor zoom.
      const donner::Transform2d documentFromCanvas =
          app.document().document().documentFromCanvasTransform();
      const auto screenToDocument = [&](const ImVec2& screenPoint) {
        const donner::Vector2d canvasPoint(
            (screenPoint.x - paneOrigin.x - view.pan.x) / view.zoom,
            (screenPoint.y - paneOrigin.y - view.pan.y) / view.zoom);
        return documentFromCanvas.transformPosition(canvasPoint);
      };

      // Only treat a click as a tool event when it's NOT a pan gesture.
      const bool toolEligible = paneHovered && !panning && !spaceHeld;

      if (toolEligible && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        selectTool.onMouseDown(app, screenToDocument(ImGui::GetMousePos()));
      }
      // Drag continues even after the cursor leaves the image.
      if (selectTool.isDragging()) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && !spaceHeld) {
          selectTool.onMouseMove(app, screenToDocument(ImGui::GetMousePos()),
                                 /*buttonHeld=*/true);
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
          selectTool.onMouseUp(app, screenToDocument(ImGui::GetMousePos()));
        }
      }

      // Whenever the selection changes, jump the source pane to the
      // selected element's XML span. Gated on actual selection change so
      // the highlight only fires on click, not every frame.
      const auto& selectionNow = app.selectedElement();
      if (selectionNow != lastHighlightedSelection) {
        if (selectionNow.has_value()) {
          HighlightElementSource(textEditor, *selectionNow);
        }
        lastHighlightedSelection = selectionNow;
      }
    } else {
      ImGui::TextUnformatted("(no rendered image)");
    }
    ImGui::End();

    ImGui::Render();
    int displayWidth = 0;
    int displayHeight = 0;
    glfwGetFramebufferSize(window, &displayWidth, &displayHeight);
    glViewport(0, 0, displayWidth, displayHeight);
    glClearColor(0.10f, 0.10f, 0.10f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    glfwSwapBuffers(window);
    FrameMark;
  }

  // ---------------------------------------------------------------------------
  // Teardown
  // ---------------------------------------------------------------------------
  glDeleteTextures(1, &texture);
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}
