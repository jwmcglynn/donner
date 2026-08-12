#pragma once
/// @file
///
/// `EditorWindow` - RAII wrapper around GLFW + OpenGL + Dear ImGui. Keeps
/// the main binary thin by encapsulating everything that would otherwise
/// be boilerplate: GLFW init, window creation, context current, glad
/// loader, ImGui context, imgui_impl_glfw + imgui_impl_opengl3 setup,
/// plus texture upload from a `RendererBitmap`.
///
/// The class is intentionally narrow - it exposes only what the main
/// binary needs:
///   - construct/destruct (RAII handles cleanup)
///   - `shouldClose()` / `pollEvents()` / `waitEvents()` / `waitEventsTimeout()` - event loop hooks
///   - `beginFrame()` / `endFrame()` - ImGui frame bracketing + swap
///   - `uploadBitmap()` - moves a CPU-side RGBA buffer into a GL texture
///     (reuses the same texture ID across frames to avoid churn)
///   - `textureId()` - exposes the current texture for `ImGui::Image`
///
/// Any code that wants to draw ImGui widgets happens *between*
/// `beginFrame()` and `endFrame()` on the caller's side - this class
/// doesn't own the widget tree, just the hosting surface.

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#ifdef DONNER_EDITOR_WGPU
#include <webgpu/webgpu.hpp>
#endif

#include "donner/base/Vector2.h"
#include "donner/svg/renderer/RendererInterface.h"

struct GLFWwindow;
struct ImFont;
using GLFWscrollfun = void (*)(GLFWwindow*, double, double);

namespace donner::geode {
class GeodeDevice;
}

namespace donner::editor::gui {

namespace internal {

/// The Wasm render worker owns a separate WebGPU device, so the UI's primary
/// and direct-framebuffer renderers remain single-threaded and may share one
/// GeodeDevice wrapper. Desktop's AsyncRenderer shares the primary wrapper
/// across threads; its UI-only framebuffer renderers need a separate wrapper
/// to isolate mutable counters and deferred-destroy queues.
[[nodiscard]] constexpr bool ShouldShareWgpuFramebufferGeodeDevice(bool emscriptenBuild) noexcept {
  return emscriptenBuild;
}

/// Opaque fallback clear color for the browser UI surface, matching the page
/// background painted behind the canvas (`donner/editor/wasm/editor.css`:
/// `background: #101317`).
inline constexpr std::array<float, 4> kWasmOpaqueSurfaceClearColor = {
    16.0f / 255.0f, 19.0f / 255.0f, 23.0f / 255.0f, 1.0f};

/// Pick the browser UI surface's clear color.
///
/// The Wasm UI surface clears uncovered render-pane pixels to alpha 0 so the
/// worker's document canvas composites underneath it. That only produces
/// transparency when the configured surface honors the alpha channel; a surface
/// composited as opaque turns the same clear into solid black and blanks the
/// whole editor area until real content covers it. Keep the transparent clear
/// only when the surface actually reports premultiplied alpha; otherwise clear
/// to the page background so the result matches what the page already paints
/// behind the canvas.
[[nodiscard]] constexpr std::array<float, 4> WasmSurfaceClearColor(
    std::array<float, 4> transparentClearColor, bool premultipliedAlphaSupported) noexcept {
  return premultipliedAlphaSupported ? transparentClearColor : kWasmOpaqueSurfaceClearColor;
}

enum class WgpuSurfaceFailureKind {
  Timeout,
  OutdatedOrLost,
  Setup,
  Fatal,
};

struct WgpuSurfaceRetryDecision {
  bool requestFrame = false;
  bool reconfigure = false;

  bool operator==(const WgpuSurfaceRetryDecision&) const = default;
};

/// Bound retries so a permanently lost/device-fatal surface cannot turn the
/// event-driven Wasm loop back into a hot spin.
[[nodiscard]] constexpr WgpuSurfaceRetryDecision WgpuSurfaceRetryDecisionFor(
    WgpuSurfaceFailureKind failure, unsigned consecutiveFailures) noexcept {
  constexpr unsigned kMaxConsecutiveRetries = 3u;
  if (failure == WgpuSurfaceFailureKind::Fatal || consecutiveFailures >= kMaxConsecutiveRetries) {
    return {};
  }
  return WgpuSurfaceRetryDecision{
      .requestFrame = true,
      .reconfigure = failure == WgpuSurfaceFailureKind::OutdatedOrLost,
  };
}

struct WgpuDiagnosticReadbackDecision {
  bool retry = false;
  bool completeRequest = false;

