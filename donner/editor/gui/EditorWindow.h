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
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#ifdef DONNER_EDITOR_WGPU
#include "donner/gpu/Descriptors.h"
#include "donner/gpu/Handles.h"
#endif

#include "donner/base/Vector2.h"
#include "donner/svg/renderer/RendererInterface.h"

struct GLFWwindow;
struct ImFont;
using GLFWscrollfun = void (*)(GLFWwindow*, double, double);

namespace donner::geode {
class GeodeDevice;
class GeodeGpuRoot;
}  // namespace donner::geode

namespace donner::editor::gui {

namespace internal {

#if defined(__linux__) && !defined(__EMSCRIPTEN__)
/// Acquires one process-wide GLFW claim for a test-owned companion window.
bool AcquireGlfwRuntimeForTesting();
/// Releases a test-owned GLFW claim after its companion window is destroyed.
void ReleaseGlfwRuntimeForTesting();
/// Number of actual glfwTerminate calls made by the editor's runtime manager.
uint64_t GlfwTerminationCountForTesting();
#endif

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

#ifdef DONNER_EDITOR_WGPU
/// What the frame loop does about a status its presentation surface reported.
enum class SurfaceFrameAction {
  Draw,                 //!< The acquired frame is usable; draw into it.
  Skip,                 //!< No frame this time; the next one tries again.
  ReconfigureAndRetry,  //!< Follow the window with a new configuration and acquire again.
  Release,              //!< The surface can serve no further frames and is given up.
};

/// Ostream output operator. @param os Output stream. @param value Action to output.
inline std::ostream& operator<<(std::ostream& os, SurfaceFrameAction value) {
  switch (value) {
    case SurfaceFrameAction::Draw: return os << "Draw";
    case SurfaceFrameAction::Skip: return os << "Skip";
    case SurfaceFrameAction::ReconfigureAndRetry: return os << "ReconfigureAndRetry";
    case SurfaceFrameAction::Release: return os << "Release";
  }
  return os << "Unknown";
}

/// Routes a status the presentation surface reported to what the frame loop does about it.
///
/// The failures are kept apart because their recoveries differ. A configuration that no longer
/// matches its window is followed with a new one, which is the operation a resize already
/// performs; a platform object that is gone and a lost device recover by neither, so the surface
/// is given up rather than acquired from again; and nothing becoming available in time is simply
/// a frame not drawn.
///
/// @param status Status the surface reported while handing over, or refusing, a frame.
[[nodiscard]] constexpr SurfaceFrameAction SurfaceFrameActionFor(
    gpu::SurfaceStatus status) noexcept {
  switch (status) {
    case gpu::SurfaceStatus::Success: return SurfaceFrameAction::Draw;
    case gpu::SurfaceStatus::Outdated: return SurfaceFrameAction::ReconfigureAndRetry;
    case gpu::SurfaceStatus::Lost:
    case gpu::SurfaceStatus::DeviceLost: return SurfaceFrameAction::Release;
    case gpu::SurfaceStatus::Timeout: break;
  }
  return SurfaceFrameAction::Skip;
}

/**
 * Picks how a surface composites its alpha channel, from what it reports it supports.
 *
 * Never answers \p preferred when the surface did not offer it: a transparent clear on a surface
 * composited as opaque presents as solid black, and the window clears to its page background
 * instead once it can see that the alpha channel is not honored.
 *
 * @param modes Alpha compositing the surface reported it supports.
 * @param preferred Compositing this platform's window wants. A desktop window is opaque; a
 *   browser canvas is composited over the page's own background, so premultiplied alpha is what
 *   lets uncovered pixels stay transparent there.
 * @return \p preferred when it is on offer, and the first mode offered otherwise. A surface that
 *   named none is composited opaque, which every surface does and which the window's fallback
 *   clear color already assumes.
 */
[[nodiscard]] gpu::SurfaceAlphaMode ChooseSurfaceAlphaMode(
    const std::vector<gpu::SurfaceAlphaMode>& modes, gpu::SurfaceAlphaMode preferred);

/// One frame's texture and what the surface reported while handing it over.
///
/// The texture is borrowed for the length of one frame and no longer. It names the frame the
/// platform handed out; presenting that frame, handing it back, reconfiguring the surface, or
/// giving the surface up all end it, and the handle goes stale at the same moment, so a use
/// afterwards is refused instead of reaching a frame the platform has taken back. Holding the
/// handle past its frame keeps nothing alive.
///
/// A non-success status can still carry a usable texture: a surface whose configuration has
/// drifted out of date usually still presents, so drawing this frame or following the window
/// first is the caller's decision.
struct AcquiredFrame {
  gpu::Texture texture;  //!< This frame's texture; invalid when no frame came back.
  gpu::SurfaceStatus status = gpu::SurfaceStatus::Success;  //!< What the surface reported.
};

/**
 * Where each frame's texture is acquired from, and what the finished frame is handed back to.
 *
 * The frame loop drives presentation entirely through this interface, so the difference between
 * the platforms lives in the single place that picks an implementation instead of spreading
 * through the loop.
 *
 * Setup is staged because the pieces it needs appear at different points of window bringup: the
 * platform object exists before there is an adapter, adapter selection may need to be constrained
 * to the surface, the renderer compiles its pipelines for the texture format before there is a
 * device, and a surface built on the GPU runtime cannot exist until the device does.
 *
 * On Apple the platform object is a Core Animation Metal layer, which presents from any Metal
 * device the system reports and belongs to no instance, so it constrains no selection and the
 * window attaches it before selecting one.
 */
class PresentationSurface {
public:
  virtual ~PresentationSurface() = default;

