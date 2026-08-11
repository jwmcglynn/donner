// Design 0064 phase 1 experiment bootstrap.
//
// Differences from the shipping `editor-bootstrap.js`:
//
//   - There is exactly one canvas. Emscripten transfers `#canvas` to the app
//     pthread as an OffscreenCanvas at startup (PROXY_TO_PTHREAD plus
//     OFFSCREENCANVAS_SUPPORT), so the page never draws into it and the dual
//     document-canvas bridge, the CSS placement path, and the bitmap
//     presentation queue are all absent.
//   - `main()` runs on a worker. Everything below runs on the browser main
//     thread and must stay side-effect free with respect to the canvas backing
//     store, because touching `canvas.width` after the transfer throws.
//   - The `__donner*` probe surface stays on `window` so the browser suites and
//     the perf lane keep reading the page. The app thread posts into it through
//     `MAIN_THREAD_ASYNC_EM_ASM` / shared-memory mirrors (see
//     `donner/editor/WholeAppWorkerBridge.h`).

const canvas = document.getElementById("canvas");
const loadingScreen = document.getElementById("loading-screen");
const status = document.getElementById("status");
const loadingProgress = document.getElementById("loading-progress");
const loadingProgressFill = document.getElementById("loading-progress-fill");
const loadingDetail = document.getElementById("loading-detail");
const capabilityError = document.getElementById("capability-error");
const capabilityErrorDetail = document.getElementById("capability-error-detail");
let editorRevealed = false;

window.__donnerBootstrapStartedAtMs = performance.now();
window.__donnerBackend = "geode";
window.__donnerWholeAppWorker = true;
// The app thread has its own `performance` time origin. Publish the page's so a
// worker-side timestamp can be expressed on the page clock without a round trip.
window.__donnerPageTimeOriginMs = performance.timeOrigin;
window.__donnerRequiresWorkerRuntime = false;
window.__donnerWorkerRuntimeStats = {
  ready: true,
  initializationMs: 0,
  maskPipelineMs: 0,
  initializationCount: 0,
  workerDeviceCreations: 0,
  readyAtMs: 0,
};

function SetLoadingPhase(message, progress, detail) {
  status.textContent = message;
  if (detail) {
    loadingDetail.textContent = detail;
  }
  if (Number.isFinite(progress)) {
    const percent = Math.max(0, Math.min(100, progress));
    loadingProgress.classList.add("is-determinate");
    loadingProgress.setAttribute("aria-valuenow", String(Math.round(percent)));
    loadingProgressFill.style.width = `${percent}%`;
  } else {
    loadingProgress.classList.remove("is-determinate");
    loadingProgress.removeAttribute("aria-valuenow");
    loadingProgressFill.style.width = "";
  }
}

function ShowCapabilityError(message) {
  capabilityErrorDetail.textContent = message;
  capabilityError.hidden = false;
  loadingScreen.hidden = true;
  canvas.hidden = true;
  console.error(message);
}

window.__donnerReportWorkerSurfaceUnavailable = function() {
  ShowCapabilityError(
    "The Geode WebGPU renderer could not present its canvas surface. "
      + "Update or enable WebGPU in this browser to continue.",
  );
};

function RevealEditorAfterFirstFrame() {
  if (editorRevealed || !window.__donnerFirstFramePresented) {
    return;
  }
  editorRevealed = true;
  window.__donnerEditorRevealedAtMs = performance.now();
  loadingScreen.classList.add("is-complete");
  canvas.focus();
  setTimeout(() => {
    window.__donnerLoadingScreenHiddenAtMs = performance.now();
    loadingScreen.hidden = true;
  }, 220);
}

window.addEventListener(
  "donner:first-frame-presented",
  () => {
    window.__donnerFirstFramePresentedAtMs = performance.now();
    RevealEditorAfterFirstFrame();
  },
  { once: true },
);

// The app thread cannot dispatch a DOM event, so it flips this flag through the
// shared-memory bridge and the page polls it until the first frame lands.
window.__donnerNotifyFirstFramePresented = function() {
  window.__donnerFirstFramePresented = true;
  window.dispatchEvent(new Event("donner:first-frame-presented"));
};

// Event round-trip probe. The capture-phase listener runs before any Emscripten
// proxying, so the delta the app thread computes against
// `__donnerLastPointerDownEpochMs` is the full main-thread-to-worker hop.
window.__donnerInputLatencyMsSamples = [];
window.__donnerLastPointerDownEpochMs = 0;
window.addEventListener("pointerdown", () => {
  window.__donnerLastPointerDownEpochMs = performance.timeOrigin + performance.now();
}, { capture: true, passive: true });

function InstallTouchPointerBridge(targetCanvas) {
  if (!window.PointerEvent) {
    return;
  }

  let activeTouchId = null;
  const dispatchMouse = (type, event, buttons) => {
    targetCanvas.dispatchEvent(
      new MouseEvent(type, {
        bubbles: true,
        cancelable: true,
        view: window,
        clientX: event.clientX,
        clientY: event.clientY,
        button: 0,
        buttons,
      }),
    );
  };

  targetCanvas.style.touchAction = "none";
  targetCanvas.addEventListener("pointerdown", (event) => {
    if (event.pointerType !== "touch" || activeTouchId !== null) {
      return;
    }
    activeTouchId = event.pointerId;
    event.preventDefault();
    targetCanvas.focus();
    targetCanvas.setPointerCapture?.(event.pointerId);
    dispatchMouse("mousedown", event, 1);
  });
  targetCanvas.addEventListener("pointermove", (event) => {
    if (event.pointerType !== "touch" || event.pointerId !== activeTouchId) {
      return;
    }
    event.preventDefault();
    dispatchMouse("mousemove", event, 1);
  });
  const finishTouch = (event) => {
    if (event.pointerType !== "touch" || event.pointerId !== activeTouchId) {
      return;
    }
    event.preventDefault();
    dispatchMouse("mouseup", event, 0);
    targetCanvas.releasePointerCapture?.(event.pointerId);
    activeTouchId = null;
  };
  targetCanvas.addEventListener("pointerup", finishTouch);
  targetCanvas.addEventListener("pointercancel", finishTouch);
}

