#include "donner/editor/WholeAppWorkerBridge.h"

#ifdef DONNER_EDITOR_WHOLE_APP_WORKER

#include <emscripten.h>
#include <emscripten/em_asm.h>
#include <emscripten/heap.h>
#include <emscripten/html5.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <pthread.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <vector>

#include "donner/base/AsyncifySuspendProbe.h"
#include "donner/base/HeapSizeHistogram.h"
#include "donner/base/MemoryAttribution.h"

namespace donner::editor::whole_app_worker {

namespace {

/// Shared-memory mirror of main-thread-only browser state.
///
/// The field offsets are a contract with the JS installed by \ref Install: that
/// JS indexes `HEAP32` and `HEAPF64` directly off this object's address. Keep
/// the layout, the `alignas(8)`, and the JS index arithmetic in sync.
struct alignas(8) BrowserMirror {
  std::int32_t cssWidth;          // +0
  std::int32_t cssHeight;         // +4
  std::int32_t frameRequested;    // +8
  std::int32_t zoomModifierHeld;  // +12
  double devicePixelRatio;        // +16
  double mouseDownEpochMs;        // +24
  std::int32_t mouseDownSeq;         // +32
  std::int32_t readbackRequestId;    // +36 (page writes, worker reads)
  std::int32_t readbackCompletedId;  // +40 (worker writes)
  std::int32_t readbackEnabled;      // +44 (page writes once at install)
};
static_assert(sizeof(BrowserMirror) == 48,
              "BrowserMirror layout is a contract with Install()'s JS");

BrowserMirror& Mirror() {
  // Boot before the first main-thread publish should look like a 1x1 viewport
  // at scale 1 with a frame already requested, so the editor draws once and
  // then resizes, rather than dividing by zero.
  static BrowserMirror mirror{
      .cssWidth = 1,
      .cssHeight = 1,
      .frameRequested = 1,
      .zoomModifierHeld = 0,
      .devicePixelRatio = 1.0,
      .mouseDownEpochMs = 0.0,
      .mouseDownSeq = 0,
      .readbackRequestId = 0,
      .readbackCompletedId = 0,
      .readbackEnabled = 0,
  };
  return mirror;
}

[[nodiscard]] std::int32_t LoadRelaxed(std::int32_t& field) {
  return std::atomic_ref<std::int32_t>(field).load(std::memory_order_relaxed);
}

// The worker's `performance` has its own time origin, so a worker-side
// `performance.now()` is not comparable with a page-side one. Both contexts do
// agree on `timeOrigin + now()`, which is Unix-epoch milliseconds.
EM_JS(double, WorkerEpochNowMs, (), { return performance.timeOrigin + performance.now(); });

/// Sequence number of the most recent main-thread mousedown this thread has
/// already turned into a latency sample.
std::int32_t g_lastObservedMouseDownSeq = 0;

/// App-thread receipt of a proxied mousedown. Registered on the app thread with
/// `EM_CALLBACK_THREAD_CONTEXT_CALLING_THREAD`, so Emscripten registers the DOM
/// listener on the main thread and dispatches the event here. The delta against
/// the main thread's capture-phase timestamp is the full event round trip.
bool OnAppThreadMouseDown(int /*eventType*/, const EmscriptenMouseEvent* /*event*/,
                          void* /*userData*/) {
  BrowserMirror& mirror = Mirror();
  const std::int32_t seqBefore = LoadRelaxed(mirror.mouseDownSeq);
  const double dispatchedAtMs = mirror.mouseDownEpochMs;
  const std::int32_t seqAfter = LoadRelaxed(mirror.mouseDownSeq);
  if (seqBefore == 0 || seqBefore != seqAfter || seqBefore == g_lastObservedMouseDownSeq) {
    return false;
  }
  g_lastObservedMouseDownSeq = seqBefore;

  const double latencyMs = WorkerEpochNowMs() - dispatchedAtMs;
  MAIN_THREAD_ASYNC_EM_ASM(
      {
        const samples = window['__donnerInputLatencyMsSamples'];
        if (samples && samples.length < 4096) {
          samples.push($0);
        }
      },
      latencyMs);
  return false;
}

}  // namespace

// See InstallWorkerGlobalShim's contract in the header. Written as EM_JS (not
// EM_ASM) so the body's top-level commas survive the preprocessor.
// clang-format off
EM_JS(void, InstallWorkerGlobalShimImpl, (), {
  if (typeof window != 'undefined') {
    return;  // Running on the main thread; the real globals are present.
  }

  const noop = function() {};
  const emptyRect = function() {
    return {x : 0, y : 0, left : 0, top : 0, right : 0, bottom : 0, width : 0, height : 0};
  };
  // Enough of CSSStyleDeclaration for the editor's layout writers. Values are
  // stored and dropped: CSS on the page is main-thread state, and a write from
  // this thread cannot reach it. A writer whose effect the user sees has to
  // target the main thread directly instead, the way the editor's cursors do
  // (see header).
  const makeStyle = function() {
    const properties = {};
    const priorities = {};
    return {
      setProperty : function(name, value, priority) {
        properties[name] = value;
        priorities[name] = priority || '';
      },
      removeProperty : function(name) {
        const previous = properties[name] || '';
        delete properties[name];
        delete priorities[name];
        return previous;
      },
      getPropertyValue : function(name) { return properties[name] || ''; },
      getPropertyPriority : function(name) { return priorities[name] || ''; },
      item : function(index) { return Object.keys(properties)[index] || ''; },
      get length() { return Object.keys(properties).length; },
      cssText : '',
    };
  };
  const elements = {};
  const makeElement = function(id) {
    return {
      id : id,
      style : makeStyle(),
      dataset : {},
      hidden : false,
      classList : {add : noop, remove : noop, contains : function() { return false; }},
      setAttribute : noop,
      removeAttribute : noop,
      getAttribute : function() { return null; },
      addEventListener : noop,
      removeEventListener : noop,
      dispatchEvent : function() { return true; },
      focus : noop,
      blur : noop,
      getBoundingClientRect : emptyRect,
    };
  };
  const getElementById = function(id) {
    if (!elements[id]) {
      elements[id] = makeElement(id);
    }
    return elements[id];
  };

  globalThis.document = {
    activeElement : null,
    body : null,
    documentElement : makeElement('html'),
    title : '',
    fullscreenElement : null,
    pointerLockElement : null,
    hidden : false,
    visibilityState : 'visible',
    getElementById : getElementById,
    querySelector : function() { return null; },
    createElement : function(tag) { return makeElement(tag); },
    addEventListener : noop,
    removeEventListener : noop,
  };

  globalThis.window = {
    // Diagnostics land here rather than on the page; see the header.
    document : globalThis.document,
    location : {search : '', href : '', hash : ''},
    devicePixelRatio : 1,
    innerWidth : 1,
    innerHeight : 1,
    isSecureContext : true,
    performance : performance,
    addEventListener : noop,
    removeEventListener : noop,
    dispatchEvent : function() { return true; },
    matchMedia : function() {
      return {matches : false, addEventListener : noop, removeEventListener : noop};
    },
    getSelection : function() { return null; },
    getComputedStyle : function() { return makeStyle(); },
    open : function() { return null; },
    requestAnimationFrame : function(callback) { return setTimeout(callback, 0); },
    cancelAnimationFrame : function(handle) { clearTimeout(handle); },
  };

  // The transferred canvas is an OffscreenCanvas, which has no `style`,
  // `dataset`, or `focus`. Editor code that decorates the UI canvas expects an
  // HTMLCanvasElement, so give it the inert equivalents.
  const canvas = Module['canvas'];
  if (canvas && !canvas.style) {
    canvas.style = makeStyle();
    canvas.dataset = {};
    canvas.setAttribute = noop;
    canvas.getAttribute = function() { return null; };
    canvas.addEventListener = noop;
    canvas.removeEventListener = noop;
    canvas.focus = noop;
    canvas.getBoundingClientRect = emptyRect;
  }
});
// clang-format on

void InstallWorkerGlobalShim() {
  InstallWorkerGlobalShimImpl();
}

void Install() {
  BrowserMirror& mirror = Mirror();
  MAIN_THREAD_EM_ASM(
      {
        const base = $0;
        const i32 = base >> 2;
        const f64 = base >> 3;

        const publishViewport = function() {
          HEAP32[i32 + 0] = Math.max(1, Math.floor(window.innerWidth));
          HEAP32[i32 + 1] = Math.max(1, Math.floor(window.innerHeight));
          HEAPF64[f64 + 2] = window.devicePixelRatio || 1.0;
          HEAP32[i32 + 2] = 1;
        };
        publishViewport();
        window.addEventListener('resize', publishViewport, {passive : true});

        const requestFrame = function() {
          HEAP32[i32 + 2] = 1;
        };
        // Same capture-phase window listener set the shipping build installs;
        // see InitializeWasmEditorFrameScheduling in donner/editor/main.cc.
        for (const eventName of([
               'mousedown',
               'mouseup',
               'mousemove',
               'pointerdown',
               'pointerup',
               'pointermove',
               'pointercancel',
               'pointerenter',
               'pointerleave',
               'wheel',
               'contextmenu',
               'keydown',
               'keyup',
               'compositionstart',
               'compositionupdate',
               'compositionend',
               'beforeinput',
               'input',
               'paste',
               'resize',
               'focus',
               'blur'
             ])) {
          window.addEventListener(eventName, requestFrame, {capture : true, passive : true});
        }
        document.addEventListener('visibilitychange', requestFrame,
                                  {capture : true, passive : true});

        // The page and the browser suites still poke `__donnerEditorFrameRequested`
        // to force a frame. The app thread cannot see that property, so pump it
        // into the mirror from the main thread's own animation frame.
        window['__donnerEditorFrameRequested'] = true;
        const pump = function() {
          if (window['__donnerEditorFrameRequested']) {
            window['__donnerEditorFrameRequested'] = false;
            HEAP32[i32 + 2] = 1;
          }
          requestAnimationFrame(pump);
        };
        requestAnimationFrame(pump);

        // Wheel zoom-modifier shadow. macOS drops the keyup when Cmd is held
        // across a focus change, so clear on blur and on tab hide.
        const canvas = document.getElementById('canvas');
        const setModifier = function(event) {
          HEAP32[i32 + 3] = (event.ctrlKey || event.metaKey) ? 1 : 0;
        };
        const clearModifier = function() {
          HEAP32[i32 + 3] = 0;
        };
        if (canvas) {
          canvas.addEventListener('wheel', setModifier, {capture : true, passive : true});
          // preventDefault mirror. The app consumes every wheel over the
          // canvas (pan and zoom), and a ctrl+wheel left to the browser
          // triggers page zoom. The real consumer runs on the app pthread and
          // cannot veto the default in time, so the page mirrors the decision
          // the app always makes for canvas wheels. Non-passive on purpose.
          canvas.addEventListener('wheel', function(event) { event.preventDefault(); },
                                  {passive : false});
        }
        window.addEventListener('blur', clearModifier);
        document.addEventListener('visibilitychange', clearModifier);

        // Event round-trip probe: capture phase, so this runs before any
        // Emscripten proxying of the same event.
        window.addEventListener('mousedown',
                                function() {
                                  HEAPF64[f64 + 3] = performance.timeOrigin + performance.now();
                                  HEAP32[i32 + 8] = (HEAP32[i32 + 8] | 0) + 1;
                                },
                                {capture : true, passive : true});

        // WebGPU readback diagnostic handshake, same page contract as the
        // pre-worker build: enabled by the wgpuReadbackStats URL parameter,
        // one seeded capture proves the path, further captures are explicit.
        // The request id rides the shared-memory mirror so the app thread can
        // poll it without a main-thread round trip; the window counters stay
        // authoritative for the suites that read them.
        const readbackEnabled =
            new URLSearchParams(window.location.search).has('wgpuReadbackStats');
        HEAP32[i32 + 11] = readbackEnabled ? 1 : 0;
        if (readbackEnabled) {
          window['__donnerWgpuReadbackRequested'] = 1;
          window['__donnerWgpuReadbackCompleted'] = 0;
          window['__donnerWgpuReadbackCaptureStarts'] = 0;
          window['__donnerWgpuReadbackCaptureCompletions'] = 0;
          window['__donnerWgpuReadbackCaptureFailures'] = 0;
          HEAP32[i32 + 9] = 1;
          window['__donnerRequestWgpuReadback'] = function() {
            const request = Number(window['__donnerWgpuReadbackRequested'] || 0) + 1;
            window['__donnerWgpuReadbackRequested'] = request;
            HEAP32[i32 + 9] = request;
            HEAP32[i32 + 2] = 1;
            window['__donnerEditorFrameRequested'] = true;
            return request;
          };
        }

        window['__donnerMainLoopRenderedFrames'] = 0;
        if (!window['__donnerFrameLoopStats']) {
          window['__donnerFrameLoopStats'] = ({
            'callbacks' : 0,
            'renderedFrames' : 0,
            'inputTriggeredFrames' : 0,
            'workerTriggeredFrames' : 0,
            'timerTriggeredFrames' : 0,
            'workerOnlyFrames' : 0,
            'uiFrameMsSamples' : [],
          });
        }
      },
      &mirror);

  emscripten_set_mousedown_callback_on_thread(EMSCRIPTEN_EVENT_TARGET_WINDOW, /*userData=*/nullptr,
                                              /*useCapture=*/EM_TRUE, &OnAppThreadMouseDown,
                                              EM_CALLBACK_THREAD_CONTEXT_CALLING_THREAD);
}

void StoreRelaxed(std::int32_t& field, std::int32_t value) {
  std::atomic_ref<std::int32_t>(field).store(value, std::memory_order_relaxed);
}

bool ReadbackStatsEnabled() {
  return LoadRelaxed(Mirror().readbackEnabled) != 0;
}

int PeekReadbackRequest() {
  BrowserMirror& mirror = Mirror();
  const std::int32_t request = LoadRelaxed(mirror.readbackRequestId);
  const std::int32_t completed = LoadRelaxed(mirror.readbackCompletedId);
  return request > completed ? request : 0;
}

void WakeForPendingReadback() {
  BrowserMirror& mirror = Mirror();
  if (LoadRelaxed(mirror.readbackRequestId) > LoadRelaxed(mirror.readbackCompletedId)) {
    StoreRelaxed(mirror.frameRequested, 1);
  }
}

void MarkReadbackCaptureStarted(int requestId) {
  MAIN_THREAD_ASYNC_EM_ASM(
      {
        window['__donnerWgpuReadbackCaptureStarts'] =
            Number(window['__donnerWgpuReadbackCaptureStarts'] || 0) + 1;
        window['__donnerWgpuReadbackLastStartedRequest'] = $0;
      },
      requestId);
}

void PublishReadbackFailure(int requestId) {
  if (requestId <= 0) {
    return;
  }
  BrowserMirror& mirror = Mirror();
  if (requestId > LoadRelaxed(mirror.readbackCompletedId)) {
    StoreRelaxed(mirror.readbackCompletedId, requestId);
  }
  MAIN_THREAD_ASYNC_EM_ASM(
      {
        window['__donnerWgpuReadbackCompleted'] =
            Math.max(Number(window['__donnerWgpuReadbackCompleted'] || 0), $0);
        window['__donnerWgpuReadbackCaptureFailures'] =
            Number(window['__donnerWgpuReadbackCaptureFailures'] || 0) + 1;
        window['__donnerWgpuReadbackLastFailedRequest'] = $0;
      },
      requestId);
}

void PublishReadbackStats(int renderSamples, int renderColored, int renderNonBlack,
                          int renderMaxChannel, int layerSamples, int layerColored,
                          int layerNonBlack, int layerMaxChannel, int selectionChromePixels,
                          int requestId) {
  BrowserMirror& mirror = Mirror();
  if (requestId > 0 && requestId > LoadRelaxed(mirror.readbackCompletedId)) {
    StoreRelaxed(mirror.readbackCompletedId, requestId);
  }
  MAIN_THREAD_ASYNC_EM_ASM(
      {
        if ($9 > 0) {
          window['__donnerWgpuReadbackCompleted'] =
              Math.max(Number(window['__donnerWgpuReadbackCompleted'] || 0), $9);
          window['__donnerWgpuReadbackCaptureCompletions'] =
              Number(window['__donnerWgpuReadbackCaptureCompletions'] || 0) + 1;
        }
        const previous = window['__donnerWgpuReadbackStats'];
        window['__donnerWgpuReadbackStats'] = ({
          'frame' : previous ? previous['frame'] + 1 : 1,
          'request' : $9 > 0 ? $9 : (previous ? previous['request'] || 0 : 0),
          'renderPane' : ({
            'samples' : $0,
            'coloredPixels' : $1,
            'nonBlackPixels' : $2,
            'maxChannel' : $3,
          }),
          'layerPreview' : ({
            'samples' : $4,
            'coloredPixels' : $5,
            'nonBlackPixels' : $6,
            'maxChannel' : $7,
          }),
          'selectionChromePixels' : $8,
        });
      },
      renderSamples, renderColored, renderNonBlack, renderMaxChannel, layerSamples, layerColored,
      layerNonBlack, layerMaxChannel, selectionChromePixels, requestId);
}

void PublishCarouselThumbnailStats(const int* values, int count) {
  // The async proxy reads the buffer after this function returns; copy into a
  // static so the caller's stack buffer is never read post-return. Capture is
  // serial (one diagnostic per explicit request), so one slot suffices.
  constexpr int kStride = 7;
  constexpr int kMaxThumbnails = 64;
  static int buffer[kMaxThumbnails * kStride];
  const int clamped = count > kMaxThumbnails ? kMaxThumbnails : count;
  std::copy(values, values + static_cast<size_t>(clamped) * kStride, buffer);
  MAIN_THREAD_ASYNC_EM_ASM(
      {
        const stride = 7;
        const base = $0 >> 2;
        const thumbnails = ([]);
        for (let index = 0; index < $1; ++index) {
          const offset = base + index * stride;
          thumbnails.push(({
            'samples' : HEAP32[offset + 0],
            'coloredPixels' : HEAP32[offset + 1],
            'nonBlackPixels' : HEAP32[offset + 2],
            'maxChannel' : HEAP32[offset + 3],
            'fingerprint' : HEAP32[offset + 4] >>> 0,
            'backgroundPixels' : HEAP32[offset + 5],
            'glyphPixels' : HEAP32[offset + 6],
          }));
        }
        const stats = window['__donnerWgpuReadbackStats'];
        if (stats) {
          stats['carouselThumbnails'] = thumbnails;
        }
      },
      buffer, clamped);
}

int CssWidth() {
  return LoadRelaxed(Mirror().cssWidth);
}

int CssHeight() {
  return LoadRelaxed(Mirror().cssHeight);
}

double DevicePixelRatio() {
  const double ratio = Mirror().devicePixelRatio;
  return ratio > 0.0 ? ratio : 1.0;
}

namespace {

/// Last size this thread pushed to the transferred canvas. Resizing an
/// `OffscreenCanvas` clears its backing store and forces a WebGPU surface
/// reconfigure, so only push on an actual change.
int g_appliedBackingWidth = 0;
int g_appliedBackingHeight = 0;

void SyncCanvasBackingSize() {
  const double ratio = DevicePixelRatio();
  const int width = std::max(1, static_cast<int>(CssWidth() * ratio));
  const int height = std::max(1, static_cast<int>(CssHeight() * ratio));
  if (width == g_appliedBackingWidth && height == g_appliedBackingHeight) {
    return;
  }
  g_appliedBackingWidth = width;
  g_appliedBackingHeight = height;
  emscripten_set_canvas_element_size("#canvas", width, height);
}

}  // namespace

int CanvasBackingWidth() {
  SyncCanvasBackingSize();
  return g_appliedBackingWidth;
}

int CanvasBackingHeight() {
  SyncCanvasBackingSize();
  return g_appliedBackingHeight;
}

bool ConsumeFrameRequest() {
  return std::atomic_ref<std::int32_t>(Mirror().frameRequested).exchange(0) != 0;
}

bool ZoomModifierHeld() {
  return LoadRelaxed(Mirror().zoomModifierHeld) != 0;
}

namespace {

/// Ring of recent inter-tick intervals. Bounded so a long session cannot grow
/// unboundedly; percentiles are computed over the window, which is what a
/// storm-length measurement wants anyway.
constexpr std::size_t kTickSampleCapacity = 2048;

struct FrameDriverState {
  void (*frameFn)(void*) = nullptr;
  void* userData = nullptr;
  pthread_t appThread{};
  em_proxying_queue* proxyQueue = nullptr;
  FrameDriver driver = FrameDriver::SetTimeoutFallback;

