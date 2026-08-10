/// @file

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

EM_JS(void, InitializeWasmEditorFrameScheduling, (), {
  window['__donnerEditorFrameRequested'] = true;
  window['__donnerMainLoopRenderedFrames'] = 0;
  const requestFrame = function() {
    window['__donnerEditorFrameRequested'] = true;
  };
  // Capture at window scope: direct worker canvases and transient DOM overlays can be the event
  // target even though GLFW ultimately routes the interaction to the editor canvas.
  for (const eventName of['mousedown', 'mouseup', 'mousemove', 'pointerdown', 'pointerup',
                          'pointermove', 'pointercancel', 'pointerenter', 'pointerleave', 'wheel',
                          'contextmenu', 'keydown', 'keyup', 'compositionstart',
                          'compositionupdate', 'compositionend', 'beforeinput', 'input', 'paste',
                          'resize', 'focus', 'blur']) {
    window.addEventListener(eventName, requestFrame, {capture : true, passive : true});
  }
  document.addEventListener('visibilitychange', requestFrame, {capture : true, passive : true});
});

EM_JS(bool, ConsumeBrowserEditorFrameRequest, (), {
  const requested = Boolean(window['__donnerEditorFrameRequested']);
  window['__donnerEditorFrameRequested'] = false;
  // Counted here rather than from its own call: this already runs on every animation-frame
  // callback, including the ones the editor declines, so an idle page pays no extra boundary
  // crossing for the probe.
  const stats = window['__donnerFrameLoopStats'];
  if (stats) {
    stats['callbacks'] = (stats['callbacks'] | 0) + 1;
  }
  return requested;
});

EM_JS(void, MarkWasmEditorFrameRendered, (), {
  window['__donnerMainLoopRenderedFrames'] =
      Number(window['__donnerMainLoopRenderedFrames'] || 0) + 1;
});

// Per-frame main-loop probe. Browser suites read `__donnerFrameLoopStats` to assert how many
// frames rebuilt the immediate-mode ImGui UI versus presented the worker document surface only,
// and the perf lane reads `uiFrameMsSamples` to compute the UI-frame cost distribution.
//
// Every property is quoted: the Wasm package is minified with Closure, which renames unquoted
// object members and would leave the browser suites reading `undefined`. The sample arrays are
// capped so a long session cannot grow the page heap without bound.
EM_JS(void, EnsureWasmFrameLoopStats, (), {
  if (!window['__donnerFrameLoopStats']) {
    window['__donnerFrameLoopStats'] = {
      'callbacks' : 0,
      'renderedFrames' : 0,
      'uiRebuilds' : 0,
      'presentationOnlyFrames' : 0,
      'inputTriggeredFrames' : 0,
      'workerTriggeredFrames' : 0,
      'timerTriggeredFrames' : 0,
      'workerOnlyFrames' : 0,
      'lastFrameUiRebuilt' : true,
      'uiFrameMsSamples' : [],
      'presentationOnlyMsSamples' : [],
    };
  }
});

EM_JS(void, RecordWasmFrameLoopSample, (int triggerBits, int uiRebuilt, double frameMs), {
  const stats = window['__donnerFrameLoopStats'];
  if (!stats) {
    return;
  }

  stats['renderedFrames'] = (stats['renderedFrames'] | 0) + 1;
  stats['lastFrameUiRebuilt'] = Boolean(uiRebuilt);
  if (triggerBits & 1) {
    stats['workerTriggeredFrames'] = (stats['workerTriggeredFrames'] | 0) + 1;
  }
  if (triggerBits & 2) {
    stats['inputTriggeredFrames'] = (stats['inputTriggeredFrames'] | 0) + 1;
  }
  if (triggerBits & 4) {
    stats['timerTriggeredFrames'] = (stats['timerTriggeredFrames'] | 0) + 1;
  }
  if (triggerBits == 1) {
    stats['workerOnlyFrames'] = (stats['workerOnlyFrames'] | 0) + 1;
  }
  const kMaxSamples = 4096;
  if (uiRebuilt) {
    stats['uiRebuilds'] = (stats['uiRebuilds'] | 0) + 1;
    if (stats['uiFrameMsSamples'].length < kMaxSamples) {
      stats['uiFrameMsSamples'].push(frameMs);
    }
  } else {
    stats['presentationOnlyFrames'] = (stats['presentationOnlyFrames'] | 0) + 1;
    if (stats['presentationOnlyMsSamples'].length < kMaxSamples) {
      stats['presentationOnlyMsSamples'].push(frameMs);
    }
  }
});