  bool operator==(const WgpuDiagnosticReadbackDecision&) const = default;
};

/// Diagnostic readback is deliberately best-effort. A transient capture failure, including setup
/// before mapAsync, gets two retries, while a successful capture or third consecutive failure
/// completes the request so the event-driven browser loop cannot become a permanent readback spin.
[[nodiscard]] constexpr WgpuDiagnosticReadbackDecision WgpuDiagnosticReadbackDecisionFor(
    bool captureSucceeded, unsigned consecutiveFailuresBeforeAttempt) noexcept {
  constexpr unsigned kMaxFailedAttempts = 3u;
  if (captureSucceeded || consecutiveFailuresBeforeAttempt >= kMaxFailedAttempts - 1u) {
    return WgpuDiagnosticReadbackDecision{
        .retry = false,
        .completeRequest = true,
    };
  }
  return WgpuDiagnosticReadbackDecision{
      .retry = true,
      .completeRequest = false,
  };
}

/// Once the map callback releases the in-flight gate, every live completion must recheck the
/// JavaScript request counters. A transient failure needs another attempt, while a successful or
/// terminal attempt may have a newer request waiting behind it. The JavaScript wake helper filters
/// completed requests, so this recheck does not create idle frames.
[[nodiscard]] constexpr bool ShouldRecheckPendingWgpuReadbackRequestsAfterCompletion(
    bool callbackAlive, WgpuDiagnosticReadbackDecision decision) noexcept {
  return callbackAlive && (decision.retry || decision.completeRequest);
}

}  // namespace internal

/// HiDPI settings derived from the native window/display scale.
struct UiScaleConfig {
  double displayScale = 1.0;

  [[nodiscard]] float scaledPixels(double basePixels) const {
    return static_cast<float>(basePixels * displayScale);
  }

  [[nodiscard]] float fontGlobalScale() const { return static_cast<float>(1.0 / displayScale); }
};

/// Derive the editor's UI scaling from logical window size, framebuffer size, and the platform's
/// content scale hint. Prefers the framebuffer/logical ratio when available.
[[nodiscard]] UiScaleConfig ComputeUiScaleConfig(int logicalWindowWidth, int framebufferWidth,
                                                 double contentScaleX);

struct EditorWindowOptions {
  std::string title = "Donner SVG Editor";
  int initialWidth = 1280;
  int initialHeight = 720;
  /// Whether the native desktop window should be shown. Hidden windows still
  /// create a real OpenGL context and are useful for framebuffer replay tests.
  bool visible = true;
  /// Request an offscreen framebuffer-readback replay surface. Linux uses GLFW's
  /// windowless "null" platform (OSMesa software GL), even when a display is
  /// available. macOS keeps a native GPU-backed Cocoa context and relies on
  /// `visible = false` for hidden replay windows.
  bool offscreen = false;
  /// Force the WebGPU frame path to render into an offscreen texture instead of
  /// a presentable window surface, on every platform.
  ///
  /// Linux replay reaches that arm implicitly through GLFW's windowless "null"
  /// platform, so on macOS - which always builds a real (possibly hidden) Cocoa
  /// surface - the offscreen arm is otherwise unreachable and untestable. This
  /// flag makes it reachable anywhere a WebGPU device exists, so a single test
  /// can cover the offscreen target creation, resize, and readback path on both
  /// the Linux and macOS lanes. No effect in non-WebGPU (OpenGL) builds.
  bool forceOffscreenRenderTarget = false;
  /// Content/display scale to emulate for hidden replay windows. Replay sets
  /// this to the recorded scale so framebuffer readback reproduces the pixel
  /// geometry of captures taken on a HiDPI machine.
  double offscreenContentScale = 1.0;
  /// Background clear color (RGBA, 0..1). Matches the viewport surround
  /// when the document doesn't fill the whole window.
  float clearColor[4] = {0.11f, 0.11f, 0.13f, 1.0f};
  /// Enable framebuffer CPU readback from \ref endFrameAndReadPixels. Intended for replay tests;
  /// disabled by default so production WGPU editor frames cannot read back by accident.
  bool enableFramebufferReadback = false;
  /// Absolute path to the ImGui settings (.ini) file used to persist the dock
  /// layout and window state across sessions. Empty (the default) keeps ImGui
  /// settings in-memory only, so tests and replay stay hermetic; the desktop app
  /// sets a scoped per-user path. A missing or corrupt file falls back to the
  /// editor's default locked layout.
  std::string imguiIniPath;
};

/// Host-frame timing captured by `EditorWindow`.
struct EditorWindowFrameTiming {
  /// Time spent starting the current ImGui frame.
  double beginFrameMs = 0.0;
  /// Total time spent ending and presenting the previous host frame.
  double endFrameMs = 0.0;
  /// End-frame time spent in `ImGui::Render`.
  double imguiRenderMs = 0.0;
  /// End-frame time spent acquiring the WGPU surface texture.
  double surfaceAcquireMs = 0.0;
  /// End-frame time spent in the document underlay direct pass.
  double underlayMs = 0.0;
  /// End-frame time spent issuing ImGui backend draw commands.
  double imguiDrawMs = 0.0;
  /// End-frame time spent in the overlay/direct append pass.
  double directMs = 0.0;
  /// End-frame time spent reading the framebuffer back to the CPU.
  double readbackMs = 0.0;
  /// End-frame time spent presenting or swapping the surface.
  double presentMs = 0.0;
  /// Vertices ImGui emitted for this frame's draw data.
  ///
  /// The editor renders every vector path through Geode, so this must stay at
  /// UI-widget scale. Document-complexity geometry reaching ImGui shows up here
  /// as a jump of an order of magnitude or more.
  int imguiVertexCount = 0;
};

/// Fonts loaded into this window's ImGui context for the editor shell.
struct EditorWindowFonts {
  ImFont* uiRegular = nullptr;
  ImFont* uiBold = nullptr;
  ImFont* code = nullptr;

