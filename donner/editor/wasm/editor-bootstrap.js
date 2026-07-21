const canvas = document.getElementById("canvas");
const documentCanvas = document.getElementById("donner-document-canvas");
const documentCanvases = [
  documentCanvas,
  document.getElementById("donner-document-canvas-back"),
];
const loadingScreen = document.getElementById("loading-screen");
const status = document.getElementById("status");
const loadingProgress = document.getElementById("loading-progress");
const loadingProgressFill = document.getElementById("loading-progress-fill");
const loadingDetail = document.getElementById("loading-detail");
const capabilityError = document.getElementById("capability-error");
const capabilityErrorDetail = document.getElementById("capability-error-detail");
let editorRevealed = false;
const documentBitmapContexts = [null, null];
window.__donnerBootstrapStartedAtMs = performance.now();

function ApplyWorkerDocumentSurfaceLayout(layout) {
  const layoutPolicy = window.__donnerWorkerSurfaceLayoutPolicy || "single-visible";
  const plan = typeof globalThis.CreateDonnerWorkerSurfaceLayoutPlan === "function"
    ? globalThis.CreateDonnerWorkerSurfaceLayoutPlan(layout, layoutPolicy)
    : documentCanvases.map((_, slot) => {
      const visibleSlot = Math.max(
        0,
        Math.min(documentCanvases.length - 1, layout.surfaceSlot),
      );
      const accepted = Boolean(layout.visible) && layout.width > 0
        && layout.height > 0
        && slot === visibleSlot;
      return { accepted, compositorVisible: accepted, slot, zIndex: accepted ? 1 : 0 };
    });
  const acceptedSurface = plan.find((surfaceLayout) => surfaceLayout.accepted);
  if (
    acceptedSurface && window.__donnerWorkerSurfaceMode === "direct-surface"
    && typeof window.__donnerObserveDirectSurfaceAcceptance === "function"
  ) {
    window.__donnerObserveDirectSurfaceAcceptance(layout.frameToken);
  }
  for (let slot = 0; slot < documentCanvases.length; ++slot) {
    const surfaceCanvas = documentCanvases[slot];
    if (!surfaceCanvas) {
      continue;
    }
    const surfaceLayout = plan[slot];
    surfaceCanvas.setAttribute(
      "data-direct-surface-visible",
      surfaceLayout.accepted ? "true" : "false",
    );
    surfaceCanvas.setAttribute(
      "data-direct-surface-compositor-resident",
      surfaceLayout.compositorVisible ? "true" : "false",
    );
    surfaceCanvas.setAttribute(
      "data-direct-surface-selection-chrome-baked",
      surfaceLayout.accepted && layout.selectionChromeBaked ? "true" : "false",
    );
    surfaceCanvas.style.zIndex = String(surfaceLayout.zIndex);
    if (!surfaceLayout.compositorVisible) {
      surfaceCanvas.style.visibility = "hidden";
      continue;
    }
    const pixelLayout = typeof globalThis.CreateDonnerWorkerSurfacePixelLayout === "function"
      ? globalThis.CreateDonnerWorkerSurfacePixelLayout(layout, window.devicePixelRatio, {
        height: surfaceCanvas.height,
        width: surfaceCanvas.width,
      })
      : layout;
    surfaceCanvas.style.left = `${pixelLayout.left}px`;
    surfaceCanvas.style.top = `${pixelLayout.top}px`;
    surfaceCanvas.style.width = `${pixelLayout.width}px`;
    surfaceCanvas.style.height = `${pixelLayout.height}px`;
    surfaceCanvas.style.clipPath =
      `inset(${pixelLayout.clipTop}px ${pixelLayout.clipRight}px ${pixelLayout.clipBottom}px ${pixelLayout.clipLeft}px)`;
    surfaceCanvas.style.visibility = "visible";
    if (!surfaceLayout.accepted) {
      continue;
    }
    surfaceCanvas.setAttribute("data-direct-surface-frame", String(pixelLayout.frameToken));
    const accepted = window.__donnerAcceptedPresentation;
    if (accepted?.kind !== "geode" || accepted.token !== pixelLayout.frameToken) {
      window.__donnerAcceptedPresentation = {
        kind: "geode",
        selectionChromeBaked: Boolean(pixelLayout.selectionChromeBaked),
        token: pixelLayout.frameToken,
        presentedAtMs: performance.now(),
      };
    }
    const diagnostic = window.__donnerWorkerSurfaceDiagnostic;
    if (diagnostic?.frameToken === pixelLayout.frameToken && !(diagnostic.acceptedAtMs > 0)) {
      diagnostic.acceptedAtMs = window.__donnerAcceptedPresentation.presentedAtMs;
    }
  }
}

