/// @file
///
/// `//donner/editor:editor` — the full-featured Donner editor binary.
///
/// This is the thin-client editor shell. It drives an `EditorBackendClient`
/// for all document operations (parse, selection, undo, export) and handles
/// only viewport, text editing, and chrome display locally.
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
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/ParseDiagnostic.h"
#include "donner/base/Transform.h"
#include "donner/editor/AddressBar.h"
#include "donner/editor/AddressBarDispatcher.h"
#include "donner/editor/ContentSniffer.h"
#include "donner/editor/EditorBackendClient.h"
#include "donner/editor/EditorIcon.h"
#include "donner/editor/EditorSplash.h"
#include "donner/editor/LocalPathDisplay.h"
#include "donner/editor/Notice.h"
#include "donner/editor/PinchEventMonitor.h"
#include "donner/editor/repro/ReproRecorder.h"
#include "donner/editor/sandbox/bridge/BridgeTexture.h"
#ifndef __EMSCRIPTEN__
// The desktop address-bar stack uses `SvgSource` + `ResourceGatekeeper` to
// enforce the local resource policy before shelling out to `curl`. On WASM
// the browser is the policy (CORS / mixed-content / user-gesture), so we
// drop those headers entirely — their libraries aren't compiled into the
// WASM target.
#include "donner/editor/ResourcePolicy.h"
#include "donner/editor/sandbox/SvgSource.h"
#endif
#include "donner/editor/SelectionOverlay.h"
#include "donner/editor/SvgFetcher.h"
#include "donner/editor/TextBuffer.h"
#include "donner/editor/TextEditor.h"
#include "donner/editor/TracyWrapper.h"
#include "donner/editor/ViewportState.h"
#include "donner/svg/renderer/PixelFormatUtils.h"

// The session-backed (subprocess sandbox) backend only links on Linux —
// `donner/editor/sandbox:session` is gated to `@platforms//os:linux` in
// BUILD.bazel. WASM and non-Linux desktops (macOS) pick up the in-process
// client instead, which doesn't need `SandboxSession`.
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
#include "donner/editor/sandbox/SandboxSession.h"
#endif

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

#include "embed_resources/FiraCodeFont.h"
#include "embed_resources/RobotoFont.h"
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
constexpr float kWheelZoomStep = 1.1f;
constexpr float kKeyboardZoomStep = 1.5f;
constexpr double kTrackpadPanPixelsPerScrollUnit = 10.0;
constexpr float kTextChangeDebounceSeconds = 0.15f;
constexpr std::size_t kFrameHistoryCapacity = 120;
constexpr float kTargetFrameMs = 1000.0f / 60.0f;
constexpr float kFrameGraphWidth = 240.0f;
constexpr float kFrameGraphHeight = 32.0f;
constexpr double kDragMoveScreenEpsilonPx = 0.25;
#ifdef __EMSCRIPTEN__
constexpr int kMaxWasmUploadBytes = 32 * 1024 * 1024;
#endif

/// Fixed-size ring buffer of recent frame times (in milliseconds).
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

void RenderFrameGraph(const FrameHistory& history) {
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
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

  const ImVec2 bottomRight(origin.x + kFrameGraphWidth, origin.y + kFrameGraphHeight);
  dl->AddRectFilled(origin, bottomRight, IM_COL32(30, 30, 30, 255));

  constexpr float scaleMs = kTargetFrameMs * 2.0f;
  const float budgetY =
      origin.y + kFrameGraphHeight - (kTargetFrameMs / scaleMs) * kFrameGraphHeight;
  dl->AddLine(ImVec2(origin.x, budgetY), ImVec2(bottomRight.x, budgetY),
              IM_COL32(255, 255, 255, 80), 1.0f);

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

// --- Test-hook instrumentation (opt-in via `?test=1` in the URL). ---
//
// Exists so a Playwright harness can drive the WASM editor deterministically:
//   - `__donner_ready`   — set once the first frame has rendered.
//   - `__donner_metrics` — rolling frame-time buffer + poll / render counters.
//   - `__donner_simulate([{type,x,y,buttons}, ...])` — dispatches synthesized
//     mouse events at canvas-local coords, used by the drag-perf test.
//
// The hooks are strictly additive and behind a URL flag, so a normal page
// launch is byte-for-byte unchanged. See `donner/editor/wasm/tests/README.md`.
EM_JS(int, DonnerTestHooksEnabled, (), {
  try {
    return (new URLSearchParams(location.search).get("test") === "1") ? 1 : 0;
  } catch (e) {
    return 0;
  }
});

EM_JS(void, DonnerTestHooksInstall, (), {
  if (window.__donner_test_hooks_installed) {
    return;
  }
  window.__donner_test_hooks_installed = true;

  // Rolling frame-time ring (capacity 240 ~= 4s @ 60 FPS — enough for our
  // drag-perf window without unbounded growth).
  const kCapacity = 240;
  const frameTimesMs = new Float64Array(kCapacity);
  let frameTimesCount = 0;
  let frameTimesNext = 0;
  window.__donner_metrics = {
    // Arrays are rebuilt on read so callers get a stable snapshot.
    frameTimesMs: [],
    documentImageRect: null,
    selectionCount: 0,
    marqueeActive: false,
    compositedPreviewActive: false,
    flatBitmapUploads: 0,
    compositedPreviewBitmapUploads: 0,
    compositorFastPathCounters: {
      fastPathFrames: 0,
      slowPathFramesWithDirty: 0,
      noDirtyFrames: 0,
    },
    pollEventsCount: 0,
    renderCount: 0,
    skippedCount: 0,
    lastFrameTimestampMs: 0,
    readyTimestampMs: 0,
  };

  window.__donner_recordLoopTick = function(
      renderedFlag, frameTimeMs, fastPathFrames, slowPathFramesWithDirty, noDirtyFrames) {
    const m = window.__donner_metrics;
    m.pollEventsCount += 1;
    m.compositorFastPathCounters = {
      fastPathFrames,
      slowPathFramesWithDirty,
      noDirtyFrames,
    };
    if (renderedFlag) {
      m.renderCount += 1;
      if (frameTimeMs >= 0) {
        frameTimesMs[frameTimesNext] = frameTimeMs;
        frameTimesNext = (frameTimesNext + 1) % kCapacity;
        if (frameTimesCount < kCapacity) { frameTimesCount += 1; }
        // Rebuild the flat array so reads are O(n) on snapshot, not the hot path.
        const out = new Array(frameTimesCount);
        for (let i = 0; i < frameTimesCount; ++i) {
          const idx = (frameTimesNext - frameTimesCount + i + kCapacity) % kCapacity;
          out[i] = frameTimesMs[idx];
        }
        m.frameTimesMs = out;
      }
      m.lastFrameTimestampMs = performance.now();
    } else {
      m.skippedCount += 1;
    }
  };

  // Synthesized-event injection. The public payload uses pointer-like names
  // (`pointerdown`/`pointermove`/`pointerup`), but emscripten-glfw registers
  // DOM mouse listeners under the hood, so translate to MouseEvent before
  // dispatch. Programmatic PointerEvents do not generate compatibility mouse
  // events in Chromium.
  window.__donner_simulate = function(events) {
    const canvas = Module.canvas || document.getElementById("canvas");
    if (!canvas) { return false; }
    const rect = canvas.getBoundingClientRect();

    const makeMouseEvent = function(type, ev, button, buttons, bubbles) {
      return new MouseEvent(type, {
        bubbles,
        cancelable: true,
        composed: true,
        view: window,
        clientX: rect.left + ev.x,
        clientY: rect.top + ev.y,
        screenX: window.screenX + rect.left + ev.x,
        screenY: window.screenY + rect.top + ev.y,
        button,
        buttons,
        altKey: ev.altKey === true,
        ctrlKey: ev.ctrlKey === true,
        metaKey: ev.metaKey === true,
        shiftKey: ev.shiftKey === true,
      });
    };

    for (const ev of events) {
      const mouseType =
          ev.type === "pointerdown" ? "mousedown" :
          ev.type === "pointermove" ? "mousemove" :
          ev.type === "pointerup" ? "mouseup" :
          ev.type;
      const buttons = ev.buttons === undefined ? (mouseType === "mouseup" ? 0 : 1) : ev.buttons;

      if (mouseType === "mousedown") {
        canvas.dispatchEvent(makeMouseEvent("mouseenter", ev, 0, 0, false));
        canvas.dispatchEvent(makeMouseEvent("mousemove", ev, 0, 0, true));
        canvas.focus();
      }

      canvas.dispatchEvent(makeMouseEvent(mouseType, ev, 0, buttons, true));
    }
    return true;
  };
});

EM_JS(void, DonnerTestHooksMarkReady, (), {
  if (!window.__donner_test_hooks_installed) { return; }
  window.__donner_metrics.readyTimestampMs = performance.now();
  window.__donner_ready = true;
});

EM_JS(void, DonnerTestHooksRecordDocumentImageRect,
      (double left, double top, double right, double bottom), {
  if (!window.__donner_test_hooks_installed) { return; }
  window.__donner_metrics.documentImageRect = {
    left,
    top,
    right,
    bottom,
    width: right - left,
    height: bottom - top,
  };
});

EM_JS(void, DonnerTestHooksRecordSelectionState, (int selectionCount, int marqueeActive), {
  if (!window.__donner_test_hooks_installed) { return; }
  window.__donner_metrics.selectionCount = selectionCount;
  window.__donner_metrics.marqueeActive = marqueeActive !== 0;
});

EM_JS(void, DonnerTestHooksRecordUploadState,
      (int flatBitmapUploaded, int compositedPreviewActive, int compositedPreviewBitmapsUploaded), {
  if (!window.__donner_test_hooks_installed) { return; }
  const m = window.__donner_metrics;
  if (flatBitmapUploaded !== 0) {
    m.flatBitmapUploads += 1;
  }
  if (compositedPreviewBitmapsUploaded !== 0) {
    m.compositedPreviewBitmapUploads += 1;
  }
  m.compositedPreviewActive = compositedPreviewActive !== 0;
});

EM_JS(void, DonnerTestHooksRecordLoopTick,
      (int rendered, double frameTimeMs, double fastPathFrames, double slowPathFramesWithDirty,
       double noDirtyFrames),
      {
  if (!window.__donner_test_hooks_installed) { return; }
  window.__donner_recordLoopTick(
      rendered !== 0, frameTimeMs, fastPathFrames, slowPathFramesWithDirty, noDirtyFrames);
});
#endif
// clang-format on

/// Build a `TextEditor::ErrorMarkers` map from a parser diagnostic.
donner::editor::TextEditor::ErrorMarkers ParseErrorToMarkers(const donner::ParseDiagnostic& diag) {
  donner::editor::TextEditor::ErrorMarkers markers;
  const auto& start = diag.range.start;
  const int line = start.lineInfo.has_value() ? static_cast<int>(start.lineInfo->line) : 1;
  markers.emplace(line, std::string(std::string_view(diag.reason)));
  return markers;
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

struct TextureUploadState {
  GLuint texture = 0;
  int width = 0;
  int height = 0;
};

struct CompositedPreviewTextureState {
  TextureUploadState background;
  TextureUploadState promoted;
  TextureUploadState foreground;
  TextureUploadState overlay;
  donner::Vector2d promotedTranslationDoc = donner::Vector2d::Zero();
  bool active = false;
  bool texturesReady = false;
};

[[nodiscard]] bool ShouldPostDragMove(const donner::Vector2d& screenPoint,
                                      const std::optional<donner::Vector2d>& lastScreenPoint) {
  if (!lastScreenPoint.has_value()) {
    return true;
  }

  const double epsilonSquared = kDragMoveScreenEpsilonPx * kDragMoveScreenEpsilonPx;
  return (screenPoint - *lastScreenPoint).lengthSquared() > epsilonSquared;
}

void EnsureTextureInitialized(TextureUploadState& state) {
  if (state.texture != 0) {
    return;
  }
  glGenTextures(1, &state.texture);
  glBindTexture(GL_TEXTURE_2D, state.texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
}

bool UploadBitmapToTexture(const donner::svg::RendererBitmap& bitmap, TextureUploadState& state) {
  if (bitmap.empty()) {
    state.width = 0;
    state.height = 0;
    return false;
  }

  const uint8_t* pixels = bitmap.pixels.data();
  std::vector<uint8_t> straightAlphaPixels;
  if (bitmap.alphaType == donner::svg::AlphaType::Premultiplied) {
    straightAlphaPixels = bitmap.pixels;
    donner::svg::UnpremultiplyRgbaInPlace(straightAlphaPixels);
    pixels = straightAlphaPixels.data();
  }

  EnsureTextureInitialized(state);
  glBindTexture(GL_TEXTURE_2D, state.texture);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(bitmap.rowBytes / 4u));
  if (bitmap.dimensions.x == state.width && bitmap.dimensions.y == state.height) {
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, bitmap.dimensions.x, bitmap.dimensions.y, GL_RGBA,
                    GL_UNSIGNED_BYTE, pixels);
  } else {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, bitmap.dimensions.x, bitmap.dimensions.y, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, pixels);
  }
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  state.width = bitmap.dimensions.x;
  state.height = bitmap.dimensions.y;
  return true;
}