  [[nodiscard]] bool complete() const {
    return uiRegular != nullptr && uiBold != nullptr && code != nullptr;
  }
};

/// ImGui input state to inject for deterministic editor replay.
struct EditorWindowInputOverride {
  /// Seconds advanced by this frame.
  double deltaSeconds = 1.0 / 60.0;
  /// Mouse position in logical window coordinates.
  Vector2d mousePosition = Vector2d::Zero();
  /// Mouse-button state, indexed like ImGui mouse buttons.
  std::array<bool, 5> mouseDown = {};
  bool keyCtrl = false;   //!< Ctrl modifier state.
  bool keyShift = false;  //!< Shift modifier state.
  bool keyAlt = false;    //!< Alt modifier state.
  bool keySuper = false;  //!< Super/Command modifier state.
  /// Horizontal mouse-wheel delta for this frame.
  float mouseWheelH = 0.0f;
  /// Vertical mouse-wheel delta for this frame.
  float mouseWheel = 0.0f;
  /// ImGui key enum values pressed during this frame.
  std::vector<int> keyDownEvents;
  /// ImGui key enum values released during this frame.
  std::vector<int> keyUpEvents;
  /// UTF-32 character input events queued during this frame.
  std::vector<std::uint32_t> inputCharacters;
};

#ifdef DONNER_EDITOR_WGPU
/// Host framebuffer target exposed to direct Geode passes against the editor surface.
struct EditorWindowWgpuRenderTarget {
  /// Current swapchain texture. Valid only for the duration of the callback.
  wgpu::Texture texture;
  /// Framebuffer dimensions in physical pixels.
  Vector2i framebufferSizePx = Vector2i::Zero();
};

/// Callback invoked before ImGui renders, after the surface has been cleared.
using WgpuUnderlayRenderCallback = std::function<void(const EditorWindowWgpuRenderTarget& target)>;

/// Callback invoked after ImGui has submitted its draw data and before the surface is presented.
using WgpuDirectRenderCallback = std::function<void(const EditorWindowWgpuRenderTarget& target)>;
#endif

/// Initializes GLFW + GL + ImGui when constructed, tears everything down
/// in the destructor. One instance per process - ImGui's global state
/// means we can't easily have two at once.
class EditorWindow {
public:
  explicit EditorWindow(EditorWindowOptions options = {});
  ~EditorWindow();

  EditorWindow(const EditorWindow&) = delete;
  EditorWindow& operator=(const EditorWindow&) = delete;

  /// True iff GLFW + GL + ImGui initialized successfully. Callers should
  /// bail out if this is false instead of trying to render.
  [[nodiscard]] bool valid() const { return valid_; }

