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
/// bazel run //donner/editor -- --experimental donner_splash.svg
/// ```
///
/// For the minimal viewer demo (no tools, no overlay chrome, just a
/// rendered SVG and a text pane) see `//examples:svg_viewer`.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/FileOffset.h"
#include "donner/base/ParseDiagnostic.h"
#include "donner/base/Transform.h"
#include "donner/base/xml/XMLNode.h"
#include "donner/editor/AsyncRenderer.h"
#include "donner/editor/AttributeWriteback.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/EditorCommand.h"
#include "donner/editor/EditorIcon.h"
#include "donner/editor/EditorSplash.h"
#include "donner/editor/ExperimentalDragPresentation.h"
#include "donner/editor/Notice.h"
#include "donner/editor/OverlayRenderer.h"
#include "donner/editor/PinchEventMonitor.h"
#include "donner/editor/RenderPaneGesture.h"
#include "donner/editor/SelectTool.h"
#include "donner/editor/SelectionAabb.h"
#include "donner/editor/SourceSync.h"
#include "donner/editor/TextBuffer.h"
#include "donner/editor/TextEditor.h"
#include "donner/editor/TextPatch.h"
#include "donner/editor/TracyWrapper.h"
#include "donner/editor/ViewportState.h"
#include "donner/svg/SVGGeometryElement.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/SVGSVGElement.h"
#include "donner/svg/renderer/Renderer.h"
#include "embed_resources/FiraCodeFont.h"
#include "embed_resources/RobotoFont.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

#define GLFW_INCLUDE_ES3
#include <GLES3/gl3.h>

#include "GLFW/emscripten_glfw3.h"
#else
#include "glad/glad.h"
#endif

extern "C" {
#include "GLFW/glfw3.h"
}

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "imgui_internal.h"

namespace {

#ifndef __EMSCRIPTEN__
constexpr int kInitialWindowWidth = 1600;
constexpr int kInitialWindowHeight = 900;
#endif
constexpr float kInitialMainLeftDockFraction = 0.78f;
constexpr float kInitialSourceDockFraction = 0.45f;
constexpr float kInitialTreeDockFraction = 0.4f;
// Zoom min/max live on `ViewportState`; the per-step factors stay
// here because they're tied to user-input cadence rather than the
// transform model.
constexpr float kWheelZoomStep = 1.1f;     // Per Cmd+wheel notch.
constexpr float kKeyboardZoomStep = 1.5f;  // Per Cmd+Plus / Cmd+Minus / menu click.
// GLFW normalizes precise trackpad scroll deltas down to ~0.1 units on
// macOS. Multiplying back by 10 gives pan deltas in roughly screen-pixel
// scale while keeping coarse wheel notches modest.
constexpr double kTrackpadPanPixelsPerScrollUnit = 10.0;
constexpr float kTextChangeDebounceSeconds = 0.15f;  // 150ms idle before re-parse.
constexpr std::size_t kFrameHistoryCapacity = 120;   // 2s @ 60fps.
constexpr float kTargetFrameMs = 1000.0f / 60.0f;
constexpr float kFrameGraphWidth = 240.0f;
constexpr float kFrameGraphHeight = 32.0f;
constexpr ImU32 kSelectionChromeColor = IM_COL32(0x00, 0xc8, 0xff, 0xff);
constexpr float kSelectionChromeThickness = 1.5f;
constexpr ImU32 kMarqueeFillColor = IM_COL32(0x00, 0xc8, 0xff, 0x33);
constexpr ImU32 kMarqueeStrokeColor = IM_COL32(0xff, 0xff, 0xff, 0xff);
constexpr float kMarqueeStrokeThickness = 1.5f;
#ifdef __EMSCRIPTEN__
constexpr int kMaxWasmUploadBytes = 32 * 1024 * 1024;
#endif

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
    const std::size_t idx = (writeIndex + kFrameHistoryCapacity - 1) % kFrameHistoryCapacity;
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

/// Raw scroll events queued from GLFW until the next ImGui frame.
struct PendingScrollEvents {
  GLFWscrollfun previousCallback = nullptr;
  std::vector<donner::editor::RenderPaneScrollEvent> events;
};

[[nodiscard]] bool IsZoomModifierHeld(GLFWwindow* window) {
  return glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
         glfwGetKey(window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS ||
         glfwGetKey(window, GLFW_KEY_LEFT_SUPER) == GLFW_PRESS ||
         glfwGetKey(window, GLFW_KEY_RIGHT_SUPER) == GLFW_PRESS;
}

void EditorScrollCallback(GLFWwindow* window, double xoffset, double yoffset) {
  auto* state = static_cast<PendingScrollEvents*>(glfwGetWindowUserPointer(window));
  if (state == nullptr) {
    return;
  }

  if (state->previousCallback != nullptr) {
    state->previousCallback(window, xoffset, yoffset);
  }

  double cursorX = 0.0;
  double cursorY = 0.0;
  glfwGetCursorPos(window, &cursorX, &cursorY);
  state->events.push_back(donner::editor::RenderPaneScrollEvent{
      .scrollDelta = donner::Vector2d(xoffset, yoffset),
      .cursorScreen = donner::Vector2d(cursorX, cursorY),
      .zoomModifierHeld = IsZoomModifierHeld(window),
  });
}

/// Render a compact bar graph of the recent frame-time history plus a
/// smoothed ms / FPS readout below it. Each bar is one sample; red if
/// over the 16.67ms/60fps budget, green otherwise. A horizontal line
/// marks the target budget. Samples read left-to-right in chronological
/// order (oldest on the left).
void RenderFrameGraph(const FrameHistory& history) {
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  // Derive FPS from the smoothed ms so the two numbers are always
  // reciprocals. Smoothing each independently (arithmetic mean of
  // reciprocals vs. reciprocal of arithmetic mean) overshoots FPS when
  // frame times are bimodal — Jensen's inequality — and produces the
  // nonsensical "ms up AND fps up" readout during drag rerenders.
  const float latestMs = history.latest();
  static float msEma = 0.0f;
  static double lastDisplayUpdateTime = 0.0;
  static float displayedMs = 0.0f;

  msEma = msEma == 0.0f ? latestMs : 0.9f * msEma + 0.1f * latestMs;
  const double now = ImGui::GetTime();
  if (displayedMs == 0.0f || now - lastDisplayUpdateTime > 0.25) {
    displayedMs = msEma;
    lastDisplayUpdateTime = now;
  }
  const float displayedFps = displayedMs > 0.0f ? 1000.0f / displayedMs : 0.0f;

  // Background panel + budget line.
  const ImVec2 bottomRight(origin.x + kFrameGraphWidth, origin.y + kFrameGraphHeight);
  dl->AddRectFilled(origin, bottomRight, IM_COL32(30, 30, 30, 255));

  // Vertical scale: fixed at 2× the 60fps budget so the budget line
  // always sits at the 50% mark and ms-to-pixel ratio stays constant.
  // Bars taller than the graph just clip at the top — no autoscale,
  // since rescaling on every spike makes it impossible to compare
  // frame times across snapshots.
  constexpr float scaleMs = kTargetFrameMs * 2.0f;

  // Budget line — horizontal guide at kTargetFrameMs.
  const float budgetY =
      origin.y + kFrameGraphHeight - (kTargetFrameMs / scaleMs) * kFrameGraphHeight;
  dl->AddLine(ImVec2(origin.x, budgetY), ImVec2(bottomRight.x, budgetY),
              IM_COL32(255, 255, 255, 80), 1.0f);

  // Bars — oldest sample on the left. Each sample occupies
  // `kFrameGraphWidth / kFrameHistoryCapacity` pixels (2px at default
  // size), which is wide enough to eyeball individual frame spikes.
  const float barWidth = kFrameGraphWidth / static_cast<float>(kFrameHistoryCapacity);
  for (std::size_t i = 0; i < history.samples; ++i) {
    const std::size_t readIdx =
        (history.writeIndex + kFrameHistoryCapacity - history.samples + i) % kFrameHistoryCapacity;
    const float ms = history.deltaMs[readIdx];
    const float clamped = std::min(ms, scaleMs);
    const float barHeight = (clamped / scaleMs) * kFrameGraphHeight;
    const bool overBudget = ms > kTargetFrameMs;
    const ImU32 color = overBudget ? IM_COL32(220, 60, 60, 255) : IM_COL32(80, 200, 100, 255);

    const float x = origin.x + static_cast<float>(i) * barWidth;
    dl->AddRectFilled(ImVec2(x, origin.y + kFrameGraphHeight - barHeight),
                      ImVec2(x + barWidth, origin.y + kFrameGraphHeight), color);
  }

  // Advance the ImGui cursor past the graph so subsequent widgets lay out
  // to the right / below without overlapping.
  ImGui::Dummy(ImVec2(kFrameGraphWidth, kFrameGraphHeight));
  const ImU32 textColor =
      displayedMs > kTargetFrameMs ? IM_COL32(220, 60, 60, 255) : IM_COL32(255, 255, 255, 255);
  ImGui::PushStyleColor(ImGuiCol_Text, textColor);
  ImGui::Text("%.2f ms / %.1f FPS", displayedMs, displayedFps);
  ImGui::PopStyleColor();
}

void GlfwErrorCallback(int error, const char* description) {
  std::cerr << "GLFW error " << error << ": " << description << "\n";
}

// clang-format off
#ifdef __EMSCRIPTEN__
std::optional<std::string> gPendingBrowserUploadPath;

extern "C" void OnBrowserFileReadyPath(const char* path) {
  if (path != nullptr && path[0] != '\0') {
    gPendingBrowserUploadPath = std::string(path);
  }
}

EM_JS(int, canvasPixelWidth, (), {
  if (Module.canvas) {
    return Module.canvas.width;
  }
  return Math.max(1, Math.floor(window.innerWidth * (window.devicePixelRatio || 1)));
});

EM_JS(int, canvasPixelHeight, (), {
  if (Module.canvas) {
    return Module.canvas.height;
  }
  return Math.max(1, Math.floor(window.innerHeight * (window.devicePixelRatio || 1)));
});

EM_JS(int, canvasCssWidth, (), { return Math.max(1, Math.floor(window.innerWidth)); });

EM_JS(int, canvasCssHeight, (), { return Math.max(1, Math.floor(window.innerHeight)); });

EM_JS(double, browserDevicePixelRatio, (), { return window.devicePixelRatio || 1.0; });

double CurrentDisplayScale() {
  const int logicalWidth = canvasCssWidth();
  const int framebufferWidth = canvasPixelWidth();
  if (logicalWidth > 0 && framebufferWidth > 0) {
    return std::max(1.0, static_cast<double>(framebufferWidth) / static_cast<double>(logicalWidth));
  }
  return std::max(1.0, browserDevicePixelRatio());
}

EM_JS(void, ShowBrowserOpenFileDialog, (int maxBytes), {
  const input = document.createElement("input");
  input.type = "file";
  input.accept = "image/svg+xml,.svg,text/plain,.txt";
  input.onchange = () => {
    if (!input.files || input.files.length === 0) {
      input.remove();
      return;
    }

    const file = input.files[0];
    if (file.size > maxBytes) {
      alert(`File is too large. Limit is ${Math.floor(maxBytes / (1024 * 1024))} MiB.`);
      input.remove();
      return;
    }

    const reader = new FileReader();
    reader.onload = () => {
      FS.writeFile("/tmp/upload.svg", reader.result);
      ccall("OnBrowserFileReadyPath", null, ["string"], ["/tmp/upload.svg"]);
      input.remove();
    };
    reader.readAsText(file);
  };
  input.click();
});

EM_JS(void, InstallBrowserDropHandler, (int maxBytes), {
  const canvas = Module.canvas || document.getElementById("canvas");
  if (!canvas || canvas.__donnerDropInstalled) {
    return;
  }

  const rejectOversizedFile = file => {
    if (file.size > maxBytes) {
      alert(`File is too large. Limit is ${Math.floor(maxBytes / (1024 * 1024))} MiB.`);
      return true;
    }
    return false;
  };

  canvas.addEventListener("dragover", event => { event.preventDefault(); });

  canvas.addEventListener(
      "drop", event => {
        event.preventDefault();
        if (!event.dataTransfer || !event.dataTransfer.files ||
                event.dataTransfer.files.length === 0) {
          return;
        }

        const file = event.dataTransfer.files[0];
        if (rejectOversizedFile(file)) {
          return;
        }

        const reader = new FileReader();
        reader.onload = () => {
          FS.writeFile("/tmp/upload.svg", reader.result);
          ccall("OnBrowserFileReadyPath", null, ["string"], ["/tmp/upload.svg"]);
        };
        reader.readAsText(file);
      });

  canvas.__donnerDropInstalled = true;
});
#endif
// clang-format on

bool IsAncestorOrSelf(const donner::svg::SVGElement& ancestor,
                      const donner::svg::SVGElement& node) {
  for (std::optional<donner::svg::SVGElement> current = node; current.has_value();
       current = current->parentElement()) {
    if (*current == ancestor) {
      return true;
    }
  }
  return false;
}

bool IsSelectedInTree(std::span<const donner::svg::SVGElement> selection,
                      const donner::svg::SVGElement& element) {
  return std::find(selection.begin(), selection.end(), element) != selection.end();
}

donner::Vector2d DragPreviewScreenOffset(
    const std::optional<donner::editor::SelectTool::ActiveDragPreview>& dragPreview,
    const donner::editor::ViewportState& viewport) {
  if (!dragPreview.has_value()) {
    return donner::Vector2d::Zero();
  }

  return dragPreview->translation * viewport.pixelsPerDocUnit();
}

std::string BuildTreeNodeLabel(const donner::svg::SVGElement& element) {
  const std::string_view tagName = element.tagName().name;
  std::string label = "<";
  label.append(tagName.data(), tagName.size());
  label.push_back('>');

  const donner::RcString id = element.id();
  const std::string_view idSv = id;
  if (!idSv.empty()) {
    label.push_back(' ');
    label.push_back('#');
    label.append(idSv.data(), idSv.size());
  }

  return label;
}

struct TreeViewState {
  std::optional<donner::svg::SVGElement> scrollTarget;
  bool pendingScroll = false;
  bool selectionChangedInTree = false;
};

bool RenderTreeRecursive(donner::editor::EditorApp& app, const donner::svg::SVGElement& element,
                         TreeViewState& state) {
  const bool hasChildren = element.firstChild().has_value();
  const bool onSelectionPath = state.pendingScroll && state.scrollTarget.has_value() &&
                               IsAncestorOrSelf(element, *state.scrollTarget);
  if (hasChildren && onSelectionPath) {
    ImGui::SetNextItemOpen(true, ImGuiCond_Always);
  }

  ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                 ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                 ImGuiTreeNodeFlags_SpanAvailWidth;
  if (!hasChildren) {
    nodeFlags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
  }
  if (IsSelectedInTree(std::span<const donner::svg::SVGElement>(app.selectedElements()), element)) {
    nodeFlags |= ImGuiTreeNodeFlags_Selected;
  }

  const donner::Entity entity = element.entityHandle().entity();
  ImGui::PushID(static_cast<int>(static_cast<std::uint32_t>(entity)));
  const std::string label = BuildTreeNodeLabel(element);
  const bool nodeOpen = ImGui::TreeNodeEx(label.c_str(), nodeFlags);

  if (ImGui::IsItemClicked()) {
    const bool toggleSelection = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper;
    if (toggleSelection) {
      app.toggleInSelection(element);
    } else {
      app.setSelection(element);
    }
    state.selectionChangedInTree = true;
    state.pendingScroll = false;
  }

  if (state.pendingScroll && state.scrollTarget.has_value() && *state.scrollTarget == element) {
    ImGui::SetScrollHereY();
    state.pendingScroll = false;
  }

  if (nodeOpen && hasChildren) {
    for (auto child = element.firstChild(); child.has_value(); child = child->nextSibling()) {
      RenderTreeRecursive(app, *child, state);
    }
    ImGui::TreePop();
  }

  ImGui::PopID();
  return state.selectionChangedInTree;
}

void RenderTreeView(donner::editor::EditorApp& app, TreeViewState& state) {
  if (!app.hasDocument()) {
    ImGui::TextDisabled("(no document)");
    return;
  }

  RenderTreeRecursive(app, app.document().document().svgElement(), state);
}

/// Build a `TextEditor::ErrorMarkers` map from a parser diagnostic. The
/// map is keyed by 1-based line number (`TextEditor`'s convention) and
/// values are the human-readable reason. Diagnostics without resolved
/// line info land on line 1 so the user always sees *something*.
donner::editor::TextEditor::ErrorMarkers ParseErrorToMarkers(const donner::ParseDiagnostic& diag) {
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
    const donner::Transform2d xform = selected.cast<donner::svg::SVGGraphicsElement>().transform();
    ImGui::Text("Transform: [%.3f %.3f %.3f %.3f  %.2f %.2f]", xform.data[0], xform.data[1],
                xform.data[2], xform.data[3], xform.data[4], xform.data[5]);
  }
}

/// Best-effort document viewBox lookup for the editor's render pane.
/// Returns the SVG root element's `viewBox` attribute when present;
/// otherwise falls back to the document's intrinsic natural size as
/// computed by `canvasSize()` (which is what the renderer would have
/// rasterized into anyway). Final fallback for completely degenerate
/// documents is a unit box, which keeps `ViewportState` math finite
/// without crashing.
donner::Box2d ResolveDocumentViewBox(donner::svg::SVGDocument& document) {
  if (auto viewBox = document.svgElement().viewBox(); viewBox.has_value()) {
    return *viewBox;
  }
  const donner::Vector2i intrinsic = document.canvasSize();
  if (intrinsic.x > 0 && intrinsic.y > 0) {
    return donner::Box2d::FromXYWH(0.0, 0.0, static_cast<double>(intrinsic.x),
                                   static_cast<double>(intrinsic.y));
  }
  return donner::Box2d::FromXYWH(0.0, 0.0, 1.0, 1.0);
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

#ifndef __EMSCRIPTEN__
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
#endif

std::string EmbeddedBytesToString(std::span<const unsigned char> bytes) {
  std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  if (!text.empty() && text.back() == '\0') {
    text.pop_back();
  }
  return text;
}

}  // namespace

int main(int argc, char** argv) {
  if (const char* bwd = std::getenv("BUILD_WORKING_DIRECTORY")) {
    std::filesystem::current_path(bwd);
  }

  bool experimentalMode = false;

#ifdef __EMSCRIPTEN__
  const std::string initialSource = EmbeddedBytesToString(donner::embedded::kEditorIconSvg);
  const std::optional<std::string> initialPath = std::string("donner_icon.svg");
#else
  std::optional<std::string> svgPath;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--experimental") {
      experimentalMode = true;
      continue;
    }