  double lastTickMs = 0.0;
  std::uint64_t ticks = 0;
  std::array<double, kTickSampleCapacity> intervalsMs{};
  std::size_t intervalCount = 0;
  std::size_t intervalCursor = 0;
  double maxIntervalMs = 0.0;
};

FrameDriverState& Driver() {
  static FrameDriverState state;
  return state;
}

/// Fold one tick into the interval window. Called on the app thread only.
void RecordTick() {
  FrameDriverState& state = Driver();
  const double nowMs = emscripten_get_now();
  if (state.lastTickMs > 0.0) {
    const double intervalMs = nowMs - state.lastTickMs;
    state.intervalsMs[state.intervalCursor] = intervalMs;
    state.intervalCursor = (state.intervalCursor + 1) % kTickSampleCapacity;
    state.intervalCount = std::min(state.intervalCount + 1, kTickSampleCapacity);
    state.maxIntervalMs = std::max(state.maxIntervalMs, intervalMs);
  }
  state.lastTickMs = nowMs;
  ++state.ticks;
}

/// Frame entry point for the worker-rAF arm: Emscripten's main loop calls this
/// once per animation frame on the app thread.
void RunDrivenFrame(void* userData) {
  RecordTick();
  FrameDriverState& state = Driver();
  if (state.frameFn != nullptr) {
    state.frameFn(userData);
  }
}

/// Frame entry point for the proxied arm, run on the app thread out of the
/// proxying queue after the browser main thread's rAF posted it.
void RunProxiedFrame(void* /*unused*/) {
  FrameDriverState& state = Driver();
  RunDrivenFrame(state.userData);
}

}  // namespace

// Called from the browser main thread's rAF loop. Runs on the main thread (the
// wasm module is shared), and its only job is to hand one task per vsync to the
// app thread's proxying queue, which wakes that thread's event loop.
extern "C" EMSCRIPTEN_KEEPALIVE void donner_whole_app_vsync_tick() {
  FrameDriverState& state = Driver();
  if (state.proxyQueue == nullptr) {
    return;
  }
  emscripten_proxy_async(state.proxyQueue, state.appThread, &RunProxiedFrame, nullptr);
}

EM_JS(bool, WorkerRequestAnimationFrameAvailableImpl, (),
      { return typeof globalThis.requestAnimationFrame == 'function'; });

bool WorkerRequestAnimationFrameAvailable() {
  return WorkerRequestAnimationFrameAvailableImpl();
}

FrameDriver InstallFrameDriver(void (*frameFn)(void*), void* userData) {
  FrameDriverState& state = Driver();
  state.frameFn = frameFn;
  state.userData = userData;
  state.appThread = pthread_self();

  if (WorkerRequestAnimationFrameAvailable()) {
    // Emscripten's `fps == 0` path selects EM_TIMING_RAF, whose scheduler calls
    // `globalThis.requestAnimationFrame` when it exists. Nothing else to do.
    state.driver = FrameDriver::WorkerRequestAnimationFrame;
    MAIN_THREAD_ASYNC_EM_ASM({ window['__donnerFrameDriver'] = 'worker-raf'; });
    emscripten_set_main_loop_arg(&RunDrivenFrame, userData, /*fps=*/0,
                                 /*simulateInfiniteLoop=*/true);
    return state.driver;
  }

  // No worker rAF on this engine: Emscripten would fall back to a setTimeout
  // emulation that is not vsync-aligned and is subject to the nested-timer
  // clamp. Drive from the browser main thread's rAF instead and pay one
  // postMessage per frame (measured at ~40 microseconds).
  state.proxyQueue = em_proxying_queue_create();
  if (state.proxyQueue == nullptr) {
    state.driver = FrameDriver::SetTimeoutFallback;
    MAIN_THREAD_ASYNC_EM_ASM({ window['__donnerFrameDriver'] = 'set-timeout'; });
    emscripten_set_main_loop_arg(&RunDrivenFrame, userData, /*fps=*/0,
                                 /*simulateInfiniteLoop=*/true);
    return state.driver;
  }

  state.driver = FrameDriver::ProxiedMainThreadRequestAnimationFrame;
  MAIN_THREAD_ASYNC_EM_ASM({
    window['__donnerFrameDriver'] = 'proxied-main-raf';
    const tick = function() {
      _donner_whole_app_vsync_tick();
      requestAnimationFrame(tick);
    };
    requestAnimationFrame(tick);
  });
  // The app thread must keep returning to its event loop so the proxying queue
  // drains; `emscripten_exit_with_live_runtime` does exactly that without
  // installing a second scheduler that would double-drive the frame.
  emscripten_exit_with_live_runtime();
  return state.driver;
}

FrameTickStats TickStats() {
  const FrameDriverState& state = Driver();
  FrameTickStats stats;
  stats.ticks = state.ticks;
  stats.maxMs = state.maxIntervalMs;
  if (state.intervalCount == 0) {
    return stats;
  }
  std::vector<double> sorted(
      state.intervalsMs.begin(),
      state.intervalsMs.begin() + static_cast<std::ptrdiff_t>(state.intervalCount));
  std::sort(sorted.begin(), sorted.end());
  const auto percentile = [&sorted](double fraction) {
    const std::size_t index = std::min(
        sorted.size() - 1, static_cast<std::size_t>(fraction * static_cast<double>(sorted.size())));
    return sorted[index];
  };
  stats.p50Ms = percentile(0.50);
  stats.p99Ms = percentile(0.99);
  return stats;
}

namespace {

/// Publish this frame's ASYNCIFY suspend attribution and the frame driver's
/// tick jitter. Both are per-frame tail statistics, so the page keeps sample
/// arrays rather than running averages.
void PublishSuspendAndTickStats() {
  const FrameSuspendTotals suspend = EndSuspendFrame();
  const FrameTickStats ticks = TickStats();
  MAIN_THREAD_ASYNC_EM_ASM(
      {
        let stats = window['__donnerAsyncifySuspendStats'];
        if (!stats) {
          stats = window['__donnerAsyncifySuspendStats'] = ({
            'frames' : 0,
            'suspends' : 0,
            'totalMs' : 0,
            'frameMsSamples' : [],
            'frameCountSamples' : [],
            'longestMs' : 0,
            'tileYieldMs' : 0,
            'gpuReadbackMs' : 0,
            'deviceWaitMs' : 0,
            'startupMs' : 0,
          });
        }
        stats['frames'] = (stats['frames'] | 0) + 1;
        stats['suspends'] = (stats['suspends'] | 0) + $0;
        stats['totalMs'] += $1;
        stats['longestMs'] = Math.max(stats['longestMs'], $2);
        stats['tileYieldMs'] += $3;
        stats['gpuReadbackMs'] += $4;
        stats['deviceWaitMs'] += $5;
        stats['startupMs'] += $6;
        const kMaxSamples = 4096;
        if (stats['frameMsSamples'].length < kMaxSamples) {
          stats['frameMsSamples'].push($1);
          stats['frameCountSamples'].push($0);
        }

        window['__donnerFrameTickStats'] = ({
          'driver' : window['__donnerFrameDriver'] || 'unknown',
          'ticks' : $7,
          'p50Ms' : $8,
          'p99Ms' : $9,
          'maxMs' : $10,
        });
      },
      static_cast<int>(suspend.count), suspend.totalMs, suspend.longestMs,
      suspend.msByKind[static_cast<std::size_t>(SuspendKind::TileYield)],
      suspend.msByKind[static_cast<std::size_t>(SuspendKind::GpuReadback)],
      suspend.msByKind[static_cast<std::size_t>(SuspendKind::DeviceWait)],
      suspend.msByKind[static_cast<std::size_t>(SuspendKind::Startup)],
      static_cast<double>(ticks.ticks), ticks.p50Ms, ticks.p99Ms, ticks.maxMs);
  // Open the next frame's attribution window immediately: the interval between
  // this publish and the next frame body is the browser's, not ours, but any
  // suspend that happens in it is still frame cost the next sample should own.
  BeginSuspendFrame();
}

/// Doubles per category in the published memory buffer: retained, retained high
/// water, transient this frame, transient high water, entry count.
constexpr std::size_t kMemoryFieldsPerCategory = 5;
/// Doubles per stage: net this frame, cumulative net, max net, entry count.
constexpr std::size_t kMemoryFieldsPerStage = 4;
/// Doubles per allocation-size bucket: live bytes, live blocks, peak bytes.
constexpr std::size_t kMemoryFieldsPerBucket = 3;
/// Doubles per live large block: usable bytes, then the `AllocTag` that
/// allocated it.
constexpr std::size_t kMemoryFieldsPerLiveLarge = 2;
/// Doubles per allocation tag: live bytes, live blocks, peak bytes, total
/// allocations.
constexpr std::size_t kMemoryFieldsPerAllocTag = 4;

/// Offsets of the layout descriptor, which is the first thing in the buffer.
///
/// The block bases and strides used to be `EM_ASM` arguments. That ran out:
/// `EM_ASM` substitutes `$0` upward and stops before `$16`, so adding the
/// allocation-tag block silently published `$16 is not defined` on every frame
/// instead of a sample. They travel in the buffer now, which has no such limit
/// and keeps one description of the layout for both sides to read.
enum MemoryLayoutField : std::size_t {
  kLayoutHeaderBase = 0,
  kLayoutCategoryBase = 1,
  kLayoutFieldsPerCategory = 2,
  kLayoutCategoryCount = 3,
  kLayoutStageBase = 4,
  kLayoutFieldsPerStage = 5,
  kLayoutStageCount = 6,
  kLayoutBucketBase = 7,
  kLayoutFieldsPerBucket = 8,
  kLayoutBucketCount = 9,
  kLayoutRecentLargeBase = 10,
  kLayoutRecentLargeCount = 11,
  kLayoutLiveLargeBase = 12,
  kLayoutFieldsPerLiveLarge = 13,
  kLayoutLiveLargeCount = 14,
  kLayoutAllocTagBase = 15,
  kLayoutFieldsPerAllocTag = 16,
  kLayoutAllocTagCount = 17,
  kMemoryLayoutFields = 18,
};

/// Doubles in the process-wide block: the six platform totals, the retained sum
/// and its high water, and the large-block table overflow count.
constexpr std::size_t kMemoryHeaderFields = 9;
constexpr std::size_t kMemoryHeaderBase = kMemoryLayoutFields;
constexpr std::size_t kMemoryCategoryBase = kMemoryHeaderBase + kMemoryHeaderFields;
constexpr std::size_t kMemoryStageBase =
    kMemoryCategoryBase + kMemoryCategoryCount * kMemoryFieldsPerCategory;
constexpr std::size_t kMemoryBucketBase =
    kMemoryStageBase + kMemoryStageCount * kMemoryFieldsPerStage;
constexpr std::size_t kMemoryLargeBase =
    kMemoryBucketBase + kHeapSizeBucketCount * kMemoryFieldsPerBucket;
constexpr std::size_t kMemoryLiveLargeBase = kMemoryLargeBase + kRecentLargeAllocationCount;
constexpr std::size_t kMemoryAllocTagBase =
    kMemoryLiveLargeBase + kLiveLargeBlockCount * kMemoryFieldsPerLiveLarge;
constexpr std::size_t kMemoryBufferDoubles =
    kMemoryAllocTagBase + kAllocTagCount * kMemoryFieldsPerAllocTag;

/// `MAIN_THREAD_ASYNC_EM_ASM` posts a task and returns, so the buffer it points
/// at has to outlive the post. A ring means a frame's sample is only clobbered
/// once the app thread has published this many more frames, which on a stalled
/// main thread costs a stale sample rather than a torn one.
constexpr std::size_t kMemoryBufferSlots = 8;

/// Publish the frame's byte attribution onto `window.__donnerMemoryStats`.
///
/// The buffer is a flat `double` array rather than an argument list because
/// there are five numbers per category and `EM_ASM` argument slots do not
/// stretch that far.
///
/// The category names are a JS literal rather than strings read out of linear
/// memory: an `EM_ASM` body is minified by closure along with the rest of the
/// glue, and the string helpers (`UTF8ToString` and friends) are module-local
/// bindings that closure is free to rename, so reaching for one from inside a
/// body throws `TypeError: a is not a function` and kills the app thread. The
/// literal is kept honest two ways: `MemoryCategoryNamesMatch` (below) asserts
/// it against the C++ enum in a native test, and the body publishes a mismatch
/// flag if the two lengths ever disagree at runtime.
void PublishMemoryAttribution() {
  static double buffers[kMemoryBufferSlots][kMemoryBufferDoubles];
  static std::size_t nextSlot = 0;

  const MemoryAttributionSample sample = SampleMemoryAttribution();
  double* buffer = buffers[nextSlot];
  nextSlot = (nextSlot + 1) % kMemoryBufferSlots;

  buffer[kLayoutHeaderBase] = static_cast<double>(kMemoryHeaderBase);
  buffer[kLayoutCategoryBase] = static_cast<double>(kMemoryCategoryBase);
  buffer[kLayoutFieldsPerCategory] = static_cast<double>(kMemoryFieldsPerCategory);
  buffer[kLayoutCategoryCount] = static_cast<double>(kMemoryCategoryCount);
  buffer[kLayoutStageBase] = static_cast<double>(kMemoryStageBase);
  buffer[kLayoutFieldsPerStage] = static_cast<double>(kMemoryFieldsPerStage);
  buffer[kLayoutStageCount] = static_cast<double>(kMemoryStageCount);
  buffer[kLayoutBucketBase] = static_cast<double>(kMemoryBucketBase);
  buffer[kLayoutFieldsPerBucket] = static_cast<double>(kMemoryFieldsPerBucket);
  buffer[kLayoutBucketCount] = static_cast<double>(kHeapSizeBucketCount);
  buffer[kLayoutRecentLargeBase] = static_cast<double>(kMemoryLargeBase);
  buffer[kLayoutRecentLargeCount] = static_cast<double>(kRecentLargeAllocationCount);
  buffer[kLayoutLiveLargeBase] = static_cast<double>(kMemoryLiveLargeBase);
  buffer[kLayoutFieldsPerLiveLarge] = static_cast<double>(kMemoryFieldsPerLiveLarge);
  buffer[kLayoutLiveLargeCount] = static_cast<double>(kLiveLargeBlockCount);
  buffer[kLayoutAllocTagBase] = static_cast<double>(kMemoryAllocTagBase);
  buffer[kLayoutFieldsPerAllocTag] = static_cast<double>(kMemoryFieldsPerAllocTag);
  buffer[kLayoutAllocTagCount] = static_cast<double>(kAllocTagCount);

  double* header = buffer + kMemoryHeaderBase;
  header[0] = static_cast<double>(sample.wasmHeapBytes);
  header[1] = static_cast<double>(sample.wasmHeapHighWaterBytes);
  header[2] = static_cast<double>(sample.mallocLiveBytes);
  header[3] = static_cast<double>(sample.mallocLiveHighWaterBytes);
  header[4] = static_cast<double>(sample.mallocFreeBytes);
  header[5] = static_cast<double>(sample.mallocArenaBytes);
  header[6] = static_cast<double>(sample.totalRetainedBytes);
  header[7] = static_cast<double>(sample.totalRetainedHighWaterBytes);
  // Filled after the histogram is read, below.
  header[8] = 0.0;
  for (std::size_t index = 0; index < kMemoryCategoryCount; ++index) {
    double* slot = buffer + kMemoryCategoryBase + index * kMemoryFieldsPerCategory;
    slot[0] = static_cast<double>(sample.retainedBytes[index]);
    slot[1] = static_cast<double>(sample.retainedHighWaterBytes[index]);
    slot[2] = static_cast<double>(sample.transientBytes[index]);
    slot[3] = static_cast<double>(sample.transientHighWaterBytes[index]);
    slot[4] = static_cast<double>(sample.entryCounts[index]);
  }

  const MemoryStageSample stages = SampleMemoryStages();
  for (std::size_t index = 0; index < kMemoryStageCount; ++index) {
    double* slot = buffer + kMemoryStageBase + index * kMemoryFieldsPerStage;
    slot[0] = static_cast<double>(stages.netBytes[index]);
    slot[1] = static_cast<double>(stages.cumulativeNetBytes[index]);
    slot[2] = static_cast<double>(stages.maxNetBytes[index]);
    slot[3] = static_cast<double>(stages.entries[index]);
  }

  const HeapSizeHistogram histogram = SampleHeapSizeHistogram();
  header[8] = static_cast<double>(histogram.tableOverflows);
  for (std::size_t index = 0; index < kHeapSizeBucketCount; ++index) {
    double* slot = buffer + kMemoryBucketBase + index * kMemoryFieldsPerBucket;
    slot[0] = static_cast<double>(histogram.liveBytes[index]);
    slot[1] = static_cast<double>(histogram.liveBlocks[index]);
    slot[2] = static_cast<double>(histogram.peakLiveBytes[index]);
  }

  for (std::size_t index = 0; index < kRecentLargeAllocationCount; ++index) {
    buffer[kMemoryLargeBase + index] = static_cast<double>(histogram.recentLargeBytes[index]);
  }
  for (std::size_t index = 0; index < kLiveLargeBlockCount; ++index) {
    double* slot = buffer + kMemoryLiveLargeBase + index * kMemoryFieldsPerLiveLarge;
    slot[0] = static_cast<double>(histogram.liveLargeBytes[index]);
    slot[1] = static_cast<double>(histogram.liveLargeTags[index]);
  }

  const AllocTagTotals tagTotals = SampleAllocTagTotals();
  for (std::size_t index = 0; index < kAllocTagCount; ++index) {
    double* slot = buffer + kMemoryAllocTagBase + index * kMemoryFieldsPerAllocTag;
    slot[0] = static_cast<double>(tagTotals.liveBytes[index]);
    slot[1] = static_cast<double>(tagTotals.liveBlocks[index]);
    slot[2] = static_cast<double>(tagTotals.peakLiveBytes[index]);
    slot[3] = static_cast<double>(tagTotals.totalAllocations[index]);
  }

  // clang-format off
  MAIN_THREAD_ASYNC_EM_ASM(
      {
        const buffer = $0 >> 3;
        const heap = HEAPF64;
        // The buffer starts with its own layout descriptor; see
        // `MemoryLayoutField`. Reading the bases from the data instead of from
        // `EM_ASM` arguments keeps the block count unbounded and keeps one
        // description of the layout rather than two that can disagree.
        const headerBase = buffer + heap[buffer + 0];
        const categoryBase = buffer + heap[buffer + 1];
        const fieldsPerCategory = heap[buffer + 2];
        const categoryCount = heap[buffer + 3];
        const stageBase = buffer + heap[buffer + 4];
        const fieldsPerStage = heap[buffer + 5];
        const stageCount = heap[buffer + 6];
        const bucketBase = buffer + heap[buffer + 7];
        const fieldsPerBucket = heap[buffer + 8];
        const bucketCount = heap[buffer + 9];
        const recentLargeBase = buffer + heap[buffer + 10];
        const recentLargeCount = heap[buffer + 11];
        const liveLargeBase = buffer + heap[buffer + 12];
        const fieldsPerLiveLarge = heap[buffer + 13];
        const liveLargeCount = heap[buffer + 14];
        const allocTagBase = buffer + heap[buffer + 15];
        const fieldsPerAllocTag = heap[buffer + 16];
        const allocTagCount = heap[buffer + 17];
        // Mirror of `donner::MemoryCategory`, in enum order. Parenthesized
        // because the preprocessor splits `EM_ASM` macro arguments on top-level
        // commas and brackets do not protect them.
        const names = ([
          'compositorSegmentBitmaps', 'compositorSegmentTextures', 'compositorLayerBitmaps',
          'compositorLayerTextures', 'renderResultTiles', 'workerFrameSnapshot',
          'presentationTiles', 'presentationOverviewTiles', 'presentationRetired', 'layerThumbnails'
        ]);
        window['__donnerMemoryCategoryMismatch'] = names.length !== categoryCount;
        const categories = {};
        for (let index = 0; index < names.length; ++index) {
          const slot = categoryBase + index * fieldsPerCategory;
          categories[names[index]] = ({
            'retainedBytes' : heap[slot],
            'retainedHighWaterBytes' : heap[slot + 1],
            'transientBytes' : heap[slot + 2],
            'transientHighWaterBytes' : heap[slot + 3],
            'entries' : heap[slot + 4],
          });
        }
        // Mirror of `donner::MemoryStage`, in enum order.
        const stageNames = ([
          'workerRenderFrame', 'workerBuildPreview', 'workerFinalSnapshot', 'workerOther',
          'appPollResult', 'appUiFrame', 'appHostFrame', 'appInput'
        ]);
        window['__donnerMemoryStageMismatch'] = stageNames.length !== stageCount;
        const stages = {};
        for (let index = 0; index < stageNames.length; ++index) {
          const slot = stageBase + index * fieldsPerStage;
          stages[stageNames[index]] = ({
            'netBytes' : heap[slot],
            'cumulativeNetBytes' : heap[slot + 1],
            'maxNetBytes' : heap[slot + 2],
            'entries' : heap[slot + 3],
          });
        }
        // Live heap by block size; bucket k covers [2^k, 2^(k+1)) bytes. Only
        // non-empty buckets are published so the object stays readable.
        const sizeBuckets = {};
        for (let index = 0; index < bucketCount; ++index) {
          const slot = bucketBase + index * fieldsPerBucket;
          if (heap[slot + 2] === 0) {
            continue;
          }
          sizeBuckets[index] = ({
            'liveBytes' : heap[slot],
            'liveBlocks' : heap[slot + 1],
            'peakLiveBytes' : heap[slot + 2],
          });
        }
        const recentLarge = [];
        for (let index = 0; index < recentLargeCount; ++index) {
          const value = heap[recentLargeBase + index];
          if (value > 0) {
            recentLarge.push(value);
          }
        }
        // Mirror of `donner::AllocTag`, in enum order. Indexed by the tag value
        // stored beside each live large block.
        const tagNames = ([
          'untagged', 'workerRenderFrame', 'workerBuildPreview', 'workerFinalSnapshot',
          'workerOther', 'appPollResult', 'appUiFrame', 'appHostFrame', 'appInput',
          'renderTileRaster', 'gpuReadbackStaging', 'compositorBitmap', 'presentationUpload',
          'imguiDrawLists'
        ]);
        window['__donnerAllocTagMismatch'] = tagNames.length !== allocTagCount;
        const liveLarge = [];
        for (let index = 0; index < liveLargeCount; ++index) {
          const slot = liveLargeBase + index * fieldsPerLiveLarge;
          const value = heap[slot];
          if (value > 0) {
            liveLarge.push(([ value, tagNames[heap[slot + 1]] || 'unknown' ]));
          }
        }
        const allocTags = {};
        for (let index = 0; index < allocTagCount; ++index) {
          const slot = allocTagBase + index * fieldsPerAllocTag;
          if (heap[slot + 3] === 0) {
            continue;
          }
          allocTags[tagNames[index]] = ({
            'liveBytes' : heap[slot],
            'liveBlocks' : heap[slot + 1],
            'peakLiveBytes' : heap[slot + 2],
            'totalAllocations' : heap[slot + 3],
          });
        }
        const stats = ({
          'frames' : ((window['__donnerMemoryStats'] && window['__donnerMemoryStats']['frames']) ||
                      0) +
              1,
          'wasmHeapBytes' : heap[headerBase],
          'wasmHeapHighWaterBytes' : heap[headerBase + 1],
          'mallocLiveBytes' : heap[headerBase + 2],
          'mallocLiveHighWaterBytes' : heap[headerBase + 3],
          'mallocFreeBytes' : heap[headerBase + 4],
          'mallocArenaBytes' : heap[headerBase + 5],
          'totalRetainedBytes' : heap[headerBase + 6],
          'totalRetainedHighWaterBytes' : heap[headerBase + 7],
          'largeBlockTableOverflows' : heap[headerBase + 8],
          'categories' : categories,
          'stages' : stages,
          'sizeBuckets' : sizeBuckets,
          'recentLargeBytes' : recentLarge,
          'liveLargeBytes' : liveLarge,
          'allocTags' : allocTags,
        });
        window['__donnerMemoryStats'] = stats;
        const series = window['__donnerMemorySeries'] || (window['__donnerMemorySeries'] = []);
        const kMaxSamples = 4096;
        if (series.length < kMaxSamples) {
          series.push(([ heap[headerBase], heap[headerBase + 2], heap[headerBase + 6] ]));
        }
      },
      buffer);
  // clang-format on
}

}  // namespace

void RecordFrameSample(int triggerBits, double frameMs, int callbacks) {
  MAIN_THREAD_ASYNC_EM_ASM(
      {
        const stats = window['__donnerFrameLoopStats'];
        if (!stats) {
          return;
        }
        stats['callbacks'] = (stats['callbacks'] | 0) + $2;
        stats['renderedFrames'] = (stats['renderedFrames'] | 0) + 1;
        if ($0 & 1) {
          stats['workerTriggeredFrames'] = (stats['workerTriggeredFrames'] | 0) + 1;
        }
        if ($0 & 2) {
          stats['inputTriggeredFrames'] = (stats['inputTriggeredFrames'] | 0) + 1;
        }
        if ($0 & 4) {
          stats['timerTriggeredFrames'] = (stats['timerTriggeredFrames'] | 0) + 1;
        }
        if ($0 == 1) {
          stats['workerOnlyFrames'] = (stats['workerOnlyFrames'] | 0) + 1;
        }
        const kMaxSamples = 4096;
        if (stats['uiFrameMsSamples'].length < kMaxSamples) {
          stats['uiFrameMsSamples'].push($1);
        }
        // Page-clock arrival time of the latest frame's sample. The presenting
        // frame publishes its worker stats and this sample through the same
        // in-order proxy queue, so a probe can measure the result-to-frame
        // handoff from product timestamps instead of from when its own poll
        // happened to run.
        stats['lastFrameAtMs'] = performance.now();
        // Each worker result publishes a fresh stats object with no
        // 'presentedAtMs'; the first frame sample to arrive after it is the
        // end of the frame that consumed that result, so stamp it exactly
        // once. Probes read the pair to measure how promptly a completed
        // result reached the canvas without racing their own poll cadence.
        const workerStats = window['__donnerWorkerStats'];
        if (workerStats && workerStats['presentedAtMs'] === undefined) {
          workerStats['presentedAtMs'] = performance.now();
        }
        window['__donnerMainLoopRenderedFrames'] =
            Number(window['__donnerMainLoopRenderedFrames'] || 0) + 1;
        window['__donnerHeapBytes'] = $3;
        window['__donnerHeapBytesHighWater'] =
            Math.max(Number(window['__donnerHeapBytesHighWater'] || 0), $3);
      },
      triggerBits, frameMs, callbacks, static_cast<double>(emscripten_get_heap_size()));

  PublishSuspendAndTickStats();
  PublishMemoryAttribution();
}

void PublishImGuiDrawStats(int vertexCount, int indexCount, int commandListCount) {
  MAIN_THREAD_ASYNC_EM_ASM(
      {
        const previous = window['__donnerImGuiDrawStats'];
        window['__donnerImGuiDrawStats'] = ({
          'vertices' : $0,
          'indices' : $1,
          'commandLists' : $2,
          // The retained staging arrays are sized by the worst frame, not the
          // current one, so the peak is the number that explains the heap.
          'peakVertices' : Math.max(previous ? previous['peakVertices'] : 0, $0),
          'peakIndices' : Math.max(previous ? previous['peakIndices'] : 0, $1),
          'frames' : (previous ? previous['frames'] : 0) + 1,
        });
      },
      vertexCount, indexCount, commandListCount);
}

void PublishHostFrameTiming(double endFrameMs, double imguiRenderMs, double surfaceAcquireMs,
                            double underlayMs, double imguiDrawMs, double directMs,
                            double readbackMs, double presentMs) {
  MAIN_THREAD_ASYNC_EM_ASM(
      {
        const stats = window['__donnerHostFrameTiming'] ||
                      (window['__donnerHostFrameTiming'] = ({'frames' : 0, 'sums' : {}}));
        const names = ([
          'endFrameMs', 'imguiRenderMs', 'surfaceAcquireMs', 'underlayMs', 'imguiDrawMs',
          'directMs', 'readbackMs', 'presentMs'
        ]);
        const values = ([ $0, $1, $2, $3, $4, $5, $6, $7 ]);
        stats['frames'] = (stats['frames'] | 0) + 1;
        for (let index = 0; index < names.length; ++index) {
          stats[names[index]] = values[index];
          stats['sums'][names[index]] = (stats['sums'][names[index]] || 0) + values[index];
        }
      },
      endFrameMs, imguiRenderMs, surfaceAcquireMs, underlayMs, imguiDrawMs, directMs, readbackMs,
      presentMs);
}

void PublishPinchZoomPolicy(double wheelDeltaPerLnScale) {
  MAIN_THREAD_ASYNC_EM_ASM(
      { window['__donnerPinchWheelDeltaPerLnScale'] = $0; }, wheelDeltaPerLnScale);
}

void NotifyFirstFramePresented(int headlessDeviceCreations) {
  MAIN_THREAD_ASYNC_EM_ASM(
      {
        window['__donnerHeadlessDeviceCreations'] = $0;
        if (window['__donnerFirstFramePresented']) {
          return;
        }
        window['__donnerNotifyFirstFramePresented']();
      },
      headlessDeviceCreations);
}

void RecordScrollDebug(bool zoomModifierHeld, double xoffset, double yoffset,
                       bool physicalKeyHeld) {
  MAIN_THREAD_ASYNC_EM_ASM(
      {
        const previous = window['__donnerLastScrollEvent'];
        window['__donnerLastScrollEvent'] = ({
          'zoomModifierHeld' : Boolean($0),
          'phys' : $3,
          'dom' : $0,
          'xoffset' : $1,
          'yoffset' : $2,
          'count' : ((previous && previous['count']) || 0) + 1,
        });
      },
      zoomModifierHeld ? 1 : 0, xoffset, yoffset, physicalKeyHeld ? 1 : 0);
}

}  // namespace donner::editor::whole_app_worker

#endif  // DONNER_EDITOR_WHOLE_APP_WORKER