// Keep in sync with donner/editor/PinchZoomPolicy.h; see editor-bootstrap.js for
// the derivation of the constant.
const kPinchWheelDeltaPerLnScaleFallback = 1049.2059;

function PinchWheelDeltaPerLnScale() {
  const published = window.__donnerPinchWheelDeltaPerLnScale;
  return Number.isFinite(published) && published > 0
    ? published
    : kPinchWheelDeltaPerLnScaleFallback;
}

function InstallTrackpadGestureBridge(targetCanvas) {
  let previousScale = 1;
  const preventPageZoom = (event) => {
    event.preventDefault();
  };
  targetCanvas.addEventListener("gesturestart", (event) => {
    preventPageZoom(event);
    previousScale = Number.isFinite(event.scale) && event.scale > 0 ? event.scale : 1;
    targetCanvas.focus();
  }, { passive: false });
  targetCanvas.addEventListener("gesturechange", (event) => {
    preventPageZoom(event);
    const scale = Number.isFinite(event.scale) && event.scale > 0 ? event.scale : previousScale;
    const incrementalScale = scale / Math.max(previousScale, 0.001);
    previousScale = scale;
    targetCanvas.dispatchEvent(
      new WheelEvent("wheel", {
        bubbles: true,
        cancelable: true,
        clientX: event.clientX,
        clientY: event.clientY,
        ctrlKey: true,
        deltaMode: WheelEvent.DOM_DELTA_PIXEL,
        deltaY: -Math.log(Math.max(incrementalScale, 0.001)) * PinchWheelDeltaPerLnScale(),
      }),
    );
  }, { passive: false });
  targetCanvas.addEventListener("gestureend", (event) => {
    preventPageZoom(event);
    previousScale = 1;
  }, { passive: false });
}

InstallTouchPointerBridge(canvas);
InstallTrackpadGestureBridge(canvas);

window.__donnerCanStartWasm = typeof SharedArrayBuffer !== "undefined"
  && typeof OffscreenCanvas !== "undefined"
  && typeof canvas.transferControlToOffscreen === "function";
if (!window.__donnerCanStartWasm) {
  const reason = typeof SharedArrayBuffer === "undefined"
    ? (window.isSecureContext
      ? "This page is secure, but cross-origin isolation is not active."
      : "This page is not running in a secure context.")
    : "This browser cannot transfer a canvas to an OffscreenCanvas.";
  ShowCapabilityError(`${reason} The whole-app-worker build cannot start.`);
}

var Module = {
  preRun: [],
  postRun: [],
  print: function(text) {
    console.log(text);
  },
  printErr: function(text) {
    console.error(text);
  },
  setStatus: function(text) {
    if (!text) {
      return;
    }
    const progressMatch = text.match(/\((\d+(?:\.\d+)?)\s*\/\s*(\d+(?:\.\d+)?)\)/);
    const progress = progressMatch && Number(progressMatch[2]) > 0
      ? (Number(progressMatch[1]) / Number(progressMatch[2])) * 100
      : undefined;
    SetLoadingPhase(text.replace(/\s*\([^)]*\)\s*$/, ""), progress);
  },
  monitorRunDependencies: function(left) {
    if (left > 0) {
      Module.totalDependencies = Math.max(Module.totalDependencies || 0, left);
      const completed = Module.totalDependencies - left;
      const progress = 28 + (completed / Module.totalDependencies) * 62;
      SetLoadingPhase(
        "Starting the editor…",
        progress,
        `${left} runtime step${left === 1 ? "" : "s"} remaining`,
      );
    }
  },
  onRuntimeInitialized: function() {
    window.__donnerRuntimeInitializedAtMs = performance.now();
    SetLoadingPhase("Opening your workspace…", 96, "Drawing the first editor frame");
    if (window.__donnerFirstFramePresented) {
      RevealEditorAfterFirstFrame();
    }
  },
  locateFile: function(path, prefix) {
    if (path.endsWith(".wasm")) {
      return prefix + "editor.wasm";
    }
    return prefix + path;
  },
  canvas: canvas,
};

canvas.addEventListener("contextmenu", function(event) {
  event.preventDefault();
});

if (window.__donnerCanStartWasm) {
  SetLoadingPhase(
    "Downloading the Geode WebGPU editor…",
    12,
    "Compiling the editor on first visit; later loads use the browser cache",
  );
  const wasmPreload = document.createElement("link");
  wasmPreload.rel = "preload";
  wasmPreload.as = "fetch";
  wasmPreload.type = "application/wasm";
  wasmPreload.crossOrigin = "anonymous";
  wasmPreload.href = "editor.wasm";
  document.head.appendChild(wasmPreload);

  const loader = document.createElement("script");
  loader.async = true;
  loader.type = "text/javascript";
  loader.src = "editor.js";
  loader.addEventListener("error", () => {
    ShowCapabilityError("Unable to load the Geode WebGPU renderer package.");
  });
  document.body.appendChild(loader);
}