  /// True when window/context creation failed specifically because the host
  /// cannot provide a usable GL context (a headless / GPU-less environment
  /// with no software-GL fallback, e.g. GitHub-hosted macOS). Distinct from a
  /// generic `!valid()` so callers can skip GL-dependent work rather than
  /// treating it as a hard failure. Only meaningful when `valid()` is false.
  [[nodiscard]] bool glUnavailable() const { return glUnavailable_; }

  /// True when the user has clicked the window close button or pressed
  /// the OS's "close" shortcut.
  [[nodiscard]] bool shouldClose() const;

  /// Pumps the OS event queue without blocking. Use on Emscripten
  /// (where `waitEvents` is unimplemented) or when a continuous render
  /// loop is required (e.g. when an active animation is driving a
  /// fresh frame every tick).
  void pollEvents();

  /// Blocks until an OS or user-posted event arrives, then pumps the
  /// event queue once. This is the on-demand render path: the UI thread
  /// sleeps when the editor is idle and wakes on user input, window
  /// resize, or an explicit `wakeEventLoop()` from another thread.
  ///
  /// No-op on Emscripten, where the browser's requestAnimationFrame
  /// drives the main loop instead (`glfwWaitEvents` is unimplemented
  /// upstream).
  void waitEvents();

  /// Blocks until an event arrives or \p timeoutSeconds elapses, then
  /// pumps the event queue once.
  ///
  /// @param timeoutSeconds Maximum wait duration in seconds.
  void waitEventsTimeout(double timeoutSeconds);

  /// Post an empty event into the window's queue, waking a concurrent
  /// `waitEvents()` call. Safe to call from any thread. Used by the
  /// async renderer worker to wake the UI thread when a render result
  /// becomes available.
  ///
  /// On Emscripten, sets the atomic gate consumed by the next browser animation frame.
  void wakeEventLoop();

#ifdef __EMSCRIPTEN__
  /// Consume one Wasm main-frame request posted by editor or worker code.
  /// Browser input requests are tracked separately by the JavaScript bridge in `main.cc`.
  [[nodiscard]] bool consumeWasmFrameRequest() {
    return wasmFrameRequested_.exchange(false, std::memory_order_acq_rel);
  }
#endif

  /// Whether ImGui is holding input events this thread has accepted but no
  /// frame has consumed yet.
  ///
  /// Input that has arrived and not been presented is a frame obligation in its
  /// own right. `beginFrame` already carries it for the events ImGui trickles
  /// across frames; the browser's demand-driven loop needs it as a wake source
  /// too, because a DOM event's frame request is raised on the page's main
  /// thread while the event itself reaches this thread through the proxying
  /// queue, and a tick already in flight can spend the request before the event
  /// lands.
  [[nodiscard]] bool hasQueuedInputEvents() const;

  /// Starts a new ImGui frame. Caller issues `ImGui::*` widget calls
  /// after this returns.
  void beginFrame();

  /// Starts a new ImGui frame after injecting deterministic replay input.
  void beginFrameWithInput(const EditorWindowInputOverride& inputOverride);

  /// Flushes the current ImGui frame to the backbuffer, clears with the
  /// configured color, and swaps. Must be called once per `beginFrame()`.
  void endFrame();

  /// Flushes the current ImGui frame, reads the GL backbuffer before swap,
  /// then swaps. Must be called once per `beginFrame()`.
  [[nodiscard]] svg::RendererBitmap endFrameAndReadPixels();

  /// Uploads `bitmap` to the GL texture owned by this window. The
  /// texture is reused across calls - later calls replace the contents.
  /// No-op on empty bitmaps. After upload, `textureId()` returns a handle
  /// suitable for `ImGui::Image((void*)(intptr_t)textureId(), ...)`.
  void uploadBitmap(const svg::RendererBitmap& bitmap);

  /// Raw GL texture name for the most recent bitmap upload. Zero when no
  /// upload has happened yet.
  [[nodiscard]] uint32_t textureId() const { return textureId_; }

  /// Dimensions of the most recently uploaded bitmap. (0, 0) before the
  /// first upload.
  [[nodiscard]] int textureWidth() const { return textureWidth_; }
  [[nodiscard]] int textureHeight() const { return textureHeight_; }

  /// Update the native window title.
  void setTitle(std::string_view title);

  /// Logical window size in screen coordinates.
  [[nodiscard]] Vector2i windowSize() const;

