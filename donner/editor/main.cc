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

#include "donner/base/AsyncifySuspendProbe.h"
#include "donner/editor/WholeAppWorkerBridge.h"
#include "donner/gpu/browser/BrowserGpuOwner.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"

// The app pthread's JS context has no `window`, so the frame-scheduling flag,
// the frame-loop probe surface, and the pinch policy publication all route
// through the main-thread bridge instead of `EM_JS`. `callbacks` is counted
// locally and flushed with the next sample rather than proxied per rAF tick.
namespace {
int g_pendingFrameLoopCallbacks = 0;
}  // namespace

bool ConsumeBrowserEditorFrameRequest() {
  ++g_pendingFrameLoopCallbacks;
  return donner::editor::whole_app_worker::ConsumeFrameRequest();
}

void RecordWasmFrameLoopSample(int triggerBits, double frameMs) {
  donner::editor::whole_app_worker::RecordFrameSample(triggerBits, frameMs,
                                                      g_pendingFrameLoopCallbacks);
  g_pendingFrameLoopCallbacks = 0;
}

void PublishWasmPinchZoomPolicy(double wheelDeltaPerLnScale) {
  donner::editor::whole_app_worker::PublishPinchZoomPolicy(wheelDeltaPerLnScale);
}
#else
#include "donner/base/FailureSignalHandler.h"
#endif

#include "donner/editor/EditorBuildInfo.h"
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
#ifndef __EMSCRIPTEN__
  // Desktop frames block here until input or the shell's idle timer. The
  // browser build must NOT: its scheduler is the animation-frame tick that
  // called this function, and a blocking wait inside that tick deadlocks the
  // demand-driven loop the first time no DOM event is pending. The wake
  // sources the wait would sleep on (worker completion, idle timers) are
  // exactly the tick's trigger bits.
  {
    ZoneScopedN("waitEvents");
    std::optional<float> wakeSeconds = shell.nextIdleWakeSeconds();
    if (window.hasIdleGpuWork()) {
      constexpr float kGpuIdlePollSeconds = 0.1f;
      wakeSeconds = wakeSeconds.has_value() ? std::min(*wakeSeconds, kGpuIdlePollSeconds)
                                            : kGpuIdlePollSeconds;
    }
    if (wakeSeconds.has_value()) {
      window.waitEventsTimeout(*wakeSeconds);
    } else {
      window.waitEvents();
    }
  }
#endif
  window.pollIdleGpu();
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

bool CanRunWasmEditorFrame(void* userdata) {
  const auto* state = static_cast<WasmEditorLoopState*>(userdata);
  return !state->frameActive && !donner::gpu::browser::IsBrowserGpuOwnerDispatchActive();
}

/// Protect an active canvas frame from nested editor callbacks and asynchronous GPU batches.
class WasmFrameScope {
public:
  explicit WasmFrameScope(WasmEditorLoopState& state) : state_(state) {
    state_.frameActive = true;
    donner::gpu::browser::SetBrowserGpuOwnerFrameActive(true);
  }

  ~WasmFrameScope() {
    donner::gpu::browser::SetBrowserGpuOwnerFrameActive(false);
    state_.frameActive = false;
#ifdef DONNER_EDITOR_WHOLE_APP_WORKER
    donner::editor::whole_app_worker::NotifyFrameFinished();
#endif
  }

  WasmFrameScope(const WasmFrameScope&) = delete;
  WasmFrameScope& operator=(const WasmFrameScope&) = delete;

private:
  WasmEditorLoopState& state_;
};