    if (svgPath.has_value()) {
      std::cerr << "Usage: donner-editor [--experimental] <filename>\n";
      return 1;
    }

    svgPath = std::string(arg);
  }

  if (!svgPath.has_value()) {
    std::cerr << "Usage: donner-editor [--experimental] <filename>\n";
    return 1;
  }

  const std::string initialSource = LoadFile(*svgPath);
  if (initialSource.empty()) {
    return 1;
  }
  const std::optional<std::string> initialPath = svgPath;
#endif

  // ---------------------------------------------------------------------------
  // GLFW + OpenGL
  // ---------------------------------------------------------------------------
  glfwSetErrorCallback(GlfwErrorCallback);
  if (!glfwInit()) {
    std::cerr << "Failed to initialize GLFW\n";
    return 1;
  }

#ifdef __EMSCRIPTEN__
  glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_EMSCRIPTEN);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_SCALE_FRAMEBUFFER, GLFW_TRUE);
#else
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

#ifdef __EMSCRIPTEN__
  emscripten_glfw_set_next_window_canvas_selector("#canvas");
#endif
  const int initialWindowWidth =
#ifdef __EMSCRIPTEN__
      canvasPixelWidth();
#else
      kInitialWindowWidth;
#endif
  const int initialWindowHeight =
#ifdef __EMSCRIPTEN__
      canvasPixelHeight();
#else
      kInitialWindowHeight;
#endif
  GLFWwindow* window =
      glfwCreateWindow(initialWindowWidth, initialWindowHeight, "Donner Editor", nullptr, nullptr);
  if (!window) {
    std::cerr << "Failed to create GLFW window\n";
    glfwTerminate();
    return 1;
  }
  glfwMakeContextCurrent(window);
#ifdef __EMSCRIPTEN__
  emscripten_glfw_make_canvas_resizable(window, "window", nullptr);
#endif
#ifndef __EMSCRIPTEN__
  glfwSwapInterval(1);

  if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
    std::cerr << "Failed to initialize OpenGL loader\n";
    glfwDestroyWindow(window);
    glfwTerminate();
    return 1;
  }
#endif

  // ---------------------------------------------------------------------------
  // ImGui
  // ---------------------------------------------------------------------------
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  double initialDisplayScale = 1.0;
#ifdef __EMSCRIPTEN__
  initialDisplayScale = CurrentDisplayScale();
#else
  {
    int logicalWindowWidth = 0;
    int logicalWindowHeight = 0;
    glfwGetWindowSize(window, &logicalWindowWidth, &logicalWindowHeight);

    int framebufferWidth = 0;
    int framebufferHeight = 0;
    glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
    if (logicalWindowWidth > 0 && framebufferWidth > 0) {
      initialDisplayScale =
          static_cast<double>(framebufferWidth) / static_cast<double>(logicalWindowWidth);
    }
  }
