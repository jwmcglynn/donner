#pragma once
/// @file
/// Main-thread bridges for the single-canvas architecture whole-app-worker build.
///
/// In that build `main()` runs on a pthread (Emscripten `PROXY_TO_PTHREAD`) and
/// the single `#canvas` is transferred to it as an `OffscreenCanvas`. `EM_JS`
/// bodies run in the JS context of the *calling* thread, so every `window`,
/// `document`, and `navigator` reference in the shipping editor's `EM_JS`
/// probes evaluates in a `DedicatedWorkerGlobalScope` where those globals do
/// not exist. This header owns the replacements.
///
/// Two mechanisms, chosen per seam by how often it is read:
///
/// - **Shared-memory mirror.** Values the app thread reads every frame (CSS
///   viewport size, device pixel ratio, the frame-request flag, the wheel zoom
///   modifier shadow) are published by main-thread DOM listeners writing
///   directly into linear memory, which is shared across all pthreads. Reads on
///   the app thread are ordinary loads. A synchronous proxied call per read
///   would cost a round trip on every frame, which is exactly the budget this
///   phase is trying to measure.
/// - **Async proxied JS.** Values the page must observe but the app thread only
///   writes (the `window.__donner*` probe surface) go through
///   `MAIN_THREAD_ASYNC_EM_ASM`, which posts to the main thread without
///   blocking the app thread.
///
/// Nothing here is compiled unless `DONNER_EDITOR_WHOLE_APP_WORKER` is defined,
/// so the shipping package is unaffected.

#ifdef DONNER_EDITOR_WHOLE_APP_WORKER

#include <cstdint>