/// Process a FrameResult: upload bitmap to texture, apply writebacks to
/// text editor, update error markers. Returns true if a bitmap was uploaded.
bool ProcessFrameResult(const donner::editor::FrameResult& result, TextureUploadState& flatTexture,
                        CompositedPreviewTextureState& compositedPreview,
                        donner::editor::ViewportState& viewport,
                        donner::editor::TextEditor& textEditor, int& lastShownErrorLine,
                        std::string& lastShownErrorReason) {
  if (!result.ok) {
    return false;
  }

  bool bitmapUploaded = false;
  bool compositedPreviewBitmapsUploaded = false;
  if (result.compositedPreview.has_value()) {
    const auto& preview = *result.compositedPreview;
    if (preview.includesBitmapUploads) {
      const bool promotedUploaded =
          UploadBitmapToTexture(preview.promotedBitmap, compositedPreview.promoted);
      const bool overlayUploaded =
          preview.overlayBitmap.empty()
              ? true
              : UploadBitmapToTexture(preview.overlayBitmap, compositedPreview.overlay);
      (void)UploadBitmapToTexture(preview.backgroundBitmap, compositedPreview.background);
      (void)UploadBitmapToTexture(preview.foregroundBitmap, compositedPreview.foreground);
      compositedPreview.texturesReady = promotedUploaded && overlayUploaded;
      compositedPreviewBitmapsUploaded = promotedUploaded;
    }
    compositedPreview.promotedTranslationDoc = preview.promotedTranslationDoc;
    compositedPreview.active = preview.active && compositedPreview.texturesReady;
  } else {
    compositedPreview.active = false;
  }

  if (!result.bitmap.empty()) {
    // `glTexImage2D` reallocates the GPU-side texture storage on every
    // call, even when the dimensions haven't changed. On retina macOS
    // (1784×1024×4 = 7 MB/frame) that allocation has been measured at
    // ~15-20 ms, which caps the drag frame rate at 20-30 fps. When
    // dimensions are unchanged (the steady-state drag case — the
    // canvas is resized only via `setViewport`, not per-frame),
    // `glTexSubImage2D` reuses the existing storage and just pushes
    // pixel bytes, which drops the upload to sub-millisecond. Match
    // the outer allocation path on first-render and resize.
    bitmapUploaded = UploadBitmapToTexture(result.bitmap, flatTexture);
    compositedPreview.active = false;
    // Keep `documentViewBox` in step with the SVG's own viewBox as
    // reported by the backend. This is what makes screen↔document
    // math — `imageScreenRect`, `screenToDocument` (for pointer
    // hit-tests), `documentToScreen` (for selection chrome) — line
    // up with the actual bitmap that was just uploaded. Unlike the
    // rasterized bitmap dimensions, the document viewBox is invariant
    // under setViewport: zoom/pan don't change which doc coordinates
    // the user is clicking in, only how they map to screen pixels.
    if (result.documentViewBox.has_value()) {
      viewport.documentViewBox = *result.documentViewBox;
    }
  }

  // Apply source writebacks (e.g. from drag completion).
  for (const auto& wb : result.writebacks) {
    std::string source = textEditor.getText();
    if (wb.sourceStart <= source.size() && wb.sourceEnd <= source.size() &&
        wb.sourceStart <= wb.sourceEnd) {
      source.replace(wb.sourceStart, wb.sourceEnd - wb.sourceStart, wb.newText);
      textEditor.setText(source, /*preserveScroll=*/true);
    }
  }

  // Full source replacement (undo/redo).
  if (result.sourceReplaceAll.has_value()) {
    textEditor.setText(*result.sourceReplaceAll, /*preserveScroll=*/true);
  }

#ifdef __EMSCRIPTEN__
  DonnerTestHooksRecordSelectionState(static_cast<int>(result.selection.selections.size()),
                                      result.selection.marquee.has_value() ? 1 : 0);
  DonnerTestHooksRecordUploadState(bitmapUploaded ? 1 : 0, compositedPreview.active ? 1 : 0,
                                   compositedPreviewBitmapsUploaded ? 1 : 0);
#endif

  // Error markers.
  constexpr int kNoErrorLine = -1;
  if (!result.parseDiagnostics.empty()) {
    const auto& diag = result.parseDiagnostics.front();
    const int line = diag.range.start.lineInfo.has_value()
                         ? static_cast<int>(diag.range.start.lineInfo->line)
                         : 1;
    const std::string_view reasonSv = diag.reason;
    if (line != lastShownErrorLine || reasonSv != lastShownErrorReason) {
      textEditor.setErrorMarkers(ParseErrorToMarkers(diag));
      lastShownErrorLine = line;
      lastShownErrorReason.assign(reasonSv);
    }
  } else if (lastShownErrorLine != kNoErrorLine) {
    textEditor.setErrorMarkers({});
    lastShownErrorLine = kNoErrorLine;
    lastShownErrorReason.clear();
  }

  return bitmapUploaded;
}

}  // namespace