  /**
   * Takes hold of the platform object behind \p window.
   *
   * @param window Window whose platform object frames are presented to.
   * @return False when the platform object could not be obtained.
   */
#ifndef __EMSCRIPTEN__
  [[nodiscard]] virtual bool attachToWindow(GLFWwindow* window) = 0;

  /**
   * Settles the format acquired textures carry, and records whether finished frames are to be
   * copied back. Runs before the device exists, because the renderer compiles its pipelines for
   * \ref format; everything else a frame carries is settled by \ref attachToDevice, which is the
   * first point the surface can say what it supports.
   *
   * @param enableReadback Whether finished frames are copied back to the host.
   * @return False when the surface cannot serve the editor's frames.
   */
  [[nodiscard]] virtual bool chooseConfiguration(bool enableReadback) = 0;
#endif

  /**
   * Finishes setup against the device whose queue draws the frames, narrowing what was asked for
   * to what the surface reports it can do. \ref usage and \ref premultipliedAlpha describe the
   * frames this surface hands out only once this has succeeded.
   *
   * @param device Device wrapper the editor renders its frames with.
   * @return False when the surface could not be completed against it.
   */
  [[nodiscard]] virtual bool attachToDevice(geode::GeodeDevice& device) = 0;

  /**
   * Points the surface at a framebuffer of \p width by \p height texels, replacing any previous
   * configuration and invalidating any acquired frame.
   *
   * This is how a surface follows its window, so a resize is a new configuration rather than a
   * new surface, and it is equally the recovery from a configuration that has drifted out of
   * date.
   *
   * @param width Framebuffer width in texels.
   * @param height Framebuffer height in texels.
   * @return False when the surface refused the configuration.
   */
  [[nodiscard]] virtual bool configure(int width, int height) = 0;

  /// Acquires this frame's texture.
  [[nodiscard]] virtual AcquiredFrame acquire() = 0;

  /// Hands the acquired frame to the platform. Does nothing when no frame is held, so the frame
  /// loop can present unconditionally on its way out.
  virtual void present() = 0;

  /// Releases the acquired frame without showing it, for a frame the caller decided not to draw.
  virtual void abandon() = 0;

  /// Gives up the configuration and any resources held for it. The surface serves no further
  /// frames.
  virtual void shutdown() = 0;