// Publish the C++-owned pinch policy so the page's WebKit gesture bridge
// synthesizes wheel deltas calibrated against the same zoom-step model the
// classifier uses. See donner/editor/PinchZoomPolicy.h for the derivation; the
// bootstrap keeps a numeric fallback for the window between page load and
// runtime initialization.
EM_JS(void, PublishWasmPinchZoomPolicy, (double wheelDeltaPerLnScale),
      { window['__donnerPinchWheelDeltaPerLnScale'] = wheelDeltaPerLnScale; });
#else
#include "donner/base/FailureSignalHandler.h"
#endif

#include "donner/editor/EditorShell.h"
#include "donner/editor/Notice.h"
#include "donner/editor/PinchZoomPolicy.h"
#include "donner/editor/TracyWrapper.h"
#include "donner/editor/gui/EditorWindow.h"

namespace {

constexpr int kInitialWindowWidth = 1600;
constexpr int kInitialWindowHeight = 900;
constexpr std::string_view kWelcomePlaceholderSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="640" height="400" viewBox="0 0 640 400"/>)";

std::string EmbeddedBytesToString(std::span<const unsigned char> bytes) {
  std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  if (!text.empty() && text.back() == '\0') {
    text.pop_back();
  }
  return text;
}

#ifndef __EMSCRIPTEN__
// Scoped per-user path for the ImGui settings (.ini) file that persists the
// editor's dock layout and window state across sessions. Returns an empty string
// if no writable config directory can be resolved, in which case the editor
// keeps its settings in-memory and always starts from the default layout.
std::string ScopedImguiIniPath() {
  std::error_code ec;
  std::filesystem::path configDir;
#if defined(__APPLE__)
  if (const char* home = std::getenv("HOME")) {
    configDir = std::filesystem::path(home) / "Library" / "Application Support" / "Donner";
  }
#else
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && xdg[0] != '\0') {
    configDir = std::filesystem::path(xdg) / "donner";
  } else if (const char* home = std::getenv("HOME")) {
    configDir = std::filesystem::path(home) / ".config" / "donner";
  }
#endif
  if (configDir.empty()) {
    return {};
  }
  std::filesystem::create_directories(configDir, ec);
  if (ec) {
    return {};
  }
  return (configDir / "editor_imgui.ini").string();
}
#endif

void RunEditorFrame(donner::editor::gui::EditorWindow& window, donner::editor::EditorShell& shell) {
  {
    ZoneScopedN("waitEvents");
    if (const std::optional<float> wakeSeconds = shell.nextIdleWakeSeconds()) {
      window.waitEventsTimeout(*wakeSeconds);
    } else {
      window.waitEvents();
    }
  }
  {
    ZoneScopedN("shell.prepareFrame");
    shell.prepareFrame();
  }
  {
    ZoneScopedN("beginFrame");
    window.beginFrame();
  }
  {
    ZoneScopedN("shell.runFrame");
    shell.runFrame();
  }
  {
    ZoneScopedN("endFrame");
    window.endFrame();
  }
  FrameMark;
}

#ifdef __EMSCRIPTEN__
struct WasmEditorLoopState {
  using RenderFrameCallback = void (*)(donner::editor::gui::EditorWindow&,
                                       donner::editor::EditorShell&);

  std::unique_ptr<donner::editor::gui::EditorWindow> window;
  std::unique_ptr<donner::editor::EditorShell> shell;
  std::optional<double> nextIdleWakeAtMs;
  // Reentrancy guard for RunWasmEditorFrame; see the drop-nested-callback
  // comment there.
  bool frameActive = false;
  // This must remain an indirect call. Binaryen otherwise folds the entire editor render path into
  // the requestAnimationFrame callback, which Safari tier-compiles even while the editor is idle.
  RenderFrameCallback renderFrame = &RunEditorFrame;
};