int main(int argc, char** argv) {
  if (const char* bwd = std::getenv("BUILD_WORKING_DIRECTORY")) {
    std::filesystem::current_path(bwd);
  }

  // CLI shape matches the composited-rendering editor's main.cc on
  // `--experimental` is a reserved CLI slot. Composited drag preview
  // is already on-by-default in the sandbox thin-client, so this
  // flag doesn't gate *rendering* behavior — but it does enable
  // **drag instrumentation**: per-event stderr logging of
  // pointer-event → backend → texture-upload wall-clock, so a
  // developer reporting "drag is laggy" can paste the output and
  // tell us which stage is the bottleneck. Always-on instrumentation
  // would clutter normal launches; hiding it behind `--experimental`
  // keeps the steady-state launch quiet.
  bool experimentalMode = false;
  // `--save-repro <path>`: install a host-side `ReproRecorder` that
  // captures raw ImGui input state once per frame and writes the
  // recording out on exit. Replay via `donner-editor` + the repro
  // binary (decodes the file and re-drives the editor).
  std::optional<std::string> reproOutputPath;

#ifdef __EMSCRIPTEN__
  // Opt-in Playwright test harness hooks (`?test=1` in the URL). Installing
  // here (before GLFW / ImGui / canvas attach) means `window.__donner_*` is
  // visible to the test runner from the first tick; `__donner_ready` flips
  // to true once the first frame has been swapped (see the main loop).
  const bool testHooksEnabled = DonnerTestHooksEnabled() != 0;
  if (testHooksEnabled) {
    DonnerTestHooksInstall();
  }
  const std::string initialSource = EmbeddedBytesToString(donner::embedded::kEditorIconSvg);
  const std::optional<std::string> initialPath = std::string("donner_icon.svg");
#else
  constexpr std::string_view kUsage =
      "Usage: donner-editor [--experimental] [--save-repro <path>] <filename>\n";
  std::optional<std::string> svgPath;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--experimental") {
      experimentalMode = true;
      continue;
    }
    if (arg == "--save-repro") {
      if (i + 1 >= argc) {
        std::cerr << "--save-repro requires a filename argument\n" << kUsage;
        return 1;
      }
      reproOutputPath = std::string(argv[++i]);
      continue;
    }
    if (svgPath.has_value()) {
      std::cerr << kUsage;
      return 1;
    }
    svgPath = std::string(arg);
  }
  if (!svgPath.has_value()) {
    std::cerr << kUsage;
    return 1;
  }

  const std::string initialSource = LoadFile(*svgPath);
  if (initialSource.empty()) {
    return 1;
  }
  const std::optional<std::string> initialPath = *svgPath;
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
  io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  io.DisplayFramebufferScale =
      ImVec2(static_cast<float>(initialDisplayScale), static_cast<float>(initialDisplayScale));
  io.FontGlobalScale = static_cast<float>(1.0 / initialDisplayScale);
#ifdef __EMSCRIPTEN__
  // The imgui_impl_glfw backend auto-sets ConfigMacOSXBehaviors from
  // `__APPLE__` at compile time, which is false for the WASM build even
  // when the user's browser is on macOS. Without this flag ImGui routes
  // `ImGuiMod_Shortcut` to Ctrl, so Cmd-A / Cmd-C / Cmd-V never reach
  // InputText's clipboard shortcuts on Mac browsers. Ask emscripten-glfw
  // what the runtime platform is instead.
  io.ConfigMacOSXBehaviors = emscripten_glfw_is_runtime_platform_apple() != 0;
#endif
  ImGui::StyleColorsDark();
  ImGui_ImplGlfw_InitForOpenGL(window, true);
#ifdef __EMSCRIPTEN__
  ImGui_ImplOpenGL3_Init("#version 300 es");
  ImGui_ImplGlfw_InstallEmscriptenCallbacks(window, "#canvas");
#else
  ImGui_ImplOpenGL3_Init("#version 330");
#endif

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
  using SteadyClock = std::chrono::steady_clock;
  const auto editorStart = SteadyClock::now();
  const auto msSinceStart = [&editorStart]() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - editorStart)
        .count();
  };

  // Create the backend client. The subprocess-sandboxed client only exists on
  // Linux (see BUILD.bazel's select on `editor_backend_client_session`);
  // everything else — WASM and macOS desktops — falls back to the in-process
  // client, which statically links the backend core into this address space.
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
  donner::editor::sandbox::SandboxSession session(donner::editor::sandbox::SandboxSessionOptions{
      .childBinaryPath = "donner/editor/sandbox/donner_editor_backend",
  });
  auto backend = donner::editor::EditorBackendClient::MakeSessionBacked(session);
#else
  auto backend = donner::editor::EditorBackendClient::MakeInProcess();
#endif

  // Offer a shared-GPU-texture handle to the backend before the first
  // render. Today only macOS ships a real factory (via `IOSurface`);
  // everything else falls through to the CPU stub. The backend's
  // `BridgeTextureBackend::ready()` currently returns `false` on every
  // platform (wgpu-native doesn't yet export the Metal-texture import
  // — see `docs/design_docs/0023-editor_sandbox.md` §"Cross-process
  // texture bridging"), so engaging this doesn't yet skip the CPU
  // `finalBitmapPixels` wire path. The plumbing is in place so the
  // switchover to zero-copy is a one-line behavior change once the
  // Rust-side wgpu-native patch lands.
#if defined(__APPLE__) && !defined(__EMSCRIPTEN__)
  std::unique_ptr<donner::editor::sandbox::bridge::BridgeTextureHost> sharedTextureHost;
  {
    int fbW = 0;
    int fbH = 0;
    glfwGetFramebufferSize(window, &fbW, &fbH);
    if (fbW > 0 && fbH > 0) {
      sharedTextureHost =
          donner::editor::sandbox::bridge::MakeHost_macOS(donner::Vector2i(fbW, fbH));
      if (sharedTextureHost) {
        auto attachFuture = backend->attachSharedTexture(sharedTextureHost->handle());
        (void)attachFuture.get();
        std::cerr << "[startup] +" << msSinceStart() << "ms attached shared IOSurface (" << fbW
                  << "x" << fbH << ")\n";
      }
    }
  }
#endif

  // Address bar stack. The widget is shared between desktop and WASM; the
  // fetcher underneath differs (desktop: `SvgSource` + curl for http(s);
  // WASM: `emscripten_fetch` with the browser as the sandbox). See
  // `docs/design_docs/0023-editor_sandbox.md` §S10/§S11.
#ifndef __EMSCRIPTEN__
  donner::editor::sandbox::SvgSource svgSource;
  donner::editor::ResourceGatekeeper resourceGatekeeper(donner::editor::DefaultDesktopPolicy());
  // `autoGrantFirstUse = true`: a URL typed into the address bar is the
  // user's consent to hit that host. No first-use prompt fires for
  // direct URL entry. The gatekeeper still enforces scheme / deny-list /
  // size + timeout limits — it just treats the typed URL as the gesture
  // that would otherwise require an explicit click-through.
  auto fetcher = donner::editor::MakeDesktopFetcher(resourceGatekeeper, svgSource,
                                                    /*autoGrantFirstUse=*/true);
#else
  auto fetcher = donner::editor::MakeWasmFetcher();