  /// Format acquired textures carry.
  [[nodiscard]] virtual gpu::TextureFormat format() const = 0;

  /// Usage flags acquired textures carry.
  [[nodiscard]] virtual gpu::TextureUsage usage() const = 0;

  /// Whether the surface composites its alpha channel premultiplied rather than ignoring it.
  [[nodiscard]] virtual bool premultipliedAlpha() const = 0;
};

/**
 * Presents through the GPU runtime's surface hooks, which is how every platform the editor runs
 * on presents.
 *
 * What differs between platforms is the platform object frames go to. A Core Animation Metal
 * layer is named directly. Native Vulkan borrows the GLFW surface selected before the logical
 * device exists. The browser backend names the transferred canvas by selector. The transitional
 * adapter uses a window-library WebGPU surface to constrain adapter selection.
 */
class RuntimePresentationSurface final : public PresentationSurface {
public:
  /// Constructs a surface that is not attached to anything yet.
  RuntimePresentationSurface() = default;

  /// Names the editor's transferred browser canvas without creating a WebGPU-C++ surface.
  /// @param format Preferred canvas format chosen before Geode pipelines are compiled.
  /// @param enableReadback Whether diagnostic pixel reads may use the acquired frame.
  RuntimePresentationSurface(gpu::TextureFormat format, bool enableReadback);

#if defined(__linux__) && !defined(__EMSCRIPTEN__)
  /// Names a GLFW-created VkSurfaceKHR without taking platform ownership from the window.
  /// @param surfaceHandle Native surface bits scoped to the selected Vulkan instance.
  /// @param format Format selected from that physical device's surface capabilities.
  /// @param enableReadback Whether diagnostic readback may use the frame.
  void attachNativeVulkanSurface(uint64_t surfaceHandle, gpu::TextureFormat format,
                                 bool enableReadback);
#endif

  /// Hands back any frame still outstanding and gives up the surface.
  ~RuntimePresentationSurface() override;

#ifndef __EMSCRIPTEN__
  bool attachToWindow(GLFWwindow* window) override;
  bool chooseConfiguration(bool enableReadback) override;
#endif
  bool attachToDevice(geode::GeodeDevice& device) override;

  /**
   * Builds the surface on \p device for the platform object \p native names, and narrows what was
   * asked for to what the surface reports it can do.
   *
   * This is the step \ref attachToDevice performs once it has resolved the runtime from a Geode
   * context, and the platform object and settled configuration from the window. Callers that
   * already hold all of them - a host embedding the editor, or a test standing in for a window -
   * reach it directly.
   *
   * @param device Runtime frames are acquired from and presented through. Must outlive this.
   * @param native Platform object frames are presented to.
   * @param format Format the renderer's pipelines were compiled for. A surface that does not
   *   present it cannot serve the editor's frames.
   * @param enableReadback Whether finished frames are copied back to the host. Dropped when the
   *   surface reports its frames cannot be copied from.
   * @return False when the surface could not be built or cannot serve the editor's frames.
   */
  [[nodiscard]] bool attachToRuntime(gpu::Device& device, gpu::NativeSurfaceHandle native,
                                     gpu::TextureFormat format, bool enableReadback);

  bool configure(int width, int height) override;
  AcquiredFrame acquire() override;
  void present() override;
  void abandon() override;
  void shutdown() override;
  gpu::TextureFormat format() const override;
  gpu::TextureUsage usage() const override;
  bool premultipliedAlpha() const override;

private:
  /// Narrows the configuration this surface was asked for to what it reported it can do, or
  /// reports that it cannot serve the editor's frames at all.
  /// @param capabilities What the surface reported.
  [[nodiscard]] bool applyCapabilities(const gpu::SurfaceCapabilities& capabilities);

  /// Usage acquired textures are configured to carry.
  [[nodiscard]] gpu::TextureUsage configuredUsage() const;

  /// Hands back any outstanding frame, gives the runtime's surface up, and lets go of the
  /// platform object, in that order.
  void release();