function DrawWorkerDocumentBitmap(slot, bitmap, width, height, frameToken) {
  const surfaceCanvas = documentCanvases[slot];
  if (!surfaceCanvas || !bitmap) {
    bitmap?.close?.();
    return;
  }
  try {
    if (surfaceCanvas.width !== width) {
      surfaceCanvas.width = width;
    }
    if (surfaceCanvas.height !== height) {
      surfaceCanvas.height = height;
    }
    if (!documentBitmapContexts[slot]) {
      documentBitmapContexts[slot] = surfaceCanvas.getContext("bitmaprenderer");
    }
    if (documentBitmapContexts[slot]) {
      documentBitmapContexts[slot].transferFromImageBitmap(bitmap);
    } else {
      const context2d = surfaceCanvas.getContext("2d");
      context2d?.clearRect(0, 0, width, height);
      context2d?.drawImage(bitmap, 0, 0);
    }
    surfaceCanvas.dataset.bitmapBridgeFrame = String(frameToken);
  } finally {
    bitmap.close?.();
  }
}

const bitmapPresentationQueue = typeof globalThis.CreateDonnerBitmapPresentationQueue === "function"
  ? globalThis.CreateDonnerBitmapPresentationQueue(
    DrawWorkerDocumentBitmap,
    ApplyWorkerDocumentSurfaceLayout,
  )
  : null;
window.__donnerApplyWorkerDocumentSurfaceLayout = ApplyWorkerDocumentSurfaceLayout;
window.__donnerRequiresWorkerRuntime = false;
window.__donnerWorkerRuntimeStats = {
  ready: false,
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
  const fallbackHint = window.__donnerBackend === "geode"
    ? "Reload with ?renderer=tiny_skia to use the software renderer."
    : "Open the TinySkia software-renderer build to continue.";
  ShowCapabilityError(
    "The WebGPU renderer could not present its canvas surface. " + fallbackHint,
  );
};