#endif
  donner::editor::AddressBar addressBar;
  const std::string initialDisplayUri =
      initialPath.has_value()
          ? donner::editor::PrettifyLocalPath(*initialPath, std::filesystem::current_path())
          : std::string();
  if (initialPath.has_value()) {
    addressBar.setInitialUri(initialDisplayUri);
  }
  // The `AddressBarDispatcher` that drives this widget is constructed further
  // down, once the `loadBytesIntoDocument` helper it needs is in scope.

  // Load the initial document.
  {
    auto loadFuture = backend->loadBytes(
        std::span(reinterpret_cast<const uint8_t*>(initialSource.data()), initialSource.size()),
        initialPath);
    auto loadResult = loadFuture.get();
    if (!loadResult.ok) {
      std::cerr << "Failed to parse initial SVG";
      if (initialPath.has_value()) {
        std::cerr << ": " << *initialPath;
      }
      std::cerr << "\n";
    }
    addressBar.setStatus({donner::editor::AddressBarStatus::kRendered,
                          loadResult.ok ? "OK" : "Parse error", initialDisplayUri});
  }
  std::cerr << "[startup] +" << msSinceStart() << "ms loadBytes done\n";

  donner::editor::TextEditor textEditor;
  textEditor.setLanguageDefinition(donner::editor::TextEditor::LanguageDefinition::SVG());
  textEditor.setText(initialSource);
  textEditor.resetTextChanged();
  textEditor.setActiveAutocomplete(true);
  std::string editorNoticeText = EmbeddedBytesToString(donner::embedded::kEditorNoticeText);

  TextureUploadState flatTexture;
  EnsureTextureInitialized(flatTexture);
  CompositedPreviewTextureState compositedPreview;

  // Upload the initial frame bitmap if available.
  {
    const auto& bitmap = backend->latestBitmap();
    if (!bitmap.empty()) {
      UploadBitmapToTexture(bitmap, flatTexture);
    }
  }

  // Seed `viewport.documentViewBox` from the initial bitmap so that
  // `imageScreenRect() = documentToScreen(documentViewBox)` returns a
  // non-zero rect — otherwise `ImGui::AddImage` targets a 0×0 box and
  // nothing appears on screen even though the texture is correctly
  // uploaded. On the main branch this wiring lived in `EditorShell`
  // via `ViewportState::setDocumentBounds(...)`; the thin-client
  // branch (S7–S11) doesn't carry it yet, and mistakenly leaves
  // `documentViewBox` at its zero-initialized default. S12 follow-up
  // will plumb the SVG's own viewBox through from the backend so
  // zoom-to-fit / aspect-preservation semantics match main — for now
  // we seed from the rendered bitmap dimensions, which is correct
  // for 1:1 / pane-fill rendering. Seeded below, right after the
  // viewport is declared.

  // Surface initial parse error.
  constexpr int kNoErrorLine = -1;
  int lastShownErrorLine = kNoErrorLine;
  std::string lastShownErrorReason;
  if (auto initialErr = backend->lastParseError(); initialErr.has_value()) {
    textEditor.setErrorMarkers(ParseErrorToMarkers(*initialErr));
    lastShownErrorLine = initialErr->range.start.lineInfo.has_value()
                             ? static_cast<int>(initialErr->range.start.lineInfo->line)
                             : 1;
    lastShownErrorReason = std::string(std::string_view(initialErr->reason));
  }

  donner::editor::ViewportState viewport;
  // Seed `documentViewBox` from the backend — this is the SVG's user-
  // space coordinate system, the space every bbox and pointer event
  // travels in. Must be populated *before* `resetTo100Percent` (in
  // the main loop below) so `documentViewBoxCenter()` returns the
  // true center, not (0, 0). Using the backend's reported viewBox
  // (rather than the rasterized bitmap's dims) is what makes
  // `screenToDocument` yield usable coordinates — otherwise
  // hit-tests land at 1-to-bitmap-pixel coords, which aren't doc
  // coords at all on any document whose intrinsic width/height
  // differs from the rasterized canvas.
  if (auto vb = backend->latestDocumentViewBox()) {
    viewport.documentViewBox = *vb;
  }
  bool viewportInitialized = false;

  bool panning = false;
  ImVec2 lastPanMouse(0.0f, 0.0f);
  FrameHistory frameHistory;

  // Pending async results from backend.
  std::optional<std::future<donner::editor::FrameResult>> pendingFrame;
  std::optional<donner::Vector2d> lastPostedDragMoveScreenPoint;

  // Tracks the last canvas size we told the backend about, so we only
  // re-post `setViewport` when the UI pane actually resizes rather than
  // on every main-loop tick. `(-1, -1)` forces a post on the first
  // usable-pane frame (matches the initial implicit render from
  // `loadBytes`).
  donner::Vector2i lastPostedCanvasSize(-1, -1);

  // Text change debounce state.
  bool textChangePending = false;
  bool textDispatchThrottled = false;
  float textChangeIdleTimer = 0.0f;

  // Track current file path for title bar.
  std::optional<std::string> currentFilePath = initialPath;
  bool documentDirty = false;

  std::string lastWindowTitle;
  bool dockspaceInitialized = false;
  bool openFileModalRequested = false;
  std::array<char, 4096> openFilePathBuffer{};
  std::string openFileError;

  // Install the repro recorder if `--save-repro` was passed. Held in
  // an `optional` so the steady-state non-recording case skips the
  // branch overhead entirely. `snapshotFrame()` is called once per
  // frame (right after `ImGui::NewFrame` below); `flush()` runs in
  // the shutdown path so Ctrl-C / kill-9 lose at most the in-memory
  // tail — flushing on every frame would dominate main-loop cost.
  std::optional<donner::editor::repro::ReproRecorder> reproRecorder;
#ifndef __EMSCRIPTEN__
  if (reproOutputPath.has_value()) {
    int winW = 0;
    int winH = 0;
    glfwGetWindowSize(window, &winW, &winH);
    float xScale = 1.0f;
    float yScale = 1.0f;
    glfwGetWindowContentScale(window, &xScale, &yScale);
    donner::editor::repro::ReproRecorderOptions options;
    options.outputPath = *reproOutputPath;
    options.svgPath = *svgPath;
    options.windowWidth = winW;
    options.windowHeight = winH;
    options.displayScale = std::max(1.0, static_cast<double>(xScale));
    options.experimentalMode = experimentalMode;
    reproRecorder.emplace(std::move(options));
    std::cerr << "[repro] recording UI inputs to " << *reproOutputPath << "\n";
  }