  gpu::Device* device_ = nullptr;
  gpu::Surface surface_;
  /// Platform object frames are presented to, filled in while attaching to the window.
  gpu::NativeSurfaceHandle native_;
  gpu::TextureFormat format_ = gpu::TextureFormat::BGRA8Unorm;
  gpu::SurfaceAlphaMode alphaMode_ = gpu::SurfaceAlphaMode::Opaque;
  /// Whether finished frames are copied back to the host.
  bool readback_ = false;
  /// Whether the platform is holding a frame this surface handed out.
  bool hasAcquiredFrame_ = false;
};

/// What acquiring one frame produced, and what the window must do about it.
struct PresentationFrameOutcome {
  gpu::Texture texture;  //!< Frame to draw into; invalid when there is no frame this time.
  gpu::SurfaceStatus status = gpu::SurfaceStatus::Success;  //!< What the surface last reported.
  double acquireMs = 0.0;  //!< Wall time the acquire (including any retry) took.
  bool released = false;   //!< The surface was given up; the window holds none any more.
  /// Report a device loss through the window's existing renderer-failure path.
  bool markDeviceLost = false;
};

/**
 * Acquires the frame to draw, recovering from whatever the surface reports on the way.
 *
 * Each status recovers the way \ref SurfaceFrameActionFor says it does, and the recoveries that
 * can succeed are attempted here rather than costing the frame. A configuration that has drifted
 * out of date is followed to the current extent and the frame acquired again; a surface whose
 * platform object is gone is rebuilt from the window that still holds a handle to make a new one
 * from, and the frame acquired from the replacement. Each is attempted once, so a status that
 * repeats settles instead of looping.
 *
 * A lost device is not rebuilt: nothing here recovers it, so it gives the surface up and asks
 * the caller to report the loss.
 *
 * @param surface Surface to acquire from. Replaced when a lost one is rebuilt, and cleared when
 *   the surface is given up.
 * @param sizePx Framebuffer extent in pixels.
 * @param configuredPx Extent the surface is configured for; updated as the surface follows the
 *   window, and zeroed when a configuration is refused so the next frame tries again.
 * @param rebuild Builds a replacement surface for \p sizePx from the window the lost one came
 *   from, fully configured and ready to acquire from, or null when none could be built.
 */
[[nodiscard]] PresentationFrameOutcome AcquirePresentationFrame(
    std::unique_ptr<PresentationSurface>& surface, Vector2i sizePx, Vector2i& configuredPx,
    const std::function<std::unique_ptr<PresentationSurface>()>& rebuild);

/// Buckets a status the presentation surface reported into the retry classes the event-driven
/// Wasm loop distinguishes.
///
/// A surface that is out of date and one whose platform object is gone share a bucket because
/// the browser recovers from both the same way: reconfigure and ask for another frame. Anything
/// else is terminal for the frame and must not rearm the loop.
///
/// @param status Status the surface reported.
[[nodiscard]] constexpr WgpuSurfaceFailureKind WgpuSurfaceFailureKindFor(
    gpu::SurfaceStatus status) noexcept {
  switch (status) {
    case gpu::SurfaceStatus::Timeout: return WgpuSurfaceFailureKind::Timeout;
    case gpu::SurfaceStatus::Outdated:
    case gpu::SurfaceStatus::Lost: return WgpuSurfaceFailureKind::OutdatedOrLost;
    case gpu::SurfaceStatus::Success:
    case gpu::SurfaceStatus::DeviceLost: break;
  }
  return WgpuSurfaceFailureKind::Fatal;
}
#endif

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
  /// They are additionally created undecorated: a titled window's frame is
  /// constrained to the display it lands on, and a silently shrunk replay
  /// window changes the editor's layout. See the constructor for details.
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
  /// This frame's target, named on the framebuffer device. Borrowed: the frame keeps the texture
  /// alive, and the name is valid only for the duration of the callback.
  const gpu::Texture& texture;
  /// Framebuffer dimensions in physical pixels.
  Vector2i framebufferSizePx = Vector2i::Zero();
  /// Physical framebuffer pixels per ImGui logical pixel for this frame.
  Vector2d framebufferFromLogicalScale = Vector2d(1.0, 1.0);
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