  /// Physical framebuffer size in pixels. Equals \ref windowSize scaled by the
  /// backing display scale, and matches the dimensions of a bitmap returned by
  /// \ref endFrameAndReadPixels. (0, 0) when the window failed to initialize.
  [[nodiscard]] Vector2i framebufferSize() const;

  /// Backing display content scale (for example 2.0 on a Retina display).
  [[nodiscard]] Vector2d contentScale() const;

  /// Effective UI display scale used for ImGui fonts and framebuffer coordinates.
  [[nodiscard]] double displayScale() const { return uiScaleConfig_.displayScale; }

  /// Fonts already installed by an EditorShell sharing this ImGui context.
  [[nodiscard]] const EditorWindowFonts& editorFonts() const { return editorFonts_; }

  /// Remember the editor fonts without changing their ImGui debug names.
  /// @param fonts Context-local font pointers owned by the ImGui atlas.
  void setEditorFonts(EditorWindowFonts fonts) { editorFonts_ = fonts; }

  /// Timing for the most recent `beginFrame` call.
  [[nodiscard]] double lastBeginFrameMs() const { return lastBeginFrameMs_; }

  /// Timing for the most recent completed `endFrame` call.
  [[nodiscard]] const EditorWindowFrameTiming& lastEndFrameTiming() const {
    return lastEndFrameTiming_;
  }

  /// Install a GLFW user pointer on the wrapped window.
  void setUserPointer(void* pointer);

  /// Replace the GLFW scroll callback, returning the previous callback.
  [[nodiscard]] GLFWscrollfun setScrollCallback(GLFWscrollfun callback);

  /// Raw GLFW window handle. Exposed for advanced use cases (custom key
  /// bindings, drag-and-drop setup). The main MVP binary doesn't need it.
  [[nodiscard]] GLFWwindow* rawHandle() const { return window_; }

  /// Shared Geode/WebGPU device for renderer instances in Geode editor builds.
  [[nodiscard]] std::shared_ptr<geode::GeodeDevice> geodeDevice() const;

#ifdef DONNER_EDITOR_WGPU
  /// True when frames render into an offscreen WebGPU texture rather than a
  /// presentable window surface. That is the case for headless/offscreen Linux
  /// replay (GLFW's null platform) and whenever
  /// \ref EditorWindowOptions::forceOffscreenRenderTarget was requested.
  /// False in OpenGL builds and before the WebGPU device came up.
  [[nodiscard]] bool usingOffscreenRenderTarget() const;

  /// Shared Geode device for direct append passes into the editor framebuffer.
  [[nodiscard]] std::shared_ptr<geode::GeodeDevice> geodeFramebufferDevice() const;

  /// Set the direct framebuffer underlay callback for the next and subsequent frames.
  void setWgpuUnderlayRenderCallback(WgpuUnderlayRenderCallback callback);

  /// Set the direct framebuffer overlay callback for the next and subsequent frames.
  /// The callback renders above the document underlay and below ImGui UI.
  void setWgpuDirectRenderCallback(WgpuDirectRenderCallback callback);
#endif

private:
  struct WgpuState;

  void beginFrameImpl(const EditorWindowInputOverride* inputOverride);
  void endFrameImpl(svg::RendererBitmap* readback);

  EditorWindowOptions options_;
  GLFWwindow* window_ = nullptr;
  std::unique_ptr<WgpuState> wgpuState_;
#ifdef DONNER_EDITOR_WGPU
  WgpuUnderlayRenderCallback wgpuUnderlayRenderCallback_;
  WgpuDirectRenderCallback wgpuDirectRenderCallback_;
#endif
  uint32_t textureId_ = 0;
  int textureWidth_ = 0;
  int textureHeight_ = 0;
  UiScaleConfig uiScaleConfig_;
  EditorWindowFonts editorFonts_;
  double lastBeginFrameMs_ = 0.0;
  EditorWindowFrameTiming lastEndFrameTiming_;
  /// When > 0, the content scale to force into ImGui's `DisplayFramebufferScale`
  /// every frame because the windowless null platform reports no HiDPI scale of
  /// its own (and `ImGui_ImplGlfw_NewFrame` would otherwise reset it to 1).
  double frameDisplayScaleOverride_ = 0.0;
  bool valid_ = false;
  bool glUnavailable_ = false;
  bool imguiInitialized_ = false;
#ifdef __EMSCRIPTEN__
  /// Cross-thread wake gate for the event-driven Wasm main loop.
  std::atomic_bool wasmFrameRequested_{true};
#endif
};

}  // namespace donner::editor::gui