#endif

  // Core loader. Used by three entry points: the File → Open modal, the
  // WASM file-picker / drop shortcut, and the address-bar dispatcher. Takes
  // already-in-memory bytes plus the origin URI (what the user typed or
  // dropped) and optional resolved path (for title bar + file-watcher
  // affordances; empty on HTTP loads). Returns true when the backend
  // accepted the parse. Declared above the main loop so the dispatcher can
  // hold a stable reference to it across frames.
  const std::function<bool(const std::string&, std::span<const uint8_t>,
                           const std::optional<std::string>&)>
      loadBytesIntoDocument = [&](const std::string& originUri, std::span<const uint8_t> bytes,
                                  const std::optional<std::string>& resolvedPath) -> bool {
    std::cerr << "[load] bytes=" << bytes.size() << " uri=" << originUri << "\n";

    // Pre-filter: if the bytes obviously aren't SVG (HTML page, JSON,
    // empty body), fail BEFORE touching the backend. The parser can
    // otherwise accept non-SVG XML-shaped content as a "successful"
    // empty document, which silently overwrites `textEditor` with the
    // non-SVG source and leaves the previous render frozen on screen
    // — user sees HTML in the editor pane and the old SVG in the
    // canvas with no visible error. Sniffing first keeps both the
    // document source and the rendering intact on failure.
    if (auto sniff = donner::editor::DescribeNonSvgBytes(bytes); sniff.has_value()) {
      openFileError = *sniff;
      addressBar.setStatus({donner::editor::AddressBarStatus::kParseError, *sniff, originUri});
      std::cerr << "[load] rejected (sniffer): " << originUri << " — " << *sniff << "\n";
      return false;
    }

    auto loadFuture = backend->loadBytes(bytes, resolvedPath.has_value()
                                                    ? std::optional<std::string>(*resolvedPath)
                                                    : std::optional<std::string>(originUri));
    auto loadResult = loadFuture.get();
    if (!loadResult.ok) {
      // Parser-level failure: XML-shaped bytes that didn't parse as SVG.
      // Surface the first diagnostic with its line number so the user
      // can jump to the offending spot in the text editor.
      std::string errorMsg;
      if (!loadResult.parseDiagnostics.empty()) {
        const auto& diag = loadResult.parseDiagnostics.front();
        errorMsg = "Parse error: ";
        errorMsg.append(std::string_view(diag.reason));
        if (diag.range.start.lineInfo.has_value()) {
          errorMsg += " (line ";
          errorMsg += std::to_string(diag.range.start.lineInfo->line);
          errorMsg += ")";
        }
      } else {
        errorMsg = "Failed to parse SVG.";
      }
      openFileError = errorMsg;
      addressBar.setStatus({donner::editor::AddressBarStatus::kParseError, errorMsg, originUri});
      std::cerr << "[load] parse failed: " << originUri << " — " << errorMsg << "\n";
      return false;
    }

    const std::string contents(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    textEditor.setText(contents);
    textEditor.resetTextChanged();
    // Display-friendly form: files inside the editor's cwd read as
    // `./donner_splash.svg`, not the full absolute path. Remote URIs
    // and paths outside cwd pass through unchanged.
    const std::string displayUri =
        donner::editor::PrettifyLocalPath(originUri, std::filesystem::current_path());
    currentFilePath = resolvedPath.has_value() ? *resolvedPath : displayUri;
    documentDirty = false;
    textChangePending = false;
    textDispatchThrottled = false;
    textChangeIdleTimer = 0.0f;
    pendingFrame.reset();
    openFileError.clear();
    addressBar.setInitialUri(displayUri);
    addressBar.setStatus({donner::editor::AddressBarStatus::kRendered, "OK", displayUri});
    // Re-center the viewport for the new document. Without this, the old
    // zoom/pan leaks across documents and the new SVG (which may have
    // very different viewBox dimensions) can render offscreen or at a
    // confusing scale. The flag is consumed by the `if
    // (!viewportInitialized && renderPaneUsable) { resetTo100Percent(); }`
    // block in the main loop; we clear it here so the next stable-pane
    // frame re-centers once the backend's new `documentViewBox` has
    // flowed through `ProcessFrameResult`.
    viewportInitialized = false;
    // Also invalidate the last-posted canvas size so `setViewport` fires
    // on the next loop iteration for the new document dimensions.
    lastPostedCanvasSize = donner::Vector2i(-1, -1);

    ProcessFrameResult(loadResult, flatTexture, compositedPreview, viewport, textEditor,
                       lastShownErrorLine, lastShownErrorReason);
    std::cerr << "[load] ok uri=" << originUri << "\n";
    return true;
  };

  donner::editor::AddressBarDispatcher addressBarDispatcher(
      addressBar, *fetcher, [&](const donner::editor::AddressBarLoadRequest& r) {
        (void)loadBytesIntoDocument(
            r.originUri, std::span<const uint8_t>(r.bytes.data(), r.bytes.size()), r.resolvedPath);
      });

  std::cerr << "[startup] +" << msSinceStart() << "ms entering main loop\n";

  // ---------------------------------------------------------------------------
  // Main loop
  // ---------------------------------------------------------------------------
  //
  // On-demand render loop (restored from commit 07dcd4e0).
  //
  // On desktop we block inside `glfwWaitEventsTimeout` while nothing is
  // happening (no drag, no async frame in flight, no debounced source
  // change, no address-bar fetch) so the editor idles at ~0 FPS instead
  // of burning a full 60/120 FPS worth of GL + ImGui + frame-graph
  // churn. Any mouse / keyboard / focus event wakes the timeout, and
  // the pinch monitor / any explicit `glfwPostEmptyEvent()` does the
  // same for event sources that live outside GLFW's own callback
  // surface. A generous 1-second idle timeout still lets slow polling
  // work (stalled futures, pending address-bar fetch on WASM where the
  // completion callback reaches us on a later tick).
  //
  // On Emscripten `glfwWaitEvents*` and `glfwPostEmptyEvent` are no-ops
  // (the browser drives the loop via `requestAnimationFrame`), so the
  // equivalent idle path is: skip the render/swap path and don't push
  // a sample into the frame-time ring. The RAF tick still fires, but
  // we avoid pushing the canvas + the FPS graph forward.
#ifndef __EMSCRIPTEN__
  constexpr double kIdleWaitTimeoutSeconds = 1.0;
  // Cursor-blink cadence for the source pane — re-render every ~200ms
  // while the text editor has focus so the blink cycle (800ms period,
  // 50% duty) stays animated. Not a full-FPS loop — the waitEvents
  // timeout gates how often we wake, and the blink only needs a few
  // frames per second.
  constexpr double kCursorBlinkWakeSeconds = 0.2;
#endif
  // Set to true at the bottom of each iteration when ImGui's post-frame
  // signals indicate continuing work (active drag, popup open, input
  // focus with blinking cursor). Read at the top of the next iteration
  // so we skip the `waitEventsTimeout` block. Starts true so the first
  // iteration always renders.
  bool lastFrameWasActive = true;
  // Independent of `lastFrameWasActive`: true when the only reason to
  // keep rendering is the cursor blink. Uses a short timeout instead of
  // full-speed polling — the editor idles between blinks.
#ifndef __EMSCRIPTEN__
  bool lastFrameNeededBlinkWake = false;
#endif
  while (!glfwWindowShouldClose(window)) {
    ZoneScopedN("main_loop");

#ifdef __EMSCRIPTEN__
    // On WASM emscripten-glfw updates its cached input state during
    // pollEvents callback dispatch — there's no "live OS query" path
    // the way desktop GLFW has. If we read `glfwGetMouseButton` BEFORE
    // polling, `needsActive` below decides based on last iteration's
    // state, which stutters drag start/end by a full iteration each
    // way (and with ASYNCIFY's `emscripten_sleep(0)` round-trips that's
    // a visible lag). Poll first so every state read below sees fresh
    // data. On desktop the idle path is `glfwWaitEventsTimeout` further
    // down, which inherently drains events on wake, so the equivalent
    // hoist isn't needed there.
    glfwPollEvents();
#endif

    // Compute whether anything time-dependent is happening BEFORE we
    // decide between poll/wait. Signals read here are either external
    // to ImGui (futures, dispatcher state, the repro recorder) or
    // physical input state via direct GLFW queries (mouse buttons,
    // space-to-pan). ImGui-derived focus/drag signals come from the
    // previous frame via `lastFrameWasActive`.
    const bool leftMouseHeld = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    const bool middleMouseHeld = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;
    const bool rightMouseHeld = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
    const bool spaceHeldNow = glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS;
    // ImGui's own mouse-down state is updated synchronously from the
    // same emscripten-glfw callbacks, and catches the case where our
    // direct `glfwGetMouseButton` read is still the one-iteration-stale
    // value (can happen on the first loop iteration after a press, or
    // when the browser coalesces pointer events). Treated as
    // belt-and-suspenders — either signal indicates "user is actively
    // clicking, keep polling at full speed".
    const bool imguiMouseDown = ImGui::IsAnyMouseDown();
    const bool mouseOrModHeld =
        leftMouseHeld || middleMouseHeld || rightMouseHeld || spaceHeldNow || imguiMouseDown;
    // Text-change debounce must keep ticking without user input —
    // `textDispatchThrottled` stays true from first keystroke through
    // the debounce expiry, gating the "send the tail edit" replay. If
    // we went idle mid-debounce the delayed replay would wait forever.
    const bool debounceTicking =
        textChangePending || textDispatchThrottled || textChangeIdleTimer > 0.0f;
    const bool fetchAnimating =
        addressBar.isLoadingAnimationActive() || addressBarDispatcher.isFetchInFlight();
    const bool scrollEventsQueued = !pendingScrollEvents.events.empty();
    const bool reproActive = reproRecorder.has_value();
    // Startup and document-load viewport sync needs at least one render-pane
    // frame with a settled non-zero content size. On WASM, going idle before
    // that frame means the loop stops recomputing layout until an input event
    // (usually a click) wakes it, which makes page load look stuck.
    const bool viewportSyncPending =
        !viewportInitialized || lastPostedCanvasSize.x < 0 || lastPostedCanvasSize.y < 0;
#ifdef __EMSCRIPTEN__
    const bool browserUploadPending = gPendingBrowserUploadPath.has_value();
#else
    constexpr bool browserUploadPending = false;
#endif
    // pendingFrame forces a poll so the future completion (handoff
    // from the backend/worker thread) is observed without waiting out
    // the idle timeout. The initial `loadBytes` path runs synchronously
    // below before the loop starts, so steady-state `pendingFrame`
    // values only come from the input-driven or viewport-resize paths.
    const bool needsActive = pendingFrame.has_value() || mouseOrModHeld || debounceTicking ||
                             fetchAnimating || scrollEventsQueued || reproActive ||
                             viewportSyncPending || browserUploadPending || lastFrameWasActive;

#ifndef __EMSCRIPTEN__
    // `enteredIdlePath` means this iteration unblocked via the idle
    // wait (timeout or wake from empty-event). We still run the frame
    // body — any wake may have processed input that needs a re-paint —
    // but skip the frame-time graph sample so the FPS counter shows
    // a quiescent low reading instead of a 1000ms "spike" every idle
    // tick.
    bool enteredIdlePath = false;
    if (needsActive) {
      ZoneScopedN("pollEvents");
      glfwPollEvents();
    } else if (lastFrameNeededBlinkWake) {
      ZoneScopedN("waitEventsTimeout_blink");
      glfwWaitEventsTimeout(kCursorBlinkWakeSeconds);
      enteredIdlePath = true;
    } else {
      ZoneScopedN("waitEventsTimeout");
      glfwWaitEventsTimeout(kIdleWaitTimeoutSeconds);
      enteredIdlePath = true;
    }
    if (experimentalMode) {
      static bool lastReportedIdle = false;
      if (enteredIdlePath != lastReportedIdle) {
        std::fprintf(stderr, "[loop] %s\n",
                     enteredIdlePath ? "idle (waitEvents)" : "active (pollEvents)");
        lastReportedIdle = enteredIdlePath;
      }
    }
#else
    // WASM: waitEventsTimeout is a no-op; the browser runs us at RAF
    // cadence regardless. Drop the frame entirely on idle (and on idle-
    // with-cursor-blink — the browser's own animation frame cadence
    // takes care of blink cadence for us) so the FPS counter doesn't
    // tick. Re-rendering a blink at 60 FPS on idle would defeat the
    // whole point of the on-demand loop.
    //
    // `glfwPollEvents()` already fired at the TOP of this iteration —
    // see the `#ifdef __EMSCRIPTEN__` block right after the `while`
    // header — so `needsActive` above was computed against fresh state.
    if (!needsActive) {
      // Still yield so emscripten's ASYNCIFY doesn't starve the
      // browser event loop (would manifest as the canvas freezing
      // even though we're doing "nothing").
      if (testHooksEnabled) {
        // `rendered=0` so test harness sees the skip cadence, not a frame.
        const auto counters = backend->compositorFastPathCountersForTesting();
        DonnerTestHooksRecordLoopTick(0, -1.0, static_cast<double>(counters.fastPathFrames),
                                      static_cast<double>(counters.slowPathFramesWithDirty),
                                      static_cast<double>(counters.noDirtyFrames));
      }
      emscripten_sleep(0);
      continue;
    }
    constexpr bool enteredIdlePath = false;
#endif

    if (!enteredIdlePath) {
      frameHistory.push(ImGui::GetIO().DeltaTime * 1000.0f);
    }

    // Window title.
    {
      std::string title;
      if (documentDirty) {
        title += "● ";
      }
      if (currentFilePath.has_value()) {
        title += std::filesystem::path(*currentFilePath).filename().string();
      } else {
        title += "untitled";
      }
      title += " — Donner SVG Editor";
      if (title != lastWindowTitle) {
        glfwSetWindowTitle(window, title.c_str());
        lastWindowTitle = std::move(title);
      }
    }

    // Poll pending async frame results.
    if (pendingFrame.has_value()) {
      if (pendingFrame->wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const auto processStart = std::chrono::steady_clock::now();
        auto result = pendingFrame->get();
        const auto gotResult = std::chrono::steady_clock::now();
        ProcessFrameResult(result, flatTexture, compositedPreview, viewport, textEditor,
                           lastShownErrorLine, lastShownErrorReason);
        const auto processEnd = std::chrono::steady_clock::now();
        pendingFrame.reset();
        if (experimentalMode) {
          const double getMs =
              std::chrono::duration<double, std::milli>(gotResult - processStart).count();
          const double uploadMs =
              std::chrono::duration<double, std::milli>(processEnd - gotResult).count();
          const std::size_t pixels =
              static_cast<std::size_t>(result.bitmap.dimensions.x * result.bitmap.dimensions.y);
          std::fprintf(stderr,
                       "[drag] frame=%" PRIu64
                       " get=%.3f ms upload+writebacks=%.3f ms (%dx%d = %zu px)\n",
                       result.frameId, getMs, uploadMs, result.bitmap.dimensions.x,
                       result.bitmap.dimensions.y, pixels);
        }
      }
    }

    // Also sync error markers from cached backend state (for latest frame).
    {
      const auto parseError = backend->lastParseError();
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

    // Snapshot must happen AFTER `ImGui::NewFrame` (so ImGuiIO is
    // populated with this frame's input state) but BEFORE any UI
    // widget code consumes those events — reading IsMouseClicked
    // from a widget marks the event "used", which affects later
    // frame-delta calculations but not the raw state the recorder
    // captures. Placing it here keeps the recording layer below
    // any UI dispatch.
    if (reproRecorder.has_value()) {
      // Feed the recorder the live viewport state and a document-space
      // mouse coord so the recording is replayable without
      // reconstructing screen→doc math from hand-tuned pane-layout
      // constants. `viewport` here is last-frame's layout — ImGui's
      // input for frame N fires against frame N-1's layout, so using
      // last-frame's viewport correctly maps the current mouse events.
      donner::editor::repro::FrameContext reproContext;
      if (viewport.paneSize.x > 0.0 && viewport.paneSize.y > 0.0) {
        donner::editor::repro::ReproViewport vp;
        vp.paneOriginX = viewport.paneOrigin.x;
        vp.paneOriginY = viewport.paneOrigin.y;
        vp.paneSizeW = viewport.paneSize.x;
        vp.paneSizeH = viewport.paneSize.y;
        vp.devicePixelRatio = viewport.devicePixelRatio;
        vp.zoom = viewport.zoom;
        vp.panDocX = viewport.panDocPoint.x;
        vp.panDocY = viewport.panDocPoint.y;
        vp.panScreenX = viewport.panScreenPoint.x;
        vp.panScreenY = viewport.panScreenPoint.y;
        vp.viewBoxX = viewport.documentViewBox.topLeft.x;
        vp.viewBoxY = viewport.documentViewBox.topLeft.y;
        vp.viewBoxW = viewport.documentViewBox.width();
        vp.viewBoxH = viewport.documentViewBox.height();
        reproContext.viewport = vp;

        const ImVec2 mouse = io.MousePos;
        const donner::Vector2d mouseScreen(mouse.x, mouse.y);
        const bool insidePane = mouseScreen.x >= viewport.paneOrigin.x &&
                                mouseScreen.x <= viewport.paneOrigin.x + viewport.paneSize.x &&
                                mouseScreen.y >= viewport.paneOrigin.y &&
                                mouseScreen.y <= viewport.paneOrigin.y + viewport.paneSize.y;
        if (insidePane) {
          const donner::Vector2d docPoint = viewport.screenToDocument(mouseScreen);
          reproContext.mouseDoc = std::make_pair(docPoint.x, docPoint.y);
        }
        // No hit-tester wired: the thin-client main.cc frontend
        // dispatches pointer events to an out-of-process backend and
        // has no synchronous hit-test API. The `hit` field is
        // diagnostic-only anyway (see `ReproHit` in `ReproFile.h`),
        // so replay works fine without it.
      }
      reproRecorder->snapshotFrame(reproContext);
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

      return loadBytesIntoDocument(
          path.string(),
          std::span(reinterpret_cast<const uint8_t*>(contents.data()), contents.size()),
          path.string());
    };

#ifdef __EMSCRIPTEN__
    if (gPendingBrowserUploadPath.has_value()) {
      const std::string path = *gPendingBrowserUploadPath;
      gPendingBrowserUploadPath.reset();
      (void)tryOpenPath(path);
    }
#endif

    // Pump the address bar. The dispatcher itself is defined above the main
    // loop so it isn't reconstructed each frame; here we only service it.
    addressBarDispatcher.pump();

    const auto triggerOpen = [&]() {
#ifdef __EMSCRIPTEN__
      ShowBrowserOpenFileDialog(kMaxWasmUploadBytes);
#else
      std::fill(openFilePathBuffer.begin(), openFilePathBuffer.end(), '\0');
      if (currentFilePath.has_value()) {
        std::strncpy(openFilePathBuffer.data(), currentFilePath->c_str(),
                     openFilePathBuffer.size() - 1);
      }
      openFileError.clear();
      openFileModalRequested = true;
#endif
    };
    const auto triggerUndo = [&]() {
      auto future = backend->undo();
      auto result = future.get();
      ProcessFrameResult(result, flatTexture, compositedPreview, viewport, textEditor,
                         lastShownErrorLine, lastShownErrorReason);
    };
    const auto triggerRedo = [&]() {
      auto future = backend->redo();
      auto result = future.get();
      ProcessFrameResult(result, flatTexture, compositedPreview, viewport, textEditor,
                         lastShownErrorLine, lastShownErrorReason);
    };
    const auto triggerQuit = [&]() { glfwSetWindowShouldClose(window, GLFW_TRUE); };
    const auto triggerZoomIn = [&]() { applyZoom(kKeyboardZoomStep, viewport.paneCenter()); };
    const auto triggerZoomOut = [&]() {
      applyZoom(1.0 / kKeyboardZoomStep, viewport.paneCenter());
    };
    const auto triggerActualSize = [&]() { viewport.resetTo100Percent(); };

    const bool sourcePaneFocused = textEditor.isFocused();
    bool openAboutPopup = false;
    bool openLicensesPopup = false;

    // Global keyboard shortcuts.
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

      // Escape: clear selection via a no-op pointer event at an impossible location.
      if (!anyPopupOpen && ImGui::IsKeyPressed(ImGuiKey_Escape, /*repeat=*/false)) {
        // Deselect by sending a click to void space (-9999, -9999).
        pendingFrame = backend->pointerEvent(donner::editor::PointerEventPayload{
            .phase = donner::editor::sandbox::PointerPhase::kDown,
            .documentPoint = donner::Vector2d(-9999.0, -9999.0),
        });
      }

      // Delete / Backspace: send key event to backend.
      const bool deleteKey = ImGui::IsKeyPressed(ImGuiKey_Delete, /*repeat=*/false) ||
                             ImGui::IsKeyPressed(ImGuiKey_Backspace, /*repeat=*/false);
      if (deleteKey && !anyPopupOpen && !ImGui::GetIO().WantCaptureKeyboard) {
        pendingFrame = backend->keyEvent(donner::editor::KeyEventPayload{
            .phase = donner::editor::sandbox::KeyPhase::kDown,
            .keyCode = GLFW_KEY_DELETE,
        });
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
        ImGui::EndMenu();
      }

      if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Cmd+Z")) {
          triggerUndo();
        }
        if (ImGui::MenuItem("Redo", "Cmd+Shift+Z")) {
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

    // Address bar lives above the dockspace so every pane reads the same
    // URL context. Dispatcher is pumped above (before this frame's UI);
    // drawing here renders this frame's state and captures navigations
    // that fire on Enter / Load. Pumped again next frame.
    {
      ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 4.0f));
      ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f));
      (void)addressBar.draw();
      ImGui::PopStyleVar(2);
      ImGui::Separator();
    }

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

    // --- Source pane ---
    ImGui::SetNextWindowClass(&windowClassNoUndocking);
    ImGui::Begin("Source");
    ImGui::PushFont(codeFont);
    textEditor.render("##source");
    ImGui::PopFont();

    // Text change dispatch (leading-edge + trailing debounce).
    const auto dispatchTextChange = [&]() {
      const std::string newSource = textEditor.getText();
      documentDirty = true;
      // Fire-and-forget: send to backend, next frame picks up result.
      pendingFrame = backend->replaceSource(std::string(newSource), false);
    };

    if (textEditor.isTextChanged()) {
      textEditor.resetTextChanged();
      documentDirty = true;

      if (!textDispatchThrottled) {
        dispatchTextChange();
        textDispatchThrottled = true;
        textChangePending = false;
      } else {
        textChangePending = true;
      }
      textChangeIdleTimer = 0.0f;
    } else if (textDispatchThrottled) {
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

    // --- Render pane ---
    ImGui::SetNextWindowClass(&windowClassNoUndocking);
    ImGui::Begin("Render");

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

    const bool renderPaneUsable = contentRegion.x > 0.0f && contentRegion.y > 0.0f;

    if (!viewportInitialized && renderPaneUsable) {
      viewport.resetTo100Percent();
      viewportInitialized = true;
    }

    // Set viewport size on the backend so it renders at the right resolution.
    //
    // `setViewport` re-renders the entire document and returns a fresh
    // `FrameResult` with a new `result.bitmap`. If we drop the returned
    // future without running it through `ProcessFrameResult`, the GL
    // texture stays frozen at whatever the last explicit upload produced
    // — which on startup is the backend's default-viewport render
    // (512×384 → aspect-fit-squared to the document's aspect for
    // whatever SVG we loaded). The user experiences that as "nothing
    // renders" or "all black" once the pane grows past the tiny initial
    // crop.
    //
    // Target size: `paneSize * devicePixelRatio`. On this branch
    // `ViewportState::documentViewBox` is not yet populated (that wiring
    // is S12 follow-up — it lived in `EditorShell` on main), so
    // `viewport.desiredCanvasSize()` falls through to `(1, 1)` and
    // posting THAT would clobber the good initial bitmap with a 1×1
    // render. Using pane pixels directly matches the user's intent
    // ("render the SVG to fill the pane") and will collapse cleanly
    // into the S12-wired `desiredCanvasSize()` once documentViewBox
    // flows through.
    //
    // Two adjustments to get this right:
    //   1. Only fire `setViewport` when the target size actually changed
    //      — otherwise we'd re-rasterize the document on every UI frame
    //      even while idle, which defeats the purpose of the event-
    //      driven loop.
    //   2. Capture the returned future as the frame's `pendingFrame` so
    //      the next loop iteration picks up the new bitmap via the same
    //      `ProcessFrameResult` path as every other mutation. Awaiting
    //      inline would stall the UI; pipelining through
    //      `pendingFrame` keeps input/paint cadence intact.
    if (renderPaneUsable) {
      // Prefer `ViewportState::desiredCanvasSize()` (which reads
      // `documentViewBox * zoom * dpr`) now that documentViewBox is
      // seeded from the backend's authoritative SVG viewBox. That
      // keeps zoom working correctly — raw pane-size bypass would
      // always render at 1:1 regardless of zoom. Fall through to
      // pane-size when documentViewBox is still empty (e.g. a
      // zero-sized document or the brief window between load and
      // first backend viewBox report).
      donner::Vector2i desiredCanvasSize = viewport.desiredCanvasSize();
      const double dpr = viewport.devicePixelRatio > 0.0 ? viewport.devicePixelRatio : 1.0;
      if (desiredCanvasSize.x <= 1 || desiredCanvasSize.y <= 1) {
        desiredCanvasSize =
            donner::Vector2i(std::max(1, static_cast<int>(std::round(viewport.paneSize.x * dpr))),
                             std::max(1, static_cast<int>(std::round(viewport.paneSize.y * dpr))));
      }
      // Guard against the "pane layout not settled" state — on the very
      // first ImGui frame the content region is occasionally clamped to
      // (1, 1). Posting a setViewport at that size clobbers the good
      // initial bitmap. Pick a small-but-reasonable floor; any real
      // pane is at least tens of pixels.
      constexpr int kMinUsefulCanvasDim = 16;
      const bool canvasSizeIsReasonable =
          desiredCanvasSize.x >= kMinUsefulCanvasDim && desiredCanvasSize.y >= kMinUsefulCanvasDim;
      if (canvasSizeIsReasonable && desiredCanvasSize != lastPostedCanvasSize) {
        lastPostedCanvasSize = desiredCanvasSize;
        if (pendingFrame.has_value() &&
            pendingFrame->wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
          ProcessFrameResult(pendingFrame->get(), flatTexture, compositedPreview, viewport,
                             textEditor, lastShownErrorLine, lastShownErrorReason);
          pendingFrame.reset();
        }
        if (!pendingFrame.has_value()) {
          pendingFrame = backend->setViewport(desiredCanvasSize.x, desiredCanvasSize.y);
        } else {
          // An input-driven frame is still in flight; let it land first
          // and pick up the new viewport on the next main-loop tick.
          // Reset `lastPostedCanvasSize` so we try again next frame.
          lastPostedCanvasSize = donner::Vector2i(-1, -1);
        }
      }
    }

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

    // Pan gesture.
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

    // Scroll gesture (zoom / pan).
    const donner::editor::RenderPaneGestureContext gestureContext{
        .paneRect = paneRect,
        .mouseDragPanActive = panning,
    };
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

    // Draw the document image and selection chrome.
    if (flatTexture.width > 0 && flatTexture.height > 0) {
      const donner::Box2d screenRect = viewport.imageScreenRect();
      const ImVec2 imageOrigin(static_cast<float>(screenRect.topLeft.x),
                               static_cast<float>(screenRect.topLeft.y));
      const ImVec2 imageBottomRight(static_cast<float>(screenRect.bottomRight.x),
                                    static_cast<float>(screenRect.bottomRight.y));
#ifdef __EMSCRIPTEN__
      if (testHooksEnabled) {
        DonnerTestHooksRecordDocumentImageRect(screenRect.topLeft.x, screenRect.topLeft.y,
                                               screenRect.bottomRight.x, screenRect.bottomRight.y);
      }
#endif

      ImDrawList* paneDrawList = ImGui::GetWindowDrawList();

      // Checkerboard background.
      const auto DrawCheckerboard = [](ImDrawList* drawList, const ImVec2& topLeft,
                                       const ImVec2& bottomRight) {
        constexpr float kCheckerSize = 16.0f;
        const ImVec2 snappedTopLeft(std::floor(topLeft.x), std::floor(topLeft.y));
        const ImVec2 snappedBottomRight(std::floor(bottomRight.x), std::floor(bottomRight.y));
        if (snappedTopLeft.x >= snappedBottomRight.x || snappedTopLeft.y >= snappedBottomRight.y) {
          return;
        }

        drawList->PushClipRect(snappedTopLeft, snappedBottomRight, true);
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
      const auto addTextureImage = [](ImDrawList* drawList, const TextureUploadState& state,
                                      const ImVec2& topLeft, const ImVec2& bottomRight) {
        if (state.texture == 0 || state.width <= 0 || state.height <= 0) {
          return;
        }
        drawList->AddImage(static_cast<ImTextureID>(static_cast<std::uintptr_t>(state.texture)),
                           topLeft, bottomRight);
      };
      if (compositedPreview.active) {
        paneDrawList->PushClipRect(imageOrigin, imageBottomRight, true);
        addTextureImage(paneDrawList, compositedPreview.background, imageOrigin, imageBottomRight);
        const donner::Vector2d promotedOffset =
            compositedPreview.promotedTranslationDoc * viewport.pixelsPerDocUnit();
        const ImVec2 promotedOrigin(imageOrigin.x + static_cast<float>(promotedOffset.x),
                                    imageOrigin.y + static_cast<float>(promotedOffset.y));
        const ImVec2 promotedBottomRight(imageBottomRight.x + static_cast<float>(promotedOffset.x),
                                         imageBottomRight.y + static_cast<float>(promotedOffset.y));
        addTextureImage(paneDrawList, compositedPreview.promoted, promotedOrigin,
                        promotedBottomRight);
        addTextureImage(paneDrawList, compositedPreview.foreground, imageOrigin, imageBottomRight);
        addTextureImage(paneDrawList, compositedPreview.overlay, promotedOrigin,
                        promotedBottomRight);
        paneDrawList->PopClipRect();
      } else {
        addTextureImage(paneDrawList, flatTexture, imageOrigin, imageBottomRight);
      }

      const auto screenToDocument = [&](const ImVec2& screenPoint) -> donner::Vector2d {
        return viewport.screenToDocument(donner::Vector2d(screenPoint.x, screenPoint.y));
      };

      // Pointer events → backend.
      const bool toolEligible = paneHovered && !panning && !spaceHeld;
      if (toolEligible && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const ImVec2 mousePos = ImGui::GetMousePos();
        lastPostedDragMoveScreenPoint = donner::Vector2d(mousePos.x, mousePos.y);
        const donner::Vector2d docPoint = screenToDocument(mousePos);
        uint32_t mods = 0;
        if (ImGui::GetIO().KeyShift) {
          mods |= 1;  // Shift modifier bit.
        }
        if (ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper) {
          mods |= 2;  // Ctrl/Cmd modifier bit.
        }
        pendingFrame = backend->pointerEvent(donner::editor::PointerEventPayload{
            .phase = donner::editor::sandbox::PointerPhase::kDown,
            .documentPoint = docPoint,
            .buttons = 1,
            .modifiers = mods,
        });
      }
      if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && !panning && !spaceHeld &&
          ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const ImVec2 mousePos = ImGui::GetMousePos();
        const donner::Vector2d screenPoint(mousePos.x, mousePos.y);
        if (ShouldPostDragMove(screenPoint, lastPostedDragMoveScreenPoint)) {
          lastPostedDragMoveScreenPoint = screenPoint;
          const donner::Vector2d docPoint = screenToDocument(mousePos);
          const auto dragPostStart = std::chrono::steady_clock::now();
          pendingFrame = backend->pointerEvent(donner::editor::PointerEventPayload{
              .phase = donner::editor::sandbox::PointerPhase::kMove,
              .documentPoint = docPoint,
              .buttons = 1,
          });
          if (experimentalMode) {
            const auto dragPostEnd = std::chrono::steady_clock::now();
            const double postMs =
                std::chrono::duration<double, std::milli>(dragPostEnd - dragPostStart).count();
            std::fprintf(stderr, "[drag] kMove post=%.3f ms (docPoint=%.1f,%.1f)\n", postMs,
                         docPoint.x, docPoint.y);
          }
        }
      }
      if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !panning) {
        lastPostedDragMoveScreenPoint.reset();
        const donner::Vector2d docPoint = screenToDocument(ImGui::GetMousePos());
        pendingFrame = backend->pointerEvent(donner::editor::PointerEventPayload{
            .phase = donner::editor::sandbox::PointerPhase::kUp,
            .documentPoint = docPoint,
        });
      } else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        lastPostedDragMoveScreenPoint.reset();
      }

      // Selection chrome (path outlines + AABB + marquee + hover rect)
      // is drawn into the document bitmap on the backend via
      // `OverlayRenderer::drawChromeWithTransform`, so it's pixel-
      // locked to the rasterized content. During active drag preview
      // frames the backend intentionally skips the overlay readback, so
      // the host draws lightweight AABB chrome above the cached preview
      // textures until the mouse-up settle frame restores the full
      // backend-rendered path outline.
    } else {
      ImGui::TextUnformatted("(no rendered image)");
    }

    // Frame graph.
    {
      constexpr float kFramePadding = 8.0f;
      const float graphHeight = kFrameGraphHeight + ImGui::GetTextLineHeightWithSpacing();
      ImGui::SetCursorPos(ImVec2(kFramePadding, contentRegion.y - graphHeight - kFramePadding));
      RenderFrameGraph(frameHistory);
    }

    ImGui::End();

    // --- Tree View pane ---
    ImGui::Begin("Tree View");
    {
      const auto& treeSummary = backend->tree();
      if (treeSummary.nodes.empty()) {
        ImGui::TextDisabled("No document loaded.");
      } else {
        ImGui::BeginChild("TreeViewScroll", ImVec2(0, 0), ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar);

        // Single-pass depth-walk using the flat node array.
        // Track open/close state via a depth stack of "currently open" depths.
        int prevDepth = -1;
        bool scrollToSelected = false;

        for (size_t i = 0; i < treeSummary.nodes.size(); ++i) {
          const auto& node = treeSummary.nodes[i];
          int depth = static_cast<int>(node.depth);

          // Close tree nodes that are deeper than current depth.
          while (prevDepth >= depth) {
            ImGui::TreePop();
            --prevDepth;
          }

          ImGuiTreeNodeFlags flags =
              ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
              ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;

          if (node.selected) {
            flags |= ImGuiTreeNodeFlags_Selected;
          }

          // Check if this node has children (next node is deeper).
          bool hasChildren =
              (i + 1 < treeSummary.nodes.size() && treeSummary.nodes[i + 1].depth > node.depth);
          if (!hasChildren) {
            flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
          }

          ImGui::PushID(static_cast<int>(i));
          bool open = ImGui::TreeNodeEx(node.displayName.c_str(), flags);
          ImGui::PopID();

          // Handle click → selection.
          if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
            uint8_t mode = 0;  // Replace.
            if (ImGui::GetIO().KeyCtrl) {
              mode = 1;  // Toggle.
            } else if (ImGui::GetIO().KeyShift) {
              mode = 2;  // Add.
            }
            (void)backend->selectElement(node.entityId, node.entityGeneration, mode);
          }

          // Scroll to selected node.
          if (node.selected && !scrollToSelected) {
            ImGui::SetScrollHereY();
            scrollToSelected = true;
          }

          if (hasChildren && open) {
            prevDepth = depth;
          } else if (!hasChildren) {
            // Leaf — don't change prevDepth (no TreePush happened).
          } else {
            // Node closed — don't descend. Skip all children.
            // (ImGui handles the skip via TreeNodeEx returning false.)
          }
        }

        // Close remaining open tree nodes.
        while (prevDepth >= 0) {
          ImGui::TreePop();
          --prevDepth;
        }

        ImGui::EndChild();
      }
    }
    ImGui::End();

    // --- Inspector pane (placeholder) ---
    ImGui::Begin("Inspector");
    ImGui::TextDisabled("Element inspector pending backend element-detail API (S9 follow-up).");
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
    if (testHooksEnabled) {
      // Record the rendered frame time in ms. We use ImGui's `DeltaTime`
      // (seconds) — same source as the in-editor FPS graph — so the test
      // harness and the on-screen diag are reading from the same clock.
      const auto counters = backend->compositorFastPathCountersForTesting();
      DonnerTestHooksRecordLoopTick(1, static_cast<double>(ImGui::GetIO().DeltaTime) * 1000.0,
                                    static_cast<double>(counters.fastPathFrames),
                                    static_cast<double>(counters.slowPathFramesWithDirty),
                                    static_cast<double>(counters.noDirtyFrames));
      // Flip `__donner_ready` on the first successfully rendered frame
      // that followed the initial (synchronous) `loadBytes`. Done once,
      // cheaply: the JS side guards `__donner_ready` itself.
      static bool markedReady = false;
      if (!markedReady) {
        DonnerTestHooksMarkReady();
        markedReady = true;
      }
    }
    emscripten_sleep(0);