  /// Maintain the GPU contexts owned by this event-loop thread without drawing another frame.
  /// The render worker maintains its own context on that worker thread.
  void pollIdleGpu();

  /// Whether a completion timer should wake this event loop for another cheap idle poll.
  [[nodiscard]] bool hasIdleGpuWork() const;

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

  /// Whether \ref endFrameAndReadPixels returns this window's frames on a live device: readback
  /// was asked for, through \ref EditorWindowOptions::enableFramebufferReadback or the browser's
  /// readback diagnostic, and the frames this window draws into can be copied from. A surface
  /// that reports its frames cannot be copied from still presents them, and the window drops the
  /// readback rather than the surface, so its frames then read back empty. False before the
  /// device came up.
  [[nodiscard]] bool framebufferReadbackAvailable() const;

  /// Shared Geode device for direct append passes into the editor framebuffer.
  [[nodiscard]] std::shared_ptr<geode::GeodeDevice> geodeFramebufferDevice() const;

  /// Set the direct framebuffer underlay callback for the next and subsequent frames.
  void setWgpuUnderlayRenderCallback(WgpuUnderlayRenderCallback callback);

  /// Set the direct framebuffer overlay callback for the next and subsequent frames.
  /// The callback renders above the document underlay and below ImGui UI.
  void setWgpuDirectRenderCallback(WgpuDirectRenderCallback callback);

  /**
   * Test seam: bounds how long \ref endFrameAndReadPixels waits for its readback map, in place of
   * the editor's readback bound, so a case can reach the bound without spending it. A map that
   * outlasts the bound declares the framebuffer device lost either way.
   *
   * @param budget Longest the map may take. Clamped to the editor's bound; zero or less restores
   *   it.
   */
  void setFramebufferReadbackBudgetForTesting(std::chrono::milliseconds budget);
#endif

private:
  struct WgpuState;

  /// Releases the native window and its process-wide GLFW claim after GPU surface retirement.
  void closeWindow();

#ifdef DONNER_EDITOR_WGPU
  /// Selects this window's runtime root and settles its presentation format before pipelines.
  /// @param offscreen Whether this window draws into its own texture.
  /// @param enableReadback Whether copied frame pixels are requested.
  std::shared_ptr<geode::GeodeGpuRoot> selectGpuRootForWindow(bool offscreen, bool enableReadback);
#endif

  void beginFrameImpl(const EditorWindowInputOverride* inputOverride);
  void endFrameImpl(svg::RendererBitmap* readback);

#ifdef DONNER_EDITOR_WGPU
  /**
   * Acquires this frame's texture from the presentation surface, routing what the surface
   * reported: a configuration that has drifted out of date is followed and the frame retried
   * once, and a surface that can serve no further frames is given up.
   *
   * @param framebufferWidth Framebuffer width in pixels.
   * @param framebufferHeight Framebuffer height in pixels.
   * @param timing Frame timing to record the acquire cost into.
   * @param[out] status What the surface reported for the frame that is returned, or for the
   *   frame that never came.
   * @return This frame's texture, or an invalid texture when there is no frame to draw. The
   *   handle is the frame's; see \ref internal::AcquiredFrame for how long it lasts.
   */
  [[nodiscard]] gpu::Texture acquirePresentationFrame(int framebufferWidth, int framebufferHeight,
                                                      EditorWindowFrameTiming& timing,
                                                      gpu::SurfaceStatus& status);

  /// Builds a replacement presentation surface from this window, configured for the given
  /// framebuffer and ready to acquire from, or null when one could not be built.
  /// @param framebufferWidth Framebuffer width in pixels.
  /// @param framebufferHeight Framebuffer height in pixels.
  [[nodiscard]] std::unique_ptr<internal::PresentationSurface> rebuildPresentationSurface(
      int framebufferWidth, int framebufferHeight);