function RevealEditorAfterFirstFrame() {
  if (editorRevealed || !window.__donnerFirstFramePresented) {
    return;
  }
  if (
    window.__donnerRequiresWorkerRuntime
    && !window.__donnerWorkerRuntimeStats?.ready
  ) {
    SetLoadingPhase("Starting the renderer…", 98, "Preparing the WebGPU worker");
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
        deltaY: -Math.log(Math.max(incrementalScale, 0.001)) * 500,
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

window.__donnerCanStartWasm = typeof SharedArrayBuffer !== "undefined";
if (!window.__donnerCanStartWasm) {
  const reason = window.isSecureContext
    ? "This page is secure, but cross-origin isolation is not active."
    : "This page is not running in a secure context.";
  ShowCapabilityError(`${reason} SharedArrayBuffer and Wasm threads are unavailable.`);
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
  stageDonnerDocumentBitmap: function(token, slot, bitmap, width, height) {
    if (!bitmapPresentationQueue) {
      bitmap?.close?.();
      return;
    }
    bitmapPresentationQueue.stage(token, slot, bitmap, width, height);
  },
  commitDonnerDocumentBitmap: function(token, slot) {
    bitmapPresentationQueue?.commit(token, slot);
  },
  discardDonnerDocumentBitmap: function(token) {
    bitmapPresentationQueue?.discard(token);
  },
  updateDonnerBitmapSurfaceLayout: function(layout) {
    bitmapPresentationQueue?.updateLayout(layout);
  },
  reportDonnerWorkerTaskWakeFailure: function(
    failureCount,
    shuttingDown,
    renderRequestDropped,
    thumbnailDropped,
    surfaceUnavailable,
  ) {
    const stats = {
      failureCount,
      shuttingDown: Boolean(shuttingDown),
      renderRequestDropped: Boolean(renderRequestDropped),
      thumbnailDropped: Boolean(thumbnailDropped),
      surfaceUnavailable: Boolean(surfaceUnavailable),
    };
    window.__donnerWorkerTaskWakeFailureStats = stats;
    const message = `Donner Wasm renderer pthread wake rejected (failure ${failureCount}): `
      + "the target mailbox is closed or the proxy queue could not allocate; "
      + `shuttingDown=${stats.shuttingDown}, `
      + `renderRequestDropped=${stats.renderRequestDropped}, `
      + `thumbnailDropped=${stats.thumbnailDropped}`;
    if (stats.surfaceUnavailable) {
      console.error(message);
      window.__donnerReportWorkerSurfaceUnavailable();
    } else {
      console.warn(message);
    }
  },
  reportDonnerWorkerRuntimeInitializationFailure: function() {
    window.__donnerWorkerRuntimeStats = {
      ...window.__donnerWorkerRuntimeStats,
      ready: false,
      failed: true,
      failedAtMs: performance.now(),
    };
    window.__donnerReportWorkerSurfaceUnavailable();
  },
  publishDonnerWorkerRuntimeStats: function(
    initializationMs,
    maskPipelineMs,
    initializationCount,
    workerDeviceCreations,
    headlessDeviceCreations,
  ) {
    window.__donnerHeadlessDeviceCreations = headlessDeviceCreations;
    window.__donnerWorkerRuntimeStats = {
      ready: true,
      initializationMs,
      maskPipelineMs,
      initializationCount,
      workerDeviceCreations,
      readyAtMs: performance.now(),
    };
    RevealEditorAfterFirstFrame();
  },
  publishDonnerWorkerSurfaceDiagnostic: function(
    frameToken,
    samples,
    coloredPixels,
    nonBlackPixels,
    maxChannel,
    textStyleBackgroundPixels,
    textStyleGlyphPixels,
  ) {
    const accepted = window.__donnerAcceptedPresentation;
    window.__donnerWorkerSurfaceDiagnostic = {
      frameToken,
      samples,
      coloredPixels,
      nonBlackPixels,
      maxChannel,
      textStyleBackgroundPixels,
      textStyleGlyphPixels,
      acceptedAtMs: accepted?.kind === "geode" && accepted.token === frameToken
        ? accepted.presentedAtMs
        : 0,
      publishedAtMs: performance.now(),
    };
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
  contextAttributes: {
    preserveDrawingBuffer: true,
  },
};

canvas.addEventListener("contextmenu", function(event) {
  event.preventDefault();
});
canvas.addEventListener("webglcontextlost", function(event) {
  alert("WebGL context lost. Reload the page.");
  event.preventDefault();
}, false);

window.__donnerBackendPromise
  .then((backend) => {
    if (!window.__donnerCanStartWasm) {
      return;
    }
    SetLoadingPhase(
      `Downloading the ${backend.name === "geode" ? "WebGPU" : "software"} editor…`,
      12,
      "Compiling the editor on first visit; later loads use the browser cache",
    );
    // Start the large Wasm transfer immediately. Emscripten cannot request it
    // until editor.js has downloaded and executed; a matching fetch preload
    // lets the browser overlap that glue work with Wasm download/compilation.
    const wasmPreload = document.createElement("link");
    wasmPreload.rel = "preload";
    wasmPreload.as = "fetch";
    wasmPreload.type = "application/wasm";
    wasmPreload.crossOrigin = "anonymous";
    wasmPreload.href = backend.base + "editor.wasm";
    document.head.appendChild(wasmPreload);

    const loader = document.createElement("script");
    loader.async = true;
    loader.type = "text/javascript";
    loader.src = backend.base + "editor.js";
    loader.addEventListener("error", () => {
      ShowCapabilityError(`Unable to load the ${backend.name} renderer package.`);
    });
    document.body.appendChild(loader);
  })
  .catch((error) => {
    if (window.__donnerCanStartWasm) {
      ShowCapabilityError(String(error));
    }
  });