#endif

    // Decide whether the next iteration should poll (fully active),
    // wake on the cursor-blink cadence (idle-with-focused-source-pane),
    // or sleep on the full idle timeout (truly idle). We read ImGui's
    // post-frame interaction state here because these signals aren't
    // available before `NewFrame` — mouse dragging, active items,
    // want-text-input, popup-open. The top-of-loop physical-state
    // signals (mouse buttons, pendingFrame, fetch in flight, debounce
    // timer, repro recorder) are complementary: they capture everything
    // that doesn't flow through ImGui's focus model.
    {
      const bool anyItemActive = ImGui::IsAnyItemActive();
      // Use `IsAnyMouseDown` rather than `IsMouseDragging`: the latter
      // gates on a ~6px displacement threshold, so the first handful
      // of milliseconds after a press (before enough motion has
      // accumulated) don't register as "dragging" and the next-frame
      // decision falls back to lastFrameWasActive=false. On WASM that
      // manifests as a visible stutter at the start of every drag:
      // the skip branch fires one iteration, the mouse events queue,
      // the next frame replays them all at once, jump. Any mouse
      // button held is sufficient signal to keep the loop polling.
      const bool anyMouseDragging = ImGui::IsAnyMouseDown();
      const bool popupOpen = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup);
      // "Active" = something other than a blinking cursor needs a
      // render next frame. Drag, popups, and any-item-active all poll
      // at full speed.
      lastFrameWasActive = anyItemActive || anyMouseDragging || popupOpen;