namespace donner::editor::whole_app_worker {

/**
 * Give the app pthread's JS context the `window` and `document` globals the
 * editor's `EM_JS` probe bodies assume.
 *
 * STUB, and the largest known gap in this build. About thirty `EM_JS`
 * bodies across the editor publish diagnostics onto `window.__donner*` or poke
 * DOM elements; on the app thread those globals do not exist and the first such
 * call throws `ReferenceError: window is not defined`, killing the thread.
 * Rather than convert every one of them to a proxied bridge for an experiment,
 * this installs a worker-local stand-in so they run harmlessly.
 *
 * Consequence: those probes land on the worker's own global and are NOT
 * readable from the page. The probes this build's exit criteria depend on are
 * explicitly bridged instead (see the rest of this header) and DO reach the
 * page. Probes known to be worker-local in this build: `__donnerWorkerStats`,
 * `__donnerSampleThumbnailStats`, `__donnerInteractionStats`,
 * `__donnerLayerThumbnailStats`, `__donnerPresentationResourceStats`,
 * `__donnerActiveSampleStats`, and the `data-active-sample-id` canvas
 * attribute. Query-param features (`?wgpuReadbackStats`,
 * `?workerSurfaceDiagnostic`) read an empty search string and stay off.
 *
 * CSS cursors are NOT in that set: a cursor is state the user sees, not a
 * probe, so RotateCursorSet keeps its registry and its writes on the browser
 * main thread rather than routing them through the stand-in below. Its
 * `__donnerBrowserCursorStats` therefore reaches the page as well.
 *
 * Must run before any other editor code on the app thread. Cheap and
 * thread-local; makes no cross-thread call.
 */
void InstallWorkerGlobalShim();

/**
 * Install the main-thread listeners that feed the shared-memory mirror, and
 * seed the page's probe surface.
 *
 * Must be called once from the app thread before the first frame. Blocks on one
 * synchronous proxied call; every subsequent read is a plain memory load.
 */
void Install();

/// CSS-pixel width of the browser viewport, as last published by the main
/// thread. 1 until \ref Install has run.
[[nodiscard]] int CssWidth();

/// CSS-pixel height of the browser viewport. 1 until \ref Install has run.
[[nodiscard]] int CssHeight();

/// `window.devicePixelRatio`, as last published by the main thread.
[[nodiscard]] double DevicePixelRatio();

/// Backing-store width the transferred canvas should have, resizing the
/// `OffscreenCanvas` to match when the CSS size or device pixel ratio moved.
///
/// The shipping build lets emscripten-glfw size the canvas from the main
/// thread. That path throws on a transferred canvas, so the app thread owns
/// sizing here instead, through `emscripten_set_canvas_element_size`.
[[nodiscard]] int CanvasBackingWidth();

/// Backing-store height the transferred canvas should have. See
/// \ref CanvasBackingWidth.
[[nodiscard]] int CanvasBackingHeight();

/// Take the pending "a DOM event asked for a frame" flag, clearing it.
[[nodiscard]] bool ConsumeFrameRequest();

/// Whether the last wheel event on the canvas carried Ctrl or Meta. Mirrors the
/// shipping build's `__donnerWheelModifierCapture` shadow, which exists because
/// the browser reports a trackpad pinch as a Ctrl-flagged wheel.
[[nodiscard]] bool ZoomModifierHeld();

/// Publish one frame-loop sample onto the page's `__donnerFrameLoopStats`.
///
/// Also publishes the frame's ASYNCIFY suspend attribution (see
/// `donner/base/AsyncifySuspendProbe.h`) onto `__donnerAsyncifySuspendStats`,
/// the frame driver's inter-tick interval onto `__donnerFrameTickStats`, and
/// the frame's byte attribution (see `donner/base/MemoryAttribution.h`) onto
/// `__donnerMemoryStats`.
///
/// @param triggerBits Bit 0 worker, bit 1 browser input, bit 2 idle timer.
/// @param frameMs Wall time the frame body took, in milliseconds.
/// @param callbacks Main-loop callbacks observed since the last sample.
void RecordFrameSample(int triggerBits, double frameMs, int callbacks);

/// How the app thread's frame loop is being driven.
enum class FrameDriver : int {
  /// Emscripten's `EM_TIMING_RAF` scheduler bound to the worker's own
  /// `requestAnimationFrame`. Vsync-aligned with no cross-thread hop.
  WorkerRequestAnimationFrame = 0,
  /// A browser-main-thread `requestAnimationFrame` loop proxying one task per
  /// vsync to the app thread. Vsync-aligned at the cost of one postMessage.
  ProxiedMainThreadRequestAnimationFrame = 1,
  /// Emscripten's `setTimeout`-based `fakeRequestAnimationFrame`. Not
  /// vsync-aligned; only reached if the proxied pump could not be installed.
  SetTimeoutFallback = 2,
};

/// Whether this worker's global scope exposes `requestAnimationFrame`.
///
/// Emscripten's `EM_TIMING_RAF` scheduler calls `globalThis.requestAnimationFrame`
/// when it exists and silently degrades to a `setTimeout` emulation when it does
/// not, which is what a dedicated worker got on the engines we measured.
/// The answer is per-engine, so it is probed at runtime rather than assumed.
[[nodiscard]] bool WorkerRequestAnimationFrameAvailable();

/// Install the vsync-aligned frame driver and return which arm was selected.
///
/// Call instead of `emscripten_set_main_loop_arg`: on the worker-rAF arm this
/// installs exactly that main loop, and on the proxied arm it installs a
/// main-thread rAF pump that hands one task per vsync to @p frameFn on the
/// calling thread. @p frameFn must be safe to re-enter-guard itself; the driver
/// does not serialize.
FrameDriver InstallFrameDriver(void (*frameFn)(void*), void* userData);

/// Interval statistics for the frame driver's ticks, in milliseconds.
struct FrameTickStats {
  /// Ticks observed since boot.
  std::uint64_t ticks = 0;
  /// Median inter-tick interval.
  double p50Ms = 0.0;
  /// 99th-percentile inter-tick interval, the jitter figure that decides
  /// whether an arm is really vsync-aligned.
  double p99Ms = 0.0;
  /// Largest inter-tick interval observed.
  double maxMs = 0.0;
};

/// Inter-tick interval statistics for the installed frame driver.
[[nodiscard]] FrameTickStats TickStats();

/// Publish the C++-owned pinch policy constant for the page's gesture bridge.
void PublishPinchZoomPolicy(double wheelDeltaPerLnScale);

/// Publish the size of the ImGui draw data the host frame is about to upload.
///
/// This is the workload behind the largest permanently-held blocks in the heap:
/// the backend keeps one host-side `ImDrawVert` array per frame in flight and
/// only ever grows them, so the peak vertex count of any single frame is
/// retained, times the frames-in-flight count, for the rest of the session. A
/// byte figure alone cannot say whether that is a leak or an honest cost; the
/// vertex count can.
void PublishImGuiDrawStats(int vertexCount, int indexCount, int commandListCount);

/// Publish the host present's internal split for one frame, with running
/// per-stage sums so a reader can take a mean over the run.
///
/// The frame graph already knows this breakdown; it just never left the
/// process. Without it "the UI frame costs 14 ms" is a number with no
/// addressee, and the question the whole-app worker has to answer - is a frame
/// with unchanged tiles pure blits, or is it re-uploading and re-snapshotting -
/// cannot be asked from the page.
void PublishHostFrameTiming(double endFrameMs, double imguiRenderMs, double surfaceAcquireMs,
                            double underlayMs, double imguiDrawMs, double directMs,
                            double readbackMs, double presentMs);

/// Tell the page the first frame reached the canvas, so the loader can hide.
void NotifyFirstFramePresented(int headlessDeviceCreations);

/// Mirror one scroll event onto `window.__donnerLastScrollEvent` for the
/// browser suites.
void RecordScrollDebug(bool zoomModifierHeld, double xoffset, double yoffset, bool physicalKeyHeld);

/// WebGPU readback diagnostic handshake. Same page contract as the pre-worker
/// build (wgpuReadbackStats URL parameter, window counters, explicit requests);
/// the request/completed ids additionally ride the shared-memory mirror so the
/// app thread polls without a main-thread round trip.
[[nodiscard]] bool ReadbackStatsEnabled();
[[nodiscard]] int PeekReadbackRequest();
void WakeForPendingReadback();
void MarkReadbackCaptureStarted(int requestId);
void PublishReadbackFailure(int requestId);
void PublishReadbackStats(int renderSamples, int renderColored, int renderNonBlack,
                          int renderMaxChannel, int layerSamples, int layerColored,
                          int layerNonBlack, int layerMaxChannel, int selectionChromePixels,
                          int requestId);
void PublishCarouselThumbnailStats(const int* values, int count);

}  // namespace donner::editor::whole_app_worker

#endif  // DONNER_EDITOR_WHOLE_APP_WORKER