  /// Brings whatever this window draws into in line with \p displayW by \p displayH, when the
  /// extent it was last configured for is not that. A presentable surface is reconfigured; a
  /// window without one reallocates its own target.
  /// @param displayW Framebuffer width in pixels. @param displayH Framebuffer height in pixels.
  /// @return Whether there is something to draw into at that extent.
  [[nodiscard]] bool configureFrameTarget(int displayW, int displayH);

  /// Draws everything that belongs below every ImGui surface: the frame is cleared to this
  /// window's clear color, then the document underlay and the selection and path chrome are
  /// drawn over it, in that order. A frame with neither callback set is left for the UI pass to
  /// clear.
  /// @param frameTarget Frame's color target, named on the framebuffer device, which is also
  ///   what the callbacks are handed.
  /// @param framebufferSizePx Framebuffer extent in pixels.
  /// @param framebufferFromLogicalScale Physical pixels per ImGui logical pixel this frame.
  /// @param hasUnderlay Whether the underlay callback is set.
  /// @param hasDirect Whether the chrome callback is set.
  /// @param timing Frame timing to record their costs in.
  /// @return Whether the frame is still drawable.
  [[nodiscard]] bool drawFrameBelowUi(const gpu::Texture& frameTarget, Vector2i framebufferSizePx,
                                      const Vector2d& framebufferFromLogicalScale, bool hasUnderlay,
                                      bool hasDirect, EditorWindowFrameTiming& timing);

  /// Records and submits the copy that puts this frame's pixels in \p buffer.
  /// @param frameTarget Frame's color target, named on the framebuffer device.
  /// @param buffer Destination on the framebuffer device, already sized for the copy.
  /// @param width Copy width in pixels. @param height Copy height in pixels.
  /// @param bytesPerRow Destination row pitch. @param timing Frame timing to record the cost in.
  /// @return Whether the copy was submitted.
  [[nodiscard]] bool recordFrameReadback(const gpu::Texture& frameTarget, const gpu::Buffer& buffer,
                                         uint32_t width, uint32_t height, uint32_t bytesPerRow,
                                         EditorWindowFrameTiming& timing);

  /// Waits for \p buffer, within the editor's bound for a readback map, and unpacks it into
  /// \p destination, leaving \p destination untouched when the map never completed. A map that
  /// outlasts the bound declares the framebuffer device lost.
  /// @param buffer Buffer the frame was copied into. @param byteSize Bytes to map.
  /// @param width Frame width in pixels. @param height Frame height in pixels.
  /// @param bytesPerRow Row pitch in \p buffer. @param destination Bitmap to fill.
  /// @param timing Frame timing to record the cost in.
  void readFrameReadback(const gpu::Buffer& buffer, uint64_t byteSize, uint32_t width,
                         uint32_t height, uint32_t bytesPerRow, svg::RendererBitmap* destination,
                         EditorWindowFrameTiming& timing);

  /// Records and submits this frame's UI draw data into \p frameTarget.
  /// @param frameTarget Frame's color target, named on the framebuffer device.
  /// @param framebufferSizePx Framebuffer extent in pixels.
  /// @param loadExisting Whether the target already holds content that must be preserved.
  /// @param timing Frame timing to record the cost in.
  /// @return Whether the draw data was submitted.
  [[nodiscard]] bool recordFrameUi(const gpu::Texture& frameTarget, Vector2i framebufferSizePx,
                                   bool loadExisting, EditorWindowFrameTiming& timing);
#else
  /// Draws this frame's UI through GL and swaps it in, reading it back first when asked.
  /// @param readback Bitmap to fill, or null. @param displayW Framebuffer width in pixels.
  /// @param displayH Framebuffer height in pixels. @param timing Frame timing to record into.
  void endFrameGl(svg::RendererBitmap* readback, int displayW, int displayH,
                  EditorWindowFrameTiming& timing);
#endif

  EditorWindowOptions options_;
  GLFWwindow* window_ = nullptr;
  bool glfwClaimed_ = false;
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