#ifndef __EMSCRIPTEN__
      const ImGuiIO& lastIo = ImGui::GetIO();
      const bool wantTextInput = lastIo.WantTextInput;
      // Blink-only path: pure focus-without-interaction. A full 60 FPS
      // wouldn't be wrong but would defeat the on-demand loop for any
      // SVG author who leaves the source pane focused between edits —
      // that would trigger the same "idle burn" the historical commit
      // fixed. The shorter wake cadence (`kCursorBlinkWakeSeconds`)
      // keeps the blink animated without pinning CPU.
      lastFrameNeededBlinkWake = wantTextInput && !lastFrameWasActive;
#endif
    }
  }

  // ---------------------------------------------------------------------------
  // Teardown
  // ---------------------------------------------------------------------------
  if (reproRecorder.has_value()) {
    const std::size_t frames = reproRecorder->frameCount();
    if (reproRecorder->flush()) {
      std::cerr << "[repro] wrote " << frames << " frames to " << reproOutputPath.value_or("")
                << "\n";
    } else {
      std::cerr << "[repro] failed to write " << reproOutputPath.value_or("") << "\n";
    }
  }

  glfwSetScrollCallback(window, pendingScrollEvents.previousCallback);
  glfwSetWindowUserPointer(window, nullptr);
  const GLuint texturesToDelete[] = {
      flatTexture.texture,
      compositedPreview.background.texture,
      compositedPreview.promoted.texture,
      compositedPreview.foreground.texture,
      compositedPreview.overlay.texture,
  };
  glDeleteTextures(static_cast<GLsizei>(sizeof(texturesToDelete) / sizeof(texturesToDelete[0])),
                   texturesToDelete);
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}