#endif
  initialDisplayScale = std::max(1.0, initialDisplayScale);
  // Match the original jwmcglynn/donner-editor font combo: Roboto Regular
  // is the default UI font (menubar, pane labels, inspector, tree view);
  // Roboto Bold is available for emphasis; Fira Code is the monospace
  // font pushed around the source pane's TextEditor widget.
  //
  // The atlas doesn't own the TTF bytes because the `embed_resources`
  // blobs are compile-time constants that outlive the atlas.
  ImFontConfig fontCfg;
  fontCfg.FontDataOwnedByAtlas = false;
  ImFont* uiFontRegular = io.Fonts->AddFontFromMemoryTTF(
      const_cast<unsigned char*>(donner::embedded::kRobotoRegularTtf.data()),
      static_cast<int>(donner::embedded::kRobotoRegularTtf.size()),
      /*size_pixels=*/static_cast<float>(15.0 * initialDisplayScale), &fontCfg);
  ImFont* uiFontBold = io.Fonts->AddFontFromMemoryTTF(
      const_cast<unsigned char*>(donner::embedded::kRobotoBoldTtf.data()),
      static_cast<int>(donner::embedded::kRobotoBoldTtf.size()),
      /*size_pixels=*/static_cast<float>(15.0 * initialDisplayScale), &fontCfg);
  ImFont* codeFont = io.Fonts->AddFontFromMemoryTTF(
      const_cast<unsigned char*>(donner::embedded::kFiraCodeRegularTtf.data()),
      static_cast<int>(donner::embedded::kFiraCodeRegularTtf.size()),
      /*size_pixels=*/static_cast<float>(14.0 * initialDisplayScale), &fontCfg);
  (void)uiFontRegular;
  (void)uiFontBold;
  // imgui.ini persistence is disabled per the editor security policy in
  // docs/design_docs/editor.md.
  io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  io.DisplayFramebufferScale =
      ImVec2(static_cast<float>(initialDisplayScale), static_cast<float>(initialDisplayScale));
  io.FontGlobalScale = static_cast<float>(1.0 / initialDisplayScale);
  ImGui::StyleColorsDark();
  ImGui_ImplGlfw_InitForOpenGL(window, true);
#ifdef __EMSCRIPTEN__
  ImGui_ImplOpenGL3_Init("#version 300 es");
  ImGui_ImplGlfw_InstallEmscriptenCallbacks(window, "#canvas");
#else
  ImGui_ImplOpenGL3_Init("#version 330");
#endif

  // ImGui's GLFW backend already consumes scroll callbacks for
  // `io.MouseWheel`. Install a thin wrapper on top so we can retain
  // the raw per-event scroll sequence for render-pane gesture routing
  // while still forwarding every event back into ImGui unchanged.
  PendingScrollEvents pendingScrollEvents;
  glfwSetWindowUserPointer(window, &pendingScrollEvents);
  pendingScrollEvents.previousCallback = glfwSetScrollCallback(window, EditorScrollCallback);
  (void)donner::editor::InstallPinchEventMonitor(window, &pendingScrollEvents.events,
                                                 kWheelZoomStep);
#ifdef __EMSCRIPTEN__
  InstallBrowserDropHandler(kMaxWasmUploadBytes);