void RunWasmEditorFrame(void* userdata) {
  auto* state = static_cast<WasmEditorLoopState*>(userdata);
  // Synchronous browser calls proxied from the pthread can let WebKit service
  // another requestAnimationFrame callback before the suspended frame resumes.
  // A nested frame would call ImGui::NewFrame() twice without an intervening
  // Render() and recurse until the JS stack overflows. Drop that callback; the
  // active frame will finish and the browser will schedule the next one.
  if (state->frameActive) {
    return;
  }
  if (state->window->shouldClose()) {
    emscripten_cancel_main_loop();
    delete state;
    return;
  }

  const double nowMs = emscripten_get_now();
  const bool editorRequested = state->window->consumeWasmFrameRequest();
  const bool browserRequested = ConsumeBrowserEditorFrameRequest();
  const bool timerDue = state->nextIdleWakeAtMs.has_value() && nowMs >= *state->nextIdleWakeAtMs;
  if (!editorRequested && !browserRequested && !timerDue) {
    return;
  }
  const int triggerBits =
      (editorRequested ? 1 : 0) | (browserRequested ? 2 : 0) | (timerDue ? 4 : 0);

  state->frameActive = true;
  const double frameStartMs = emscripten_get_now();
  // Immediate-mode UI is rebuilt from scratch every frame, and on this build that rebuild is the
  // dominant per-frame cost. A frame woken only by the render worker publishing a fresh document
  // epoch changes nothing the UI draws - the document lives on its own canvas - so the gate lets
  // that frame place the document surface and leave the UI canvas showing the frame it already
  // presented. See donner/editor/UiFrameGate.h for the full predicate.
  const donner::editor::UiFrameWork work =
      state->shell->classifyFrameWork(editorRequested, browserRequested, timerDue);
  const bool uiRebuilt = work != donner::editor::UiFrameWork::PresentationOnly;
  if (uiRebuilt) {
    state->renderFrame(*state->window, *state->shell);
  } else {
    state->shell->runPresentationOnlyFrame();
  }
  RecordWasmFrameLoopSample(triggerBits, uiRebuilt ? 1 : 0, emscripten_get_now() - frameStartMs);
  MarkWasmEditorFrameRendered();
  if (const std::optional<float> wakeSeconds = state->shell->nextIdleWakeSeconds()) {
    state->nextIdleWakeAtMs = emscripten_get_now() + std::max(0.0f, *wakeSeconds) * 1000.0;
  } else {
    state->nextIdleWakeAtMs.reset();
  }
  state->frameActive = false;
}
#endif

}  // namespace

int main(int argc, char** argv) {
#ifndef __EMSCRIPTEN__
  donner::InstallFailureSignalHandler();
#endif

  if (const char* bwd = std::getenv("BUILD_WORKING_DIRECTORY")) {
    std::filesystem::current_path(bwd);
  }

  std::optional<std::string> svgPath;
  std::optional<std::string> initialSource;
  std::optional<std::string> initialPath;
  std::optional<std::string> reproOutputPath;
  bool showWelcome = false;
#ifdef __EMSCRIPTEN__
  initialSource = kWelcomePlaceholderSvg;
  showWelcome = true;
#else
  constexpr std::string_view kUsage =
      "Usage: donner-editor [--experimental] [--save-repro <path>] [filename]\n";
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--experimental") {
      // Developer CLI contract: keep accepting this flag even when it is a
      // no-op. Old repro scripts and launch aliases pass it, and removing it
      // breaks callers.
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

    if (arg.starts_with("--")) {
      std::cerr << "Unknown option " << arg << "\n" << kUsage;
      return 1;
    }

    if (svgPath.has_value()) {
      std::cerr << kUsage;
      return 1;
    }

    svgPath = std::string(arg);
  }

  if (!svgPath.has_value()) {
    initialSource = kWelcomePlaceholderSvg;
    showWelcome = true;
  }
#endif

  std::string imguiIniPath;
#ifndef __EMSCRIPTEN__
  imguiIniPath = ScopedImguiIniPath();
#endif
  auto window = std::make_unique<donner::editor::gui::EditorWindow>(
      donner::editor::gui::EditorWindowOptions{.title = "Donner SVG Editor",
                                               .initialWidth = kInitialWindowWidth,
                                               .initialHeight = kInitialWindowHeight,
                                               .imguiIniPath = imguiIniPath});
  if (!window->valid()) {
    std::cerr << "Failed to initialize editor window\n";
    return 1;
  }

  auto shell = std::make_unique<donner::editor::EditorShell>(
      *window, donner::editor::EditorShellOptions{
                   .svgPath = svgPath.value_or(""),
                   .initialSource = initialSource,
                   .initialPath = initialPath,
                   .showWelcome = showWelcome,
                   .editorNoticeText = EmbeddedBytesToString(donner::embedded::kEditorNoticeText),
                   .reproOutputPath = reproOutputPath});
  if (!shell->valid()) {
    if (svgPath.has_value()) {
      std::cerr << "Could not open file " << *svgPath << "\n";
    } else {
      std::cerr << "Could not initialize editor content\n";
    }
    return 1;
  }

#ifdef __EMSCRIPTEN__
  auto* loopState = new WasmEditorLoopState{std::move(window), std::move(shell), std::nullopt};
  InitializeWasmEditorFrameScheduling();
  EnsureWasmFrameLoopStats();
  PublishWasmPinchZoomPolicy(donner::editor::PinchWheelDeltaPerLnScale());
  // The browser presents the WebGPU canvas when the requestAnimationFrame callback returns.
  emscripten_set_main_loop_arg(&RunWasmEditorFrame, loopState, /*fps=*/0,
                               /*simulateInfiniteLoop=*/true);
#else
  while (!window->shouldClose()) {
    RunEditorFrame(*window, *shell);
  }
#endif

  return 0;
}