void RunWasmEditorFrame(void* userdata) {
  auto* state = static_cast<WasmEditorLoopState*>(userdata);
  // Synchronous browser calls proxied from the pthread can let WebKit service
  // another requestAnimationFrame callback before the suspended frame resumes.
  // A nested frame would call ImGui::NewFrame() twice without an intervening
  // Render() and recurse until the JS stack overflows. Drop that callback; the
  // active frame will finish and the browser will schedule the next one.
  if (!CanRunWasmEditorFrame(userdata)) {
    return;
  }
  if (state->window->shouldClose()) {
#ifdef DONNER_EDITOR_WHOLE_APP_WORKER
    donner::editor::whole_app_worker::StopFrameDriver();
#else
    emscripten_cancel_main_loop();
#endif
    delete state;
#ifdef DONNER_EDITOR_WHOLE_APP_WORKER
    donner::gpu::browser::StopBrowserGpuOwner();
#endif
    return;
  }

  WasmFrameScope frameScope(*state);
  const double nowMs = emscripten_get_now();
  const bool editorRequested = state->window->consumeWasmFrameRequest();
  // Input that has arrived but not been presented is its own wake source.
  //
  // A DOM event raises the browser frame request on the page's main thread the
  // moment it fires, while the event itself reaches this thread through the
  // proxying queue. An animation-frame tick already in flight can therefore
  // spend the request and render before the event is applied; the event then
  // lands with nothing left to ask for a frame, and this loop declines every
  // subsequent tick with the input un-presented. Measured on Gecko: a pointer
  // move mid-drag sat applied-but-unpresented through ~520 consecutive declined
  // ticks (~8s) until an unrelated DOM event happened to raise the flag again,
  // and a press taken the same way left the click buffered and the shape
  // unselected. Consuming the request first keeps the request-clearing
  // unconditional.
  const bool browserRequested =
      ConsumeBrowserEditorFrameRequest() || state->window->hasQueuedInputEvents();
  const bool timerDue = state->nextIdleWakeAtMs.has_value() && nowMs >= *state->nextIdleWakeAtMs;
  if (!editorRequested && !browserRequested && !timerDue) {
    // Idle completion confirmation can yield to the browser and deliver another scheduler callback.
    state->window->pollIdleGpu();
    return;
  }
  const int triggerBits =
      (editorRequested ? 1 : 0) | (browserRequested ? 2 : 0) | (timerDue ? 4 : 0);

  const double frameStartMs = emscripten_get_now();
  // Every frame is a full frame. The document and the UI are composited by Geode into the same
  // canvas in the same pass, so there is no frame whose only work lives elsewhere: skipping the
  // UI pass would also skip the document. The presentation-only frame the browser build used to
  // run existed purely because the document had its own canvas.
  state->renderFrame(*state->window, *state->shell);
  RecordWasmFrameLoopSample(triggerBits, emscripten_get_now() - frameStartMs);
  if (const std::optional<float> wakeSeconds = state->shell->nextIdleWakeSeconds()) {
    state->nextIdleWakeAtMs = emscripten_get_now() + std::max(0.0f, *wakeSeconds) * 1000.0;
  } else {
    state->nextIdleWakeAtMs.reset();
  }
}
#endif

}  // namespace

int main(int argc, char** argv) {
#ifdef DONNER_EDITOR_WHOLE_APP_WORKER
  // Must precede every other line: `main()` runs on a pthread here, and the
  // first editor `EM_JS` body that touches `window` would otherwise throw
  // before anything is constructed.
  donner::editor::whole_app_worker::InstallWorkerGlobalShim();
  donner::editor::whole_app_worker::Install();
  donner::gpu::browser::RegisterBrowserGpuOwner();
#endif
#ifndef __EMSCRIPTEN__
  donner::InstallFailureSignalHandler();
#endif

  if (const char* bwd = std::getenv("BUILD_WORKING_DIRECTORY")) {
    std::filesystem::current_path(bwd);
  }

  std::optional<std::string> svgPath;
  std::optional<std::string> initialSource;
  std::optional<std::string> initialPath;
  bool showWelcome = false;
#ifdef __EMSCRIPTEN__
  initialSource = kWelcomePlaceholderSvg;
  showWelcome = true;
#else
  std::optional<std::string> reproOutputPath;
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
      *window,
      donner::editor::EditorShellOptions{
          .svgPath = svgPath.value_or(""),
          .initialSource = initialSource,
          .initialPath = initialPath,
          .showWelcome = showWelcome,
          .editorNoticeText = EmbeddedBytesToString(donner::embedded::kEditorNoticeText),
          .editorBuildInfo = EmbeddedBytesToString(donner::embedded::kEditorBuildInfo),
          .fontPreviewCachePath =
              imguiIniPath.empty()
                  ? std::string()
                  : (std::filesystem::path(imguiIniPath).parent_path() / "font-previews").string(),
#ifndef __EMSCRIPTEN__
          .reproOutputPath = reproOutputPath,
#endif
      });
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
  PublishWasmPinchZoomPolicy(donner::editor::PinchWheelDeltaPerLnScale());
  // The browser presents the WebGPU canvas when the requestAnimationFrame callback returns.
  // `main()` runs on a worker here. Emscripten's rAF scheduler silently
  // degrades to a setTimeout emulation when the worker global has no
  // `requestAnimationFrame`, which is not vsync-aligned, so the driver probes
  // for it and installs a proxied main-thread rAF pump where it is missing.
  donner::BeginSuspendFrame();
  donner::editor::whole_app_worker::InstallFrameDriver(&RunWasmEditorFrame, loopState,
                                                       &CanRunWasmEditorFrame);
#else
  while (!window->shouldClose()) {
    RunEditorFrame(*window, *shell);
  }
#endif

  return 0;
}