#endif

  // ---------------------------------------------------------------------------
  // Editor state
  // ---------------------------------------------------------------------------
  // Startup-timing instrumentation: print where the time is going on
  // first launch so we can chase the "took a few seconds for the SVG
  // to appear" report. Cheap to leave in until we have a proper
  // perf-profiling story.
  using SteadyClock = std::chrono::steady_clock;
  const auto editorStart = SteadyClock::now();
  const auto msSinceStart = [&editorStart]() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - editorStart)
        .count();
  };

  donner::editor::EditorApp app;
  if (!app.loadFromString(initialSource)) {
    std::cerr << "Failed to parse initial SVG";
    if (initialPath.has_value()) {
      std::cerr << ": " << *initialPath;
    }
    std::cerr << "\n";
    // Keep running with an empty document so the user can still fix it
    // in the source pane.
  }
  std::cerr << "[startup] +" << msSinceStart() << "ms loadFromString done\n";
  // Remember the startup path so the title bar can show the filename and
  // future save wiring can reuse it.
  if (initialPath.has_value()) {
    app.setCurrentFilePath(*initialPath);
  }

  donner::editor::SelectTool selectTool;
  selectTool.setCompositedDragPreviewEnabled(experimentalMode);

  donner::editor::TextEditor textEditor;
  textEditor.setLanguageDefinition(donner::editor::TextEditor::LanguageDefinition::SVG());
  textEditor.setText(initialSource);
  textEditor.resetTextChanged();
  app.setCleanSourceText(initialSource);
  // Enable autocomplete. Once active, typing letters auto-triggers the
  // suggestion popup; matches are drawn from the language definition's
  // keyword set (SVG element names) + identifiers (SVG/CSS attribute
  // names) + any entries added via `addAutocompleteEntry`. Ctrl+I can
  // also explicitly open the popup.
  textEditor.setActiveAutocomplete(true);
  std::string editorNoticeText = EmbeddedBytesToString(donner::embedded::kEditorNoticeText);

  // The document renderer is owned by `asyncRenderer` and lives on
  // its worker thread — backend resources (especially the WebGPU
  // device under Geode) must be created and used from a single
  // thread, so the UI thread intentionally does not hold a Renderer
  // for document rasterization. Overlay chrome still rasterizes
  // synchronously on the UI thread via `overlayRenderer` below.
  donner::editor::AsyncRenderer asyncRenderer;
  GLuint texture = 0;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  GLuint dragBackgroundTexture = 0;
  GLuint dragPromotedTexture = 0;
  GLuint dragForegroundTexture = 0;
  glGenTextures(1, &dragBackgroundTexture);
  glGenTextures(1, &dragPromotedTexture);
  glGenTextures(1, &dragForegroundTexture);
  for (const GLuint dragTexture : {dragBackgroundTexture, dragPromotedTexture, dragForegroundTexture}) {
    glBindTexture(GL_TEXTURE_2D, dragTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  }

  // Selection path outlines live in a *second* pixmap drawn by a
  // *second* renderer instance, layered on top of the document
  // texture at display time. The AABB and marquee moved to the ImGui
  // draw list so click / drag feedback shows up in the same frame
  // without waiting for a CPU re-rasterize.
  donner::svg::Renderer overlayRenderer;
  GLuint overlayTexture = 0;
  glGenTextures(1, &overlayTexture);
  glBindTexture(GL_TEXTURE_2D, overlayTexture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  int overlayTextureWidth = 0;
  int overlayTextureHeight = 0;
  std::optional<donner::svg::RendererBitmap> pendingOverlayBitmap;
  std::uint64_t pendingOverlayVersion = 0;
  std::uint64_t displayedDocVersion = 0;
  // Tracks the selection vector (multi-select aware) snapshotted at
  // the last successful overlay re-render. Stored as a value so the
  // change-detection compare is straightforward; the contents are
  // shallow `SVGElement` handles, so the copy cost is in the noise.
  std::vector<donner::svg::SVGElement> lastOverlaySelectionVec;
  donner::Vector2i lastOverlayCanvasSize{0, 0};
  // Sentinel: forces the first frame to always re-rasterize the
  // overlay so it lines up with the freshly-rendered document
  // bitmap. After the first frame, the version-change trigger keeps
  // it in sync with `flushFrame` mutations; the upload itself is
  // deferred until the matching document bitmap lands.
  std::uint64_t lastOverlayVersion = std::numeric_limits<std::uint64_t>::max();
  // Cached document-space selection bounds used by the immediate-mode
  // AABB draw. Refreshed eagerly on selection-changing clicks and on
  // any later document-version change that could move the selected
  // geometry.
  donner::editor::SelectionBoundsCache selectionBoundsCache;

  std::uint64_t lastRenderedVersion = 0;
  std::optional<donner::svg::SVGElement> lastHighlightedSelection;
  donner::Vector2i lastRenderedCanvasSize{0, 0};
  donner::editor::ExperimentalDragPresentation experimentalDragPresentation;
  int dragBackgroundTextureWidth = 0;
  int dragBackgroundTextureHeight = 0;
  int dragPromotedTextureWidth = 0;
  int dragPromotedTextureHeight = 0;
  int dragForegroundTextureWidth = 0;
  int dragForegroundTextureHeight = 0;
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
    lastShownErrorLine = initialErr->range.start.lineInfo.has_value()
                             ? static_cast<int>(initialErr->range.start.lineInfo->line)
                             : 1;
    lastShownErrorReason = std::string(std::string_view(initialErr->reason));
  }

  // Single source of truth for the render pane's coordinate system.
  // See `docs/design_docs/0025-editor_ux.md`. Inputs (paneOrigin,
  // paneSize, viewBox, DPR) are refreshed at the top of every frame
  // from the live ImGui / GLFW state; user input only ever mutates
  // `zoom`, `panDocPoint`, `panScreenPoint` via the helpers on the
  // struct. Default-constructed → `zoom = 1.0` (= 100%).
  donner::editor::ViewportState viewport;
  // Set true after the first frame in which we know a non-zero pane
  // size, so we can call `resetTo100Percent()` once and anchor the
  // initial document center to the pane center.
  bool viewportInitialized = false;

  bool panning = false;
  ImVec2 lastPanMouse(0.0f, 0.0f);
  FrameHistory frameHistory;

  // A click captured by ImGui that hasn't yet been processed by
  // `SelectTool` because the async render worker was busy at the
  // time. Drained on the next frame the worker is idle. Without this
  // buffer, clicks arriving during the initial render (which can
  // take a few hundred ms on a complex SVG) are silently dropped and
  // the user sees their mouse-downs go nowhere.
  struct PendingClick {
    donner::Vector2d documentPoint;
    donner::editor::MouseModifiers modifiers;
  };
  std::optional<PendingClick> pendingClick;

  // Previous frame's source text — used by the M5 change classifier to
  // diff against the current text and determine whether the edit lands
  // inside a single attribute value.
  std::string previousSourceText = initialSource;
  std::optional<std::string> lastWritebackSourceText;
  std::optional<donner::editor::EditorApp::CompletedTransformWriteback> pendingTransformWriteback;
  std::vector<donner::editor::EditorApp::CompletedElementRemoveWriteback>
      pendingElementRemoveWritebacks;

  // Cached window title pieces so we only call glfwSetWindowTitle when
  // something actually changes.
  std::string lastWindowTitle;

  // Leading-edge + trailing debounce state for text changes.
  //
  // Every keystroke would otherwise trigger the full parse→CSS→
  // render pipeline; on a complex SVG that's ~100ms+ of CPU per
  // press even in opt mode, which overwhelms the typing rate. We
  // still need to debounce, but the *pure* trailing debounce that
  // lived here before felt broken: the user would delete a character,
  // wait, see nothing, press another key, and only then watch the
  // previous change land. The effect was "every edit takes one extra
  // key press to take effect", even though the 150 ms trailing wait
  // was the actual cause.
  //
  // Leading-edge fixes that: fire immediately on the first
  // keystroke after an idle period, then throttle subsequent edits
  // until the timer expires, and emit one final trailing dispatch
  // at idle so in-flight mid-burst edits aren't lost. The result is
  // that the canvas updates on the very first frame after a
  // keystroke when the user has been idle, and the trailing
  // dispatch cleans up any changes that were throttled out of the
  // middle of a typing burst.
  bool textChangePending = false;
  bool textDispatchThrottled = false;
  float textChangeIdleTimer = 0.0f;

  // Enable the M5 incremental path by default during development. The
  // design doc says default-off until the fuzzing soak (M8), but during
  // active use it should be on so attribute-value edits skip the full
  // re-parse. The flag can still be toggled at runtime for debugging.
  app.setStructuredEditingEnabled(true);

  std::cerr << "[startup] +" << msSinceStart() << "ms entering main loop\n";

  // One-shot guards so the startup-timing prints fire exactly once
  // each, on the frames we care about. Cheap to leave in.
  bool loggedFirstRenderRequest = false;
  bool loggedFirstTextureLanded = false;
  bool dockspaceInitialized = false;
  std::optional<donner::svg::SVGElement> lastTreeSelection;
  bool treeviewPendingScroll = false;
  bool treeSelectionOriginatedInTree = false;
  bool openFileModalRequested = false;
  std::array<char, 4096> openFilePathBuffer{};
  std::string openFileError;

  const auto promotePendingSelectionBoundsIfReady = [&]() {
    donner::editor::PromoteSelectionBoundsIfReady(selectionBoundsCache, displayedDocVersion);
  };

  const auto refreshPendingSelectionBoundsCache = [&]() {
    if (!app.hasDocument()) {
      selectionBoundsCache = donner::editor::SelectionBoundsCache{};
      return;
    }

    donner::editor::RefreshSelectionBoundsCache(
        selectionBoundsCache, std::span<const donner::svg::SVGElement>(app.selectedElements()),
        app.document().currentFrameVersion(), displayedDocVersion);
  };

  const auto selectedExperimentalEntity = [&]() -> donner::Entity {
    if (!experimentalMode || !app.selectedElement().has_value()) {
      return entt::null;
    }

    const auto& selected = *app.selectedElement();
    if (!selected.isa<donner::svg::SVGGraphicsElement>()) {
      return entt::null;
    }

    return selected.entityHandle().entity();
  };

  // Rasterize the Skia path-outline overlay for the current selection
  // and upload it to `overlayTexture`. Must be called only when the
  // worker thread is idle (caller's responsibility). When the current
  // document version matches the displayed doc version (the click /
  // selection-only fast path) the upload happens immediately so the
  // overlay texture and the AABB promote together in the same frame.
  // When they differ (drag / doc-mutation path) the bitmap is stashed
  // in `pendingOverlayBitmap` and uploaded lock-step in `pollResult`
  // when the matching doc bitmap lands.
  //
  // Returns true if a rasterize happened, false if nothing to redraw.
  const auto rasterizeOverlayForCurrentSelection = [&]() -> bool {
    if (!app.hasDocument()) {
      return false;
    }
    const donner::Vector2i currentCanvasSize = app.document().document().canvasSize();
    const auto currentVersion = app.document().currentFrameVersion();

    donner::svg::RenderViewport overlayViewport;
    overlayViewport.size =
        donner::Vector2d(static_cast<double>(currentCanvasSize.x) / viewport.devicePixelRatio,
                         static_cast<double>(currentCanvasSize.y) / viewport.devicePixelRatio);
    overlayViewport.devicePixelRatio = viewport.devicePixelRatio;
    overlayRenderer.beginFrame(overlayViewport);
    const donner::Transform2d canvasFromDoc =
        app.document().document().canvasFromDocumentTransform();
    const auto& overlaySelection = app.selectedElements();
    donner::editor::OverlayRenderer::drawChromeWithTransform(
        overlayRenderer, std::span<const donner::svg::SVGElement>(overlaySelection), canvasFromDoc);
    overlayRenderer.endFrame();
    donner::svg::RendererBitmap overlayBitmap = overlayRenderer.takeSnapshot();
    pendingOverlayBitmap = std::move(overlayBitmap);
    pendingOverlayVersion = currentVersion;
    if (currentVersion == displayedDocVersion && pendingOverlayBitmap.has_value() &&
        !pendingOverlayBitmap->empty()) {
      const auto& readyOverlayBitmap = *pendingOverlayBitmap;
      glBindTexture(GL_TEXTURE_2D, overlayTexture);
      glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(readyOverlayBitmap.rowBytes / 4u));
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, readyOverlayBitmap.dimensions.x,
                   readyOverlayBitmap.dimensions.y, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                   readyOverlayBitmap.pixels.data());
      glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
      overlayTextureWidth = readyOverlayBitmap.dimensions.x;
      overlayTextureHeight = readyOverlayBitmap.dimensions.y;
      pendingOverlayBitmap.reset();
      pendingOverlayVersion = 0;
    } else if (currentVersion == displayedDocVersion) {
      overlayTextureWidth = 0;
      overlayTextureHeight = 0;
      pendingOverlayBitmap.reset();
      pendingOverlayVersion = 0;
    }
    lastOverlaySelectionVec = overlaySelection;
    lastOverlayCanvasSize = currentCanvasSize;
    lastOverlayVersion = currentVersion;
    return true;
  };

  const auto applyPendingTransformWriteback = [&]() {
    // Drain SelectTool first (drag completion), then EditorApp (undo /
    // redo). Either source overwrites a previously-pending writeback
    // because the latest transform value is always the one we want.
    if (auto completed = selectTool.consumeCompletedDragWriteback(); completed.has_value()) {
      pendingTransformWriteback = donner::editor::EditorApp::CompletedTransformWriteback{
          .target = std::move(completed->target),
          .transform = completed->transform,
      };
    }
    if (auto completed = app.consumeTransformWriteback(); completed.has_value()) {
      pendingTransformWriteback = std::move(*completed);
    }

    if (!pendingTransformWriteback.has_value()) {
      return;
    }

    std::string source = textEditor.getText();
    const donner::RcString serialized =
        donner::toSVGTransformString(pendingTransformWriteback->transform);
    if (std::string_view(serialized).empty()) {
      auto patch = donner::editor::buildAttributeRemoveWriteback(
          source, pendingTransformWriteback->target, "transform");
      pendingTransformWriteback.reset();
      if (!patch.has_value()) {
        return;
      }

      donner::editor::applyPatches(source, {{*patch}});
      textEditor.setText(source, /*preserveScroll=*/true);
      donner::editor::QueueSourceWritebackReparse(app, source, &previousSourceText,
                                                  &lastWritebackSourceText);
      app.syncDirtyFromSource(source);
      return;
    }
    auto patch = donner::editor::buildAttributeWriteback(source, pendingTransformWriteback->target,
                                                         "transform", std::string_view(serialized));
    if (!patch.has_value()) {
      return;
    }

    donner::editor::applyPatches(source, {{*patch}});
    textEditor.setText(source, /*preserveScroll=*/true);
    donner::editor::QueueSourceWritebackReparse(app, source, &previousSourceText,
                                                &lastWritebackSourceText);
    app.syncDirtyFromSource(source);
    pendingTransformWriteback.reset();
  };

  const auto applyPendingElementRemoveWritebacks = [&]() {
    auto completed = app.consumeElementRemoveWritebacks();
    for (auto& writeback : completed) {
      pendingElementRemoveWritebacks.push_back(std::move(writeback));
    }
    if (pendingElementRemoveWritebacks.empty()) {
      return;
    }

    std::string source = textEditor.getText();
    bool changed = false;
    for (const auto& pendingRemove : pendingElementRemoveWritebacks) {
      auto patch = donner::editor::buildElementRemoveWriteback(source, pendingRemove.target);
      if (!patch.has_value()) {
        continue;
      }

      donner::editor::applyPatches(source, {{*patch}});
      changed = true;
    }

    pendingElementRemoveWritebacks.clear();
    if (!changed) {
      return;
    }

    textEditor.setText(source, /*preserveScroll=*/true);
    donner::editor::QueueSourceWritebackReparse(app, source, &previousSourceText,
                                                &lastWritebackSourceText);
    app.syncDirtyFromSource(source);
  };

  // ---------------------------------------------------------------------------
  // Main loop
  // ---------------------------------------------------------------------------
  while (!glfwWindowShouldClose(window)) {
    ZoneScopedN("main_loop");

    // Capture the previous frame's duration before any other ImGui
    // bookkeeping; this is the wall-clock time the user actually waited.
    frameHistory.push(ImGui::GetIO().DeltaTime * 1000.0f);

    glfwPollEvents();

    // Update the window title with the filename + dirty indicator (`●`).
    // Diffed against the previous frame so glfwSetWindowTitle is only
    // called on transitions.
    {
      std::string title = "";
      if (app.isDirty()) {
        title += "● ";
      }
      if (app.currentFilePath().has_value()) {
        title += std::filesystem::path(*app.currentFilePath()).filename().string();
      } else {
        title += "untitled";
      }
      title += "— Donner SVG Editor";
      if (title != lastWindowTitle) {
        glfwSetWindowTitle(window, title.c_str());
        lastWindowTitle = std::move(title);
      }
    }

    // First, check if a prior async render has completed and pick up
    // the bitmap if so.
    if (auto resultOpt = asyncRenderer.pollResult(); resultOpt.has_value()) {
      const auto& result = *resultOpt;
      const auto uploadBitmapToTexture = [](GLuint targetTexture, const donner::svg::RendererBitmap& bitmap,
                                            int* outWidth, int* outHeight) {
        if (bitmap.empty()) {
          *outWidth = 0;
          *outHeight = 0;
          return;
        }

        glBindTexture(GL_TEXTURE_2D, targetTexture);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(bitmap.rowBytes / 4u));
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, bitmap.dimensions.x, bitmap.dimensions.y, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, bitmap.pixels.data());
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        *outWidth = bitmap.dimensions.x;
        *outHeight = bitmap.dimensions.y;
      };

      if (result.compositedPreview.has_value() && result.compositedPreview->valid()) {
        uploadBitmapToTexture(dragBackgroundTexture, result.compositedPreview->backgroundBitmap,
                              &dragBackgroundTextureWidth, &dragBackgroundTextureHeight);
        uploadBitmapToTexture(dragPromotedTexture, result.compositedPreview->promotedBitmap,
                              &dragPromotedTextureWidth, &dragPromotedTextureHeight);
        uploadBitmapToTexture(dragForegroundTexture, result.compositedPreview->foregroundBitmap,
                              &dragForegroundTextureWidth, &dragForegroundTextureHeight);
        experimentalDragPresentation.noteCachedTextures(
            result.compositedPreview->entity, result.version,
            donner::Vector2i(result.compositedPreview->promotedBitmap.dimensions.x,
                             result.compositedPreview->promotedBitmap.dimensions.y));
      } else if (!result.bitmap.empty()) {
        const auto& bitmap = result.bitmap;
        uploadBitmapToTexture(texture, bitmap, &textureWidth, &textureHeight);
        experimentalDragPresentation.noteFullRenderLanded(result.version);
      }

      displayedDocVersion = result.version;
      if (pendingOverlayBitmap.has_value() && pendingOverlayVersion == displayedDocVersion) {
        const auto& overlayBitmap = *pendingOverlayBitmap;
        if (!overlayBitmap.empty()) {
          glBindTexture(GL_TEXTURE_2D, overlayTexture);
          glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(overlayBitmap.rowBytes / 4u));
          glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, overlayBitmap.dimensions.x,
                       overlayBitmap.dimensions.y, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                       overlayBitmap.pixels.data());
          glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
          overlayTextureWidth = overlayBitmap.dimensions.x;
          overlayTextureHeight = overlayBitmap.dimensions.y;
        } else {
          overlayTextureWidth = 0;
          overlayTextureHeight = 0;
        }
        pendingOverlayBitmap.reset();
        pendingOverlayVersion = 0;
      }
      promotePendingSelectionBoundsIfReady();
      if (!loggedFirstTextureLanded) {
        loggedFirstTextureLanded = true;
        if (result.compositedPreview.has_value() && result.compositedPreview->valid()) {
          std::cerr << "[startup] +" << msSinceStart()
                    << "ms first composited texture set landed\n";
        } else if (!result.bitmap.empty()) {
          std::cerr << "[startup] +" << msSinceStart() << "ms first texture landed ("
                    << result.bitmap.dimensions.x << "x" << result.bitmap.dimensions.y << ")\n";
        }
      }
    }

    // Drain the command queue (ReplaceDocument from text edits,
    // SetTransform from SelectTool drags). Gated on the async renderer
    // being idle — if a render is in flight, the document is owned by
    // the render thread and we must not mutate it. Mutations queue up
    // in the CommandQueue and drain on the next idle frame.
    if (!asyncRenderer.isBusy()) {
      ZoneScopedN("flushFrame");
      if (app.flushFrame()) {
        refreshPendingSelectionBoundsCache();
      }
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

    // Canvas→text writeback is latched across frames. Drag completion
    // can happen after this frame's `flushFrame()` and render request,
    // so we snapshot the target + transform in SelectTool and retry the
    // text splice here until it succeeds, even while the async renderer
    // is busy.
    applyPendingElementRemoveWritebacks();
    applyPendingTransformWriteback();

    {
      double currentDisplayScale = 1.0;
#ifdef __EMSCRIPTEN__
      currentDisplayScale = CurrentDisplayScale();
#else
      float xScale = 1.0f;
      float yScale = 1.0f;
      glfwGetWindowContentScale(window, &xScale, &yScale);
      currentDisplayScale = static_cast<double>(xScale);
#endif
      currentDisplayScale = std::max(1.0, currentDisplayScale);
      io.DisplayFramebufferScale =
          ImVec2(static_cast<float>(currentDisplayScale), static_cast<float>(currentDisplayScale));
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    if (app.hasDocument()) {
      viewport.documentViewBox = ResolveDocumentViewBox(app.document().document());
    }

    const auto applyZoom = [&](double factor, const donner::Vector2d& focalScreen) {
      viewport.zoomAround(viewport.zoom * factor, focalScreen);
    };

    const auto tryOpenPath = [&](const std::string& rawPath) {
      const std::filesystem::path path(rawPath);
      std::ifstream file(path, std::ios::binary);
      if (!file) {
        openFileError = "Could not open file.";
        return false;
      }

      std::ostringstream out;
      out << file.rdbuf();
      const std::string contents = std::move(out).str();
      if (!app.loadFromString(contents)) {
        openFileError = "Failed to parse SVG.";
        return false;
      }

      textEditor.setText(contents);
      textEditor.resetTextChanged();
      previousSourceText = contents;
      lastWritebackSourceText.reset();
      app.setCurrentFilePath(path.string());
      app.setCleanSourceText(contents);
      pendingTransformWriteback.reset();
      pendingElementRemoveWritebacks.clear();
      pendingClick.reset();
      textChangePending = false;
      textDispatchThrottled = false;
      textChangeIdleTimer = 0.0f;
      lastHighlightedSelection.reset();
      refreshPendingSelectionBoundsCache();
      openFileError.clear();
      return true;
    };

#ifdef __EMSCRIPTEN__
    if (gPendingBrowserUploadPath.has_value() && !asyncRenderer.isBusy()) {
      const std::string path = *gPendingBrowserUploadPath;
      gPendingBrowserUploadPath.reset();
      (void)tryOpenPath(path);
    }
#endif

    const auto triggerOpen = [&]() {
#ifdef __EMSCRIPTEN__
      ShowBrowserOpenFileDialog(kMaxWasmUploadBytes);
#else
      std::fill(openFilePathBuffer.begin(), openFilePathBuffer.end(), '\0');
      if (app.currentFilePath().has_value()) {
        std::strncpy(openFilePathBuffer.data(), app.currentFilePath()->c_str(),
                     openFilePathBuffer.size() - 1);
      }
      openFileError.clear();
      openFileModalRequested = true;
#endif
    };
    const auto triggerUndo = [&]() {
      if (app.canUndo()) {
        app.undo();
      }
    };
    const auto triggerRedo = [&]() { app.redo(); };
    const auto triggerQuit = [&]() { glfwSetWindowShouldClose(window, GLFW_TRUE); };
    const auto triggerZoomIn = [&]() { applyZoom(kKeyboardZoomStep, viewport.paneCenter()); };
    const auto triggerZoomOut = [&]() {
      applyZoom(1.0 / kKeyboardZoomStep, viewport.paneCenter());
    };
    const auto triggerActualSize = [&]() { viewport.resetTo100Percent(); };

    const bool sourcePaneFocused = textEditor.isFocused();
    const bool canCanvasRedo = app.undoTimeline().entryCount() > 0;
    bool openAboutPopup = false;
    bool openLicensesPopup = false;

    // Global keyboard shortcuts. The source pane keeps its own text
    // editing shortcuts; these handlers only cover app-level actions
    // or render-pane actions that do not belong to `TextEditor`.
    {
      const bool anyPopupOpen = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup);
      const bool cmd = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper;
      const bool shift = ImGui::GetIO().KeyShift;
      const bool pressedZ = ImGui::IsKeyPressed(ImGuiKey_Z, /*repeat=*/false);

      if (!anyPopupOpen && cmd && !shift && ImGui::IsKeyPressed(ImGuiKey_O, /*repeat=*/false)) {
        triggerOpen();
      }

      if (!anyPopupOpen && cmd && !shift && ImGui::IsKeyPressed(ImGuiKey_Q, /*repeat=*/false)) {
        triggerQuit();
      }

      if (!sourcePaneFocused) {
        if (pressedZ && cmd && !shift) {
          triggerUndo();
        } else if (pressedZ && cmd && shift) {
          triggerRedo();
        }
      }

      if (!anyPopupOpen && cmd &&
          (ImGui::IsKeyPressed(ImGuiKey_Equal, /*repeat=*/false) ||
           ImGui::IsKeyPressed(ImGuiKey_KeypadAdd, /*repeat=*/false))) {
        triggerZoomIn();
      }
      if (!anyPopupOpen && cmd &&
          (ImGui::IsKeyPressed(ImGuiKey_Minus, /*repeat=*/false) ||
           ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract, /*repeat=*/false))) {
        triggerZoomOut();
      }
      if (!anyPopupOpen && cmd && ImGui::IsKeyPressed(ImGuiKey_0, /*repeat=*/false)) {
        triggerActualSize();
      }

      // Escape: clear the current selection. Modal popups capture Escape
      // first (to close themselves), so this only fires when no popup is
      // active.
      if (!anyPopupOpen && ImGui::IsKeyPressed(ImGuiKey_Escape, /*repeat=*/false) &&
          app.hasSelection()) {
        app.setSelection(std::nullopt);
      }

      // Delete / Backspace: detach the selected element from the tree
      // via a `DeleteElement` command. The command flows through the
      // queue like any other mutation and the actual detach happens on
      // the next `flushFrame`. Undo for this action is not supported
      // (see `EditorCommand::Kind::DeleteElement`).
      const bool deleteKey = ImGui::IsKeyPressed(ImGuiKey_Delete, /*repeat=*/false) ||
                             ImGui::IsKeyPressed(ImGuiKey_Backspace, /*repeat=*/false);
      if (deleteKey && app.hasSelection() && !anyPopupOpen && !ImGui::GetIO().WantCaptureKeyboard) {
        const std::vector<donner::svg::SVGElement> selected = app.selectedElements();
        app.setSelection(std::nullopt);
        for (const auto& element : selected) {
          if (auto target = donner::editor::captureAttributeWritebackTarget(element);
              target.has_value()) {
            app.enqueueElementRemoveWriteback(
                donner::editor::EditorApp::CompletedElementRemoveWriteback{
                    .target = std::move(*target),
                });
          }
          app.applyMutation(donner::editor::EditorCommand::DeleteElementCommand(element));
        }
      }
    }

    if (ImGui::BeginMainMenuBar()) {
      ImGui::PushFont(uiFontBold);
      if (ImGui::BeginMenu("Donner SVG Editor")) {
        ImGui::PopFont();
        if (ImGui::MenuItem("About...")) {
          openAboutPopup = true;
        }
#ifndef __EMSCRIPTEN__
        if (ImGui::MenuItem("Quit Donner", "Cmd+Q")) {
          triggerQuit();
        }
#endif
        ImGui::EndMenu();
      } else {
        ImGui::PopFont();
      }

      if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open...", "Cmd+O")) {
          triggerOpen();
        }
        // TODO: Restore File → Save / Save As once persistence UI exists.
        ImGui::EndMenu();
      }

      if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Cmd+Z", false, app.canUndo())) {
          triggerUndo();
        }
        if (ImGui::MenuItem("Redo", "Cmd+Shift+Z", false, canCanvasRedo)) {
          triggerRedo();
        }
        ImGui::Separator();

        if (!sourcePaneFocused) {
          ImGui::BeginDisabled();
        }
        if (ImGui::MenuItem("Cut", "Cmd+X", false, sourcePaneFocused)) {
          textEditor.cut();
        }
        if (!sourcePaneFocused) {
          ImGui::EndDisabled();
        }

        if (!sourcePaneFocused) {
          ImGui::BeginDisabled();
        }
        if (ImGui::MenuItem("Copy", "Cmd+C", false, sourcePaneFocused)) {
          textEditor.copy();
        }
        if (!sourcePaneFocused) {
          ImGui::EndDisabled();
        }

        if (!sourcePaneFocused) {
          ImGui::BeginDisabled();
        }
        if (ImGui::MenuItem("Paste", "Cmd+V", false, sourcePaneFocused)) {
          textEditor.paste();
        }
        if (!sourcePaneFocused) {
          ImGui::EndDisabled();
        }

        if (!sourcePaneFocused) {
          ImGui::BeginDisabled();
        }
        if (ImGui::MenuItem("Select All", "Cmd+A", false, sourcePaneFocused)) {
          textEditor.selectAll();
        }
        if (!sourcePaneFocused) {
          ImGui::EndDisabled();
        }
        ImGui::EndMenu();
      }

      if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Zoom In", "Cmd+=")) {
          triggerZoomIn();
        }
        if (ImGui::MenuItem("Zoom Out", "Cmd+-")) {
          triggerZoomOut();
        }
        if (ImGui::MenuItem("Actual Size", "Cmd+0")) {
          triggerActualSize();
        }
        ImGui::EndMenu();
      }

      ImGui::EndMainMenuBar();
    }
    if (openAboutPopup) {
      ImGui::OpenPopup("About Donner SVG Editor");
    }
    if (openFileModalRequested) {
      ImGui::OpenPopup("Open SVG");
      openFileModalRequested = false;
    }

    {
      const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
      ImGui::SetNextWindowSize(ImVec2(720.0f, 0.0f), ImGuiCond_Appearing);
      ImGui::SetNextWindowPos(ImVec2(displaySize.x * 0.5f, displaySize.y * 0.5f),
                              ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
      if (ImGui::BeginPopupModal("Open SVG", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Enter the SVG path to open.");
        const bool submitted =
            ImGui::InputText("Path", openFilePathBuffer.data(), openFilePathBuffer.size(),
                             ImGuiInputTextFlags_EnterReturnsTrue);
        if (!openFileError.empty()) {
          ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", openFileError.c_str());
        }

        if (submitted || ImGui::Button("Open")) {
          if (tryOpenPath(openFilePathBuffer.data())) {
            ImGui::CloseCurrentPopup();
          }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
          openFileError.clear();
          ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
      }
    }

    {
      const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
      ImGui::SetNextWindowSize(ImVec2(520.0f, 0.0f), ImGuiCond_Appearing);
      ImGui::SetNextWindowPos(ImVec2(displaySize.x * 0.5f, displaySize.y * 0.5f),
                              ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
      if (ImGui::BeginPopupModal("About Donner SVG Editor", nullptr,
                                 ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("About Donner SVG Editor");
        ImGui::Separator();
        ImGui::TextUnformatted("(c) 2024-2026 Jeff McGlynn");
        ImGui::Spacing();
        ImGui::TextWrapped(
            "An SVG editor powered by the Donner, a browser-grade SVG2 rendering engine written in "
            "C++20.");
        ImGui::TextUnformatted("https://github.com/jwmcglynn/donner");
        ImGui::Separator();
        if (ImGui::Button("Show Licenses")) {
          openLicensesPopup = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Close")) {
          ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
      }
    }
    if (openLicensesPopup) {
      ImGui::OpenPopup("Third-Party Licenses");
    }

    {
      const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
      ImGui::SetNextWindowSize(ImVec2(displaySize.x * 0.75f, displaySize.y * 0.8f),
                               ImGuiCond_Appearing);
      ImGui::SetNextWindowPos(ImVec2(displaySize.x * 0.5f, displaySize.y * 0.5f),
                              ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
      if (ImGui::BeginPopupModal("Third-Party Licenses", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextUnformatted("Third-party license notices");
        ImGui::Separator();
        ImGui::InputTextMultiline(
            "##third_party_licenses", editorNoticeText.data(), editorNoticeText.size() + 1,
            ImVec2(-FLT_MIN, -ImGui::GetFrameHeightWithSpacing()), ImGuiInputTextFlags_ReadOnly);
        if (ImGui::Button("Close")) {
          ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
      }
    }

    ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(mainViewport->WorkPos);
    ImGui::SetNextWindowSize(mainViewport->WorkSize);
    ImGui::SetNextWindowViewport(mainViewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    const ImGuiWindowFlags dockspaceHostFlags =
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
    ImGui::Begin("##EditorDockspaceHost", nullptr, dockspaceHostFlags);
    ImGui::PopStyleVar(3);

    const ImGuiID dockspaceId = ImGui::GetID("EditorDockspace");
    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);

    if (!dockspaceInitialized) {
      dockspaceInitialized = true;

      ImGui::DockBuilderRemoveNode(dockspaceId);
      ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
      ImGui::DockBuilderSetNodeSize(dockspaceId, mainViewport->WorkSize);

      ImGuiID dockIdMainLeft = dockspaceId;
      ImGuiID dockIdRight = 0;
      ImGui::DockBuilderSplitNode(dockIdMainLeft, ImGuiDir_Left, kInitialMainLeftDockFraction,
                                  &dockIdMainLeft, &dockIdRight);

      ImGuiID dockIdSource = dockIdMainLeft;
      ImGuiID dockIdRender = 0;
      ImGui::DockBuilderSplitNode(dockIdSource, ImGuiDir_Left, kInitialSourceDockFraction,
                                  &dockIdSource, &dockIdRender);

      ImGuiID dockIdTree = dockIdRight;
      ImGuiID dockIdInspector = 0;
      ImGui::DockBuilderSplitNode(dockIdTree, ImGuiDir_Up, kInitialTreeDockFraction, &dockIdTree,
                                  &dockIdInspector);

      ImGui::DockBuilderDockWindow("Source", dockIdSource);
      ImGui::DockBuilderDockWindow("Render", dockIdRender);
      ImGui::DockBuilderDockWindow("Tree View", dockIdTree);
      ImGui::DockBuilderDockWindow("Inspector", dockIdInspector);
      ImGui::DockBuilderFinish(dockspaceId);
    }

    ImGui::End();

    ImGuiWindowClass windowClassNoUndocking;
    windowClassNoUndocking.DockNodeFlagsOverrideSet =
        ImGuiDockNodeFlags_NoUndocking |
        static_cast<ImGuiDockNodeFlags_>(ImGuiDockNodeFlags_NoTabBar);

    ImGui::SetNextWindowClass(&windowClassNoUndocking);
    ImGui::Begin("Source");
    ImGui::PushFont(codeFont);
    textEditor.render("##source");
    ImGui::PopFont();

    // Dispatch the current text buffer as an EditorCommand. Used by
    // both the leading-edge and trailing-edge paths below.
    const auto dispatchTextChange = [&]() {
      const std::string newSource = textEditor.getText();
      (void)donner::editor::DispatchSourceTextChange(app, newSource, &previousSourceText,
                                                     &lastWritebackSourceText);
      app.syncDirtyFromSource(newSource);
    };

    if (textEditor.isTextChanged()) {
      textEditor.resetTextChanged();
      app.syncDirtyFromSource(textEditor.getText());

      if (!textDispatchThrottled) {
        // Leading-edge fire: no dispatch in the last
        // kTextChangeDebounceSeconds window, so run this one
        // immediately. The canvas updates on the very next frame
        // after the keystroke — no perceptible "one extra press"
        // latency.
        dispatchTextChange();
        textDispatchThrottled = true;
        textChangePending = false;
      } else {
        // Inside the throttle window: stash this edit as "pending"
        // so the trailing dispatch catches it when the user stops
        // typing for `kTextChangeDebounceSeconds`.
        textChangePending = true;
      }
      textChangeIdleTimer = 0.0f;
    } else if (textDispatchThrottled) {
      // No new edit this frame. Advance the idle timer; when it
      // expires, drop the throttle and (if there were throttled
      // edits) flush the final state.
      textChangeIdleTimer += ImGui::GetIO().DeltaTime;
      if (textChangeIdleTimer >= kTextChangeDebounceSeconds) {
        if (textChangePending) {
          dispatchTextChange();
          textChangePending = false;
        }
        textDispatchThrottled = false;
        textChangeIdleTimer = 0.0f;
      }
    }
    ImGui::End();

    // Render pane + SelectTool interaction. The window is docked into the
    // DockSpace above, so the user gets the same resizable text/render split
    // that the original `jwmcglynn/donner-editor` used.
    ImGui::SetNextWindowClass(&windowClassNoUndocking);
    ImGui::Begin("Render");

    // Refresh the viewport's per-frame inputs from ImGui / GLFW. Pane
    // origin and size come from the current ImGui layout; viewBox
    // comes from the SVG document; DPR comes from GLFW. Zoom and pan
    // are persistent state and are mutated only by user input handlers
    // below.
    const ImVec2 contentRegion = ImGui::GetContentRegionAvail();
    const ImVec2 paneOriginImGui = ImGui::GetCursorScreenPos();
    viewport.paneOrigin = donner::Vector2d(paneOriginImGui.x, paneOriginImGui.y);
    viewport.paneSize = donner::Vector2d(contentRegion.x, contentRegion.y);
    {
#ifdef __EMSCRIPTEN__
      viewport.devicePixelRatio = CurrentDisplayScale();
#else
      float xScale = 1.0f;
      float yScale = 1.0f;
      glfwGetWindowContentScale(window, &xScale, &yScale);
      viewport.devicePixelRatio = static_cast<double>(xScale);
#endif
    }
    if (app.hasDocument()) {
      viewport.documentViewBox = ResolveDocumentViewBox(app.document().document());
    }

    const bool renderPaneUsable = contentRegion.x > 0.0f && contentRegion.y > 0.0f;

    // First frame after the pane has a non-zero size: snap to 100%
    // with the viewBox center pinned to the pane center. Subsequent
    // frames preserve the user's zoom/pan state.
    if (!viewportInitialized && renderPaneUsable && app.hasDocument()) {
      viewport.resetTo100Percent();
      viewportInitialized = true;
    }

    // Canvas-size dispatch + async render request + overlay re-render.
    // Both renderer paths need exclusive access to the document, so
    // both are gated on the worker thread being idle. Once we hand
    // the document to the worker (`requestRender`), nothing in this
    // frame may touch it again until the next frame's `pollResult`
    // / `flushFrame`.
    if (!asyncRenderer.isBusy() && app.hasDocument() && renderPaneUsable) {
      const donner::Vector2i desiredCanvasSize = viewport.desiredCanvasSize();
      const donner::Vector2i currentSize = app.document().document().canvasSize();
      if (currentSize.x != desiredCanvasSize.x || currentSize.y != desiredCanvasSize.y) {
        app.document().document().setCanvasSize(desiredCanvasSize.x, desiredCanvasSize.y);
      }
      const donner::Vector2i currentCanvasSize = app.document().document().canvasSize();
      const auto currentVersion = app.document().currentFrameVersion();
      const auto dragPreview = selectTool.activeDragPreview();
      const donner::Entity prewarmEntity = selectedExperimentalEntity();
      experimentalDragPresentation.clearSettlingIfSelectionChanged(prewarmEntity,
                                                                   dragPreview.has_value());

      // Refresh the cached document-space selection bounds before we
      // hand the document to the worker. Triggers on selection changes
      // (click / shift-click / marquee resolve / clear) and on any
      // document version change that may have moved the selected
      // geometry.
      //
      // The separate overlay re-render runs FIRST so the doc-render
      // request below can kick the worker without racing us. It only
      // needs to track selection, canvas size, and document version
      // now that AABBs and marquee chrome are immediate-mode draws.
      //
      // The version check ensures we re-rasterize the overlay against
      // the latest flushed document state. Upload is deferred until
      // the matching document bitmap lands so the chrome and SVG stay
      // lock-step during drags.
      const auto& overlaySelection = app.selectedElements();
      const bool selectionBoundsChanged = overlaySelection != selectionBoundsCache.lastSelection;
      if (selectionBoundsChanged || currentVersion != selectionBoundsCache.lastRefreshVersion) {
        refreshPendingSelectionBoundsCache();
      }

      const bool selectionDiffers = overlaySelection != lastOverlaySelectionVec;
      if (selectionDiffers || currentCanvasSize != lastOverlayCanvasSize ||
          currentVersion != lastOverlayVersion) {
        rasterizeOverlayForCurrentSelection();
      }

      const bool needsExperimentalLayerCapture =
          experimentalMode && dragPreview.has_value() &&
          (!experimentalDragPresentation.hasCachedTextures ||
           experimentalDragPresentation.cachedEntity != dragPreview->entity ||
           experimentalDragPresentation.cachedVersion != currentVersion ||
           experimentalDragPresentation.cachedCanvasSize != currentCanvasSize);
      const bool needsExperimentalPrewarm =
          experimentalDragPresentation.shouldPrewarm(prewarmEntity, currentVersion,
                                                     currentCanvasSize, /*dragActive=*/false);
      const bool needsRegularRender =
          (!experimentalMode || !dragPreview.has_value()) &&
          (currentVersion != lastRenderedVersion || currentCanvasSize != lastRenderedCanvasSize ||
           experimentalDragPresentation.waitingForFullRender);

      // Request a new SVG render if anything that affects the bitmap
      // has changed. Selection is deliberately NOT in the trigger set:
      // the document bitmap doesn't depend on the selection, and
      // selection chrome lives in the overlay texture above.
      if (needsExperimentalLayerCapture || needsExperimentalPrewarm || needsRegularRender) {
        donner::editor::RenderRequest req;
        req.document = &app.document().document();
        req.version = currentVersion;
        req.selection = std::nullopt;
        if (dragPreview.has_value()) {
          req.dragPreview = donner::editor::RenderRequest::DragPreview{
              .entity = dragPreview->entity,
              .translation = dragPreview->translation,
          };
        } else if (needsExperimentalPrewarm) {
          req.dragPreview = donner::editor::RenderRequest::DragPreview{
              .entity = prewarmEntity,
              .translation = donner::Vector2d::Zero(),
          };
        }
        asyncRenderer.requestRender(req);
        if (!loggedFirstRenderRequest) {
          loggedFirstRenderRequest = true;
          std::cerr << "[startup] +" << msSinceStart()
                    << "ms first render request kicked off (canvas " << currentCanvasSize.x << "x"
                    << currentCanvasSize.y << ")\n";
        }

        if (needsRegularRender) {
          lastRenderedVersion = currentVersion;
          lastRenderedCanvasSize = currentCanvasSize;
        }
      }
    }

    // Reserve the full content area as an invisible button so ImGui
    // routes mouse events (wheel, drag, click) to this pane even when
    // the cursor is over empty space around the image.
    bool paneHovered = false;
    donner::Box2d paneRect;
    if (renderPaneUsable) {
      ImGui::InvisibleButton("##render_canvas", contentRegion,
                             ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
      paneHovered = ImGui::IsItemHovered();
      paneRect = donner::Box2d::FromXYWH(viewport.paneOrigin.x, viewport.paneOrigin.y,
                                         viewport.paneSize.x, viewport.paneSize.y);
    } else {
      ImGui::TextDisabled("(render pane is collapsed)");
    }

    // Space+drag → pan. Middle-click+drag is a common alternative; both
    // work. `panning` is a one-frame edge-detected state so the first
    // frame of a drag captures the starting mouse position.
    const bool spaceHeld = ImGui::IsKeyDown(ImGuiKey_Space);
    const bool middleDown = ImGui::IsMouseDown(ImGuiMouseButton_Middle);
    if (paneHovered && (spaceHeld || middleDown) && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
        !panning) {
      panning = true;
      lastPanMouse = ImGui::GetMousePos();
    } else if (middleDown && !panning) {
      panning = true;
      lastPanMouse = ImGui::GetMousePos();
    }
    if (panning) {
      const ImVec2 now = ImGui::GetMousePos();
      viewport.panBy(donner::Vector2d(now.x - lastPanMouse.x, now.y - lastPanMouse.y));
      lastPanMouse = now;
      if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
          !ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
        panning = false;
      }
    }

    // Raw GLFW scroll events are queued in the window callback above
    // so we can preserve event order and cursor position instead of
    // relying on ImGui's per-frame wheel accumulation. This lets the
    // render pane treat bare two-finger scroll as pan and modified
    // scroll as zoom while leaving the keyboard/menu zoom paths alone.
    // TODO: If GLFW gains first-class touch callbacks on Linux/Wayland,
    // route single-finger touch through the existing left-click / drag
    // pipeline instead of adding a parallel tool path here.
    const donner::editor::RenderPaneGestureContext gestureContext{
        .paneRect = paneRect,
        .mouseDragPanActive = panning,
    };
    // Suppress render-pane zoom/pan while an ImGui popup modal is open
    // (About, Licenses, Open File, etc.) — the raw GLFW scroll callback
    // fires regardless of ImGui's modal state, so without this the
    // Licenses dialog scroll wheel would zoom the canvas underneath.
    // Drain the queue either way so events don't pile up.
    const bool modalCapturingInput = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup);
    if (!modalCapturingInput) {
      for (const auto& event : pendingScrollEvents.events) {
        const auto action = donner::editor::ClassifyRenderPaneScrollGesture(
            event, gestureContext, kWheelZoomStep, kTrackpadPanPixelsPerScrollUnit);
        if (!action.has_value()) {
          continue;
        }
        donner::editor::ApplyRenderPaneGesture(viewport, *action);
      }
    }
    pendingScrollEvents.events.clear();

    const auto displayedDragPreview =
        experimentalDragPresentation.presentationPreview(selectTool.activeDragPreview());
    if ((textureWidth > 0 && textureHeight > 0) ||
        experimentalDragPresentation.hasCachedTextures) {
      // The on-screen rectangle of the document image is computed from
      // the viewport, NOT from the texture dimensions. The texture's
      // resolution only affects sampling fidelity; layout is purely a
      // function of `viewport`. So we can stretch a stale low-res
      // texture into the new on-screen rect during a zoom transient
      // and the click math (which also goes through `viewport`) stays
      // consistent with what the user sees.
      const donner::Box2d screenRect = viewport.imageScreenRect();
      const ImVec2 imageOrigin(static_cast<float>(screenRect.topLeft.x),
                               static_cast<float>(screenRect.topLeft.y));
      const ImVec2 imageBottomRight(static_cast<float>(screenRect.bottomRight.x),
                                    static_cast<float>(screenRect.bottomRight.y));
      const donner::Vector2d dragScreenOffset =
          DragPreviewScreenOffset(displayedDragPreview, viewport);

      // Draw the document image into the pane's foreground draw list,
      // followed by the overlay texture composited at the same screen
      // rectangle.
      ImDrawList* paneDrawList = ImGui::GetWindowDrawList();
      const auto DrawCheckerboard = [](ImDrawList* drawList, const ImVec2& topLeft,
                                       const ImVec2& bottomRight) {
        constexpr float kCheckerSize = 16.0f;
        const ImVec2 snappedTopLeft(std::floor(topLeft.x), std::floor(topLeft.y));
        const ImVec2 snappedBottomRight(std::floor(bottomRight.x), std::floor(bottomRight.y));
        if (snappedTopLeft.x >= snappedBottomRight.x || snappedTopLeft.y >= snappedBottomRight.y) {
          return;
        }

        drawList->PushClipRect(snappedTopLeft, snappedBottomRight,
                               /*intersect_with_current_clip_rect=*/true);
        const float startY = std::floor(snappedTopLeft.y / kCheckerSize) * kCheckerSize;
        const float startX = std::floor(snappedTopLeft.x / kCheckerSize) * kCheckerSize;
        for (float y = startY; y < snappedBottomRight.y; y += kCheckerSize) {
          const int row = static_cast<int>(std::floor(y / kCheckerSize));
          for (float x = startX; x < snappedBottomRight.x; x += kCheckerSize) {
            const int column = static_cast<int>(std::floor(x / kCheckerSize));
            const ImU32 color =
                ((row + column) % 2 == 0) ? IM_COL32(60, 60, 60, 255) : IM_COL32(40, 40, 40, 255);
            drawList->AddRectFilled(ImVec2(x, y), ImVec2(x + kCheckerSize, y + kCheckerSize),
                                    color);
          }
        }
        drawList->PopClipRect();
      };
      DrawCheckerboard(paneDrawList, imageOrigin, imageBottomRight);
      if (experimentalMode &&
          experimentalDragPresentation.shouldDisplayCompositedLayers(
              selectTool.activeDragPreview()) &&
          dragPromotedTextureWidth > 0 && dragPromotedTextureHeight > 0 &&
          displayedDragPreview.has_value()) {
        if (dragBackgroundTextureWidth > 0 && dragBackgroundTextureHeight > 0) {
          paneDrawList->AddImage(
              static_cast<ImTextureID>(static_cast<std::uintptr_t>(dragBackgroundTexture)),
              imageOrigin, imageBottomRight);
        }

        const ImVec2 promotedOrigin(imageOrigin.x + static_cast<float>(dragScreenOffset.x),
                                    imageOrigin.y + static_cast<float>(dragScreenOffset.y));
        const ImVec2 promotedBottomRight(
            imageBottomRight.x + static_cast<float>(dragScreenOffset.x),
            imageBottomRight.y + static_cast<float>(dragScreenOffset.y));
        paneDrawList->AddImage(
            static_cast<ImTextureID>(static_cast<std::uintptr_t>(dragPromotedTexture)),
            promotedOrigin, promotedBottomRight);

        if (dragForegroundTextureWidth > 0 && dragForegroundTextureHeight > 0) {
          paneDrawList->AddImage(
              static_cast<ImTextureID>(static_cast<std::uintptr_t>(dragForegroundTexture)),
              imageOrigin, imageBottomRight);
        }
      } else {
        paneDrawList->AddImage(static_cast<ImTextureID>(static_cast<std::uintptr_t>(texture)),
                               imageOrigin, imageBottomRight);
      }
      if (overlayTextureWidth > 0 && overlayTextureHeight > 0) {
        const ImVec2 overlayOrigin(imageOrigin.x + static_cast<float>(dragScreenOffset.x),
                                   imageOrigin.y + static_cast<float>(dragScreenOffset.y));
        const ImVec2 overlayBottomRight(
            imageBottomRight.x + static_cast<float>(dragScreenOffset.x),
            imageBottomRight.y + static_cast<float>(dragScreenOffset.y));
        paneDrawList->AddImage(
            static_cast<ImTextureID>(static_cast<std::uintptr_t>(overlayTexture)), overlayOrigin,
            overlayBottomRight);
      }

      const auto screenToDocument = [&](const ImVec2& screenPoint) -> donner::Vector2d {
        return viewport.screenToDocument(donner::Vector2d(screenPoint.x, screenPoint.y));
      };

      // Only treat a click as a tool event when it's NOT a pan gesture.
      // Tool events that read from the document (hit test on mouse-down)
      // must not fire while the render thread holds the document.
      const bool toolEligible = paneHovered && !panning && !spaceHeld;

      // Buffer the click position when ImGui sees the press event,
      // even if the worker is currently busy. This lets clicks that
      // arrive during a slow initial render still register — they'll
      // be processed on the next idle frame instead of getting
      // silently dropped. Without this, the user perceives "clicks
      // are broken" until the worker happens to be idle at exactly
      // the right moment.
      if (toolEligible && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        donner::editor::MouseModifiers modifiers;
        modifiers.shift = ImGui::GetIO().KeyShift;
        pendingClick = PendingClick{
            .documentPoint = screenToDocument(ImGui::GetMousePos()),
            .modifiers = modifiers,
        };
      }
      if (pendingClick.has_value() && !asyncRenderer.isBusy()) {
        selectTool.onMouseDown(app, pendingClick->documentPoint, pendingClick->modifiers);
        refreshPendingSelectionBoundsCache();
        // Re-rasterize the overlay texture NOW (same frame, before
        // `AddImage` is consumed at end-of-frame) so the path outline
        // updates together with the AABB. The overlay block earlier in
        // this frame ran with the pre-click selection; without this
        // refresh the path outline lags 1 frame behind the AABB.
        rasterizeOverlayForCurrentSelection();
        pendingClick.reset();
      }
      // Drag *or* marquee continues even after the cursor leaves the
      // image. `onMouseMove` reads cached drag/marquee state and is
      // safe during a render. `onMouseUp` records undo state and (for
      // marqueeing) resolves the rect to a selection set.
      if (selectTool.isDragging() || selectTool.isMarqueeing()) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && !spaceHeld) {
          selectTool.onMouseMove(app, screenToDocument(ImGui::GetMousePos()),
                                 /*buttonHeld=*/true);
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
          const auto previewBeforeRelease = selectTool.activeDragPreview();
          selectTool.onMouseUp(app, screenToDocument(ImGui::GetMousePos()));
          if (experimentalMode && previewBeforeRelease.has_value()) {
            experimentalDragPresentation.beginSettling(
                previewBeforeRelease, app.document().currentFrameVersion() + 1);
          }
          if (!asyncRenderer.isBusy()) {
            refreshPendingSelectionBoundsCache();
            // Marquee resolve may have added new selected elements;
            // click-to-deselect may have cleared them. Either way the
            // overlay texture needs the new selection before the frame
            // renders.
            rasterizeOverlayForCurrentSelection();
          }
        }
      }

      if (experimentalMode && !asyncRenderer.isBusy() && app.hasDocument()) {
        const auto dragPreview = selectTool.activeDragPreview();
        const donner::Vector2i currentCanvasSize = app.document().document().canvasSize();
        const auto currentVersion = app.document().currentFrameVersion();
        const donner::Entity prewarmEntity = selectedExperimentalEntity();
        experimentalDragPresentation.clearSettlingIfSelectionChanged(prewarmEntity,
                                                                     dragPreview.has_value());

        const bool needsDragCapture =
            dragPreview.has_value() &&
            (!experimentalDragPresentation.hasCachedTextures ||
             experimentalDragPresentation.cachedEntity != dragPreview->entity ||
             experimentalDragPresentation.cachedVersion != currentVersion ||
             experimentalDragPresentation.cachedCanvasSize != currentCanvasSize);
        const bool needsPrewarm =
            !dragPreview.has_value() &&
            experimentalDragPresentation.shouldPrewarm(prewarmEntity, currentVersion,
                                                       currentCanvasSize, /*dragActive=*/false);

        if (needsDragCapture || needsPrewarm) {
          donner::editor::RenderRequest req;
          req.renderer = &renderer;
          req.document = &app.document().document();
          req.version = currentVersion;
          req.dragPreview = donner::editor::RenderRequest::DragPreview{
              .entity = needsDragCapture ? dragPreview->entity : prewarmEntity,
              .translation =
                  needsDragCapture ? dragPreview->translation : donner::Vector2d::Zero(),
          };
          asyncRenderer.requestRender(req);
        }
      }

      for (const donner::Box2d& screenRect : donner::editor::ComputeSelectionAabbScreenRects(
               viewport, std::span<const donner::Box2d>(selectionBoundsCache.displayedBoundsDoc))) {
        paneDrawList->AddRect(ImVec2(static_cast<float>(screenRect.topLeft.x + dragScreenOffset.x),
                                     static_cast<float>(screenRect.topLeft.y + dragScreenOffset.y)),
                              ImVec2(static_cast<float>(screenRect.bottomRight.x +
                                                        dragScreenOffset.x),
                                     static_cast<float>(screenRect.bottomRight.y +
                                                        dragScreenOffset.y)),
                              kSelectionChromeColor, 0.0f, ImDrawFlags_None,
                              kSelectionChromeThickness);
      }

      if (auto marqueeRectDoc = selectTool.marqueeRect(); marqueeRectDoc.has_value()) {
        const donner::Box2d marqueeRectScreen = viewport.documentToScreen(*marqueeRectDoc);
        paneDrawList->AddRectFilled(ImVec2(static_cast<float>(marqueeRectScreen.topLeft.x),
                                           static_cast<float>(marqueeRectScreen.topLeft.y)),
                                    ImVec2(static_cast<float>(marqueeRectScreen.bottomRight.x),
                                           static_cast<float>(marqueeRectScreen.bottomRight.y)),
                                    kMarqueeFillColor);
        paneDrawList->AddRect(ImVec2(static_cast<float>(marqueeRectScreen.topLeft.x),
                                     static_cast<float>(marqueeRectScreen.topLeft.y)),
                              ImVec2(static_cast<float>(marqueeRectScreen.bottomRight.x),
                                     static_cast<float>(marqueeRectScreen.bottomRight.y)),
                              kMarqueeStrokeColor, 0.0f, ImDrawFlags_None, kMarqueeStrokeThickness);
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

    // Frame-time graph pinned to the bottom of the render pane. The
    // image's `InvisibleButton` consumes the entire content region so
    // the ImGui cursor sits at the bottom of the pane by this point;
    // we rewind it relative to the current content region height so
    // the graph stays pinned even when the dock splitter moves.
    {
      constexpr float kFramePadding = 8.0f;
      const float graphHeight = kFrameGraphHeight + ImGui::GetTextLineHeightWithSpacing();
      ImGui::SetCursorPos(ImVec2(kFramePadding, contentRegion.y - graphHeight - kFramePadding));
      RenderFrameGraph(frameHistory);
    }

    ImGui::End();

    const auto& selectionBeforeTree = app.selectedElement();
    if (selectionBeforeTree != lastTreeSelection) {
      treeviewPendingScroll = selectionBeforeTree.has_value() && !treeSelectionOriginatedInTree;
    }
    treeSelectionOriginatedInTree = false;

    ImGui::Begin("Tree View");

    if (!asyncRenderer.isBusy()) {
      TreeViewState treeState{
          .scrollTarget = selectionBeforeTree,
          .pendingScroll = treeviewPendingScroll,
      };
      RenderTreeView(app, treeState);
      treeviewPendingScroll = treeState.pendingScroll;
      if (treeState.selectionChangedInTree) {
        treeSelectionOriginatedInTree = true;
        treeviewPendingScroll = false;
      }
    } else {
      ImGui::TextDisabled("(rendering...)");
    }

    ImGui::End();
    lastTreeSelection = app.selectedElement();

    ImGui::Begin("Inspector");

    // Selection summary. Only safe to read when the async renderer is
    // idle — otherwise the worker holds exclusive access to the document.
    if (!asyncRenderer.isBusy()) {
      RenderInspector(app);
    } else {
      ImGui::TextDisabled("(rendering…)");
    }
    ImGui::Separator();
    ImGui::Text("Zoom: %.0f%%", viewport.zoom * 100.0);
    ImGui::Text("Pan anchor: doc=(%.1f, %.1f) screen=(%.0f, %.0f)", viewport.panDocPoint.x,
                viewport.panDocPoint.y, viewport.panScreenPoint.x, viewport.panScreenPoint.y);
    ImGui::Text("DPR: %.2fx", viewport.devicePixelRatio);
    ImGui::TextDisabled("scroll = pan");
    ImGui::TextDisabled("Cmd+scroll = zoom");
    ImGui::TextDisabled("space+drag = pan");
    ImGui::TextDisabled("Cmd+0 = 100%%");

    ImGui::End();

    ImGui::Render();
    int displayWidth = 0;
    int displayHeight = 0;
#ifdef __EMSCRIPTEN__
    displayWidth = canvasPixelWidth();
    displayHeight = canvasPixelHeight();
#else
    glfwGetFramebufferSize(window, &displayWidth, &displayHeight);
#endif
    glViewport(0, 0, displayWidth, displayHeight);
    glClearColor(0.10f, 0.10f, 0.10f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    glfwSwapBuffers(window);
    FrameMark;
#ifdef __EMSCRIPTEN__
    emscripten_sleep(0);
#endif
  }

  // ---------------------------------------------------------------------------
  // Teardown
  // ---------------------------------------------------------------------------
  glfwSetScrollCallback(window, pendingScrollEvents.previousCallback);
  glfwSetWindowUserPointer(window, nullptr);
  glDeleteTextures(1, &texture);
  glDeleteTextures(1, &overlayTexture);
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}
