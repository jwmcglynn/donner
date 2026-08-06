import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";
import vm from "node:vm";

async function loadWorkerSurfaceUtilities() {
  const source = await readFile(new URL("../worker-surface-selector.js", import.meta.url), "utf8");
  const context = vm.createContext({});
  vm.runInContext(source, context, { filename: "worker-surface-selector.js" });
  return context;
}

async function runBootstrapWithoutThreads() {
  const source = await readFile(new URL("../editor-bootstrap.js", import.meta.url), "utf8");
  const elements = {
    canvas: {
      addEventListener() {},
      focus() {},
      hidden: false,
    },
    "loading-screen": { hidden: false },
    status: { hidden: false, textContent: "" },
    "capability-error": { hidden: true },
    "capability-error-detail": { textContent: "" },
  };
  const window = {
    addEventListener() {},
    isSecureContext: false,
  };
  const context = vm.createContext({
    console,
    document: {
      body: { appendChild() {} },
      getElementById: (id) => elements[id],
    },
    performance: { now: () => 0 },
    SharedArrayBuffer: undefined,
    window,
  });
  vm.runInContext(source, context, { filename: "editor-bootstrap.js" });
  await new Promise((resolve) => setImmediate(resolve));
  return { elements, window };
}

async function loadTouchPointerBridge() {
  const source = await readFile(new URL("../editor-bootstrap.js", import.meta.url), "utf8");
  const handlers = new Map();
  const dispatched = [];
  const captured = [];
  const released = [];
  const canvas = {
    addEventListener(type, handler) {
      handlers.set(type, handler);
    },
    dispatchEvent(event) {
      dispatched.push(event);
    },
    focus() {},
    hidden: false,
    releasePointerCapture(pointerId) {
      released.push(pointerId);
    },
    setPointerCapture(pointerId) {
      captured.push(pointerId);
    },
    style: {},
  };
  const elements = {
    canvas,
    "loading-screen": { hidden: false },
    status: { hidden: false, textContent: "" },
    "capability-error": { hidden: true },
    "capability-error-detail": { textContent: "" },
  };
  const window = {
    addEventListener() {},
    isSecureContext: false,
    PointerEvent: function PointerEvent() {},
  };
  class MouseEvent {
    constructor(type, init) {
      this.type = type;
      Object.assign(this, init);
    }
  }
  class WheelEvent extends MouseEvent {}
  const context = vm.createContext({
    console,
    document: {
      body: { appendChild() {} },
      getElementById: (id) => elements[id],
    },
    MouseEvent,
    performance: { now: () => 0 },
    WheelEvent,
    SharedArrayBuffer: undefined,
    window,
  });
  vm.runInContext(source, context, { filename: "editor-bootstrap.js" });
  await new Promise((resolve) => setImmediate(resolve));
  return { canvas, captured, dispatched, handlers, released };
}

async function loadReadyHandoff(
  { devicePixelRatio, workerSurfaceUtilities = false } = {},
) {
  const source = await readFile(new URL("../editor-bootstrap.js", import.meta.url), "utf8");
  const windowHandlers = new Map();
  const timers = [];
  const loadingClasses = new Set();
  const consoleMessages = { error: [], warn: [] };
  const drawImageCalls = [];
  const bitmapTransfers = [];
  let focusCount = 0;
  let nowMs = 100;
  const canvas = {
    addEventListener() {},
    focus() {
      focusCount += 1;
    },
    hidden: false,
  };
  const loadingProgress = {
    classList: { add() {}, remove() {} },
    removeAttribute() {},
    setAttribute() {},
  };
  const makeDocumentCanvas = () => ({
    attributes: new Map(),
    dataset: {},
    getContext(type) {
      if (type === "bitmaprenderer") {
        return {
          transferFromImageBitmap(bitmap) {
            bitmapTransfers.push(bitmap);
          },
        };
      }
      if (type !== "2d") {
        return null;
      }
      return {
        clearRect() {},
        drawImage(...args) {
          drawImageCalls.push(args);
        },
      };
    },
    height: 0,
    setAttribute(name, value) {
      this.attributes.set(name, value);
    },
    style: {},
    width: 0,
  });
  const elements = {
    canvas,
    "donner-document-canvas": makeDocumentCanvas(),
    "donner-document-canvas-back": makeDocumentCanvas(),
    "loading-screen": {
      classList: { add: (name) => loadingClasses.add(name) },
      hidden: false,
    },
    status: { textContent: "" },
    "loading-progress": loadingProgress,
    "loading-progress-fill": { style: {} },
    "loading-detail": { textContent: "" },
    "capability-error": { hidden: true },
    "capability-error-detail": { textContent: "" },
  };
  const window = {
    addEventListener(type, handler) {
      windowHandlers.set(type, handler);
    },
    devicePixelRatio,
    isSecureContext: true,
  };
  const context = vm.createContext({
    console: {
      error: (...args) => consoleMessages.error.push(args.join(" ")),
      log() {},
      warn: (...args) => consoleMessages.warn.push(args.join(" ")),
    },
    document: {
      body: { appendChild() {} },
      createElement: (tagName) => ({
        tagName: tagName.toUpperCase(),
        addEventListener() {},
      }),
      head: { appendChild() {} },
      getElementById: (id) => elements[id],
    },
    SharedArrayBuffer: function SharedArrayBuffer() {},
    performance: { now: () => ++nowMs },
    setTimeout(callback) {
      timers.push(callback);
    },
    window,
  });
  if (workerSurfaceUtilities) {
    const utilities = await loadWorkerSurfaceUtilities();
    context.CreateDonnerBitmapPresentationQueue = utilities.CreateDonnerBitmapPresentationQueue;
    context.CreateDonnerWorkerSurfaceLayoutPlan = utilities.CreateDonnerWorkerSurfaceLayoutPlan;
    context.CreateDonnerWorkerSurfacePixelLayout = utilities.CreateDonnerWorkerSurfacePixelLayout;
  }
  vm.runInContext(source, context, { filename: "editor-bootstrap.js" });
  assert.equal(context.window.__donnerBootstrapStartedAtMs, 101);
  return {
    context,
    bitmapTransfers,
    consoleMessages,
    drawImageCalls,
    elements,
    focusCount: () => focusCount,
    loadingClasses,
    timers,
    windowHandlers,
  };
}

async function loadBootstrapAssetOrder() {
  const source = await readFile(new URL("../editor-bootstrap.js", import.meta.url), "utf8");
  const appended = [];
  const canvas = {
    addEventListener() {},
    focus() {},
    hidden: false,
  };
  const loadingProgress = {
    classList: { add() {}, remove() {} },
    removeAttribute() {},
    setAttribute() {},
  };
  const elements = {
    canvas,
    "donner-document-canvas": null,
    "loading-screen": { classList: { add() {} }, hidden: false },
    status: { textContent: "" },
    "loading-progress": loadingProgress,
    "loading-progress-fill": { style: {} },
    "loading-detail": { textContent: "" },
    "capability-error": { hidden: true },
    "capability-error-detail": { textContent: "" },
  };
  const window = {
    addEventListener() {},
    isSecureContext: true,
  };
  const document = {
    body: { appendChild: (node) => appended.push(node) },
    head: { appendChild: (node) => appended.push(node) },
    createElement: (tagName) => ({
      tagName: tagName.toUpperCase(),
      addEventListener() {},
    }),
    getElementById: (id) => elements[id],
  };
  const context = vm.createContext({
    console,
    document,
    Number,
    performance: { now: () => 0 },
    SharedArrayBuffer: function SharedArrayBuffer() {},
    setTimeout() {},
    window,
  });
  vm.runInContext(source, context, { filename: "editor-bootstrap.js" });
  await new Promise((resolve) => setImmediate(resolve));
  return appended;
}

test("bootstrap publishes the Geode-only served-page backend", async () => {
  const { context } = await loadReadyHandoff();
  assert.equal(context.window.__donnerBackend, "geode");
});

test("bootstrap starts the Wasm download before loading JavaScript glue", async () => {
  const appended = await loadBootstrapAssetOrder();
  assert.equal(appended[0]?.tagName, "LINK");
  assert.equal(appended[0]?.rel, "preload");
  assert.equal(appended[0]?.as, "fetch");
  assert.equal(appended[0]?.type, "application/wasm");
  assert.equal(appended[0]?.href, "editor.wasm");
  assert.equal(appended[1]?.tagName, "SCRIPT");
  assert.equal(appended[1]?.src, "editor.js");
});

test("Safari retains worker frames while Firefox uses one stable direct surface", async () => {
  const context = await loadWorkerSurfaceUtilities();
  const selectWorkerSurface = context.SelectDonnerWorkerSurfaceMode;
  const selectLayoutPolicy = context.SelectDonnerWorkerSurfaceLayoutPolicy;
  const createLayoutPlan = context.CreateDonnerWorkerSurfaceLayoutPlan;
  assert.equal(
    selectWorkerSurface({ browserVendor: "Apple Computer, Inc.", browserEngine: "AppleWebKit" }),
    "bitmap-bridge",
  );
  assert.equal(
    selectWorkerSurface({ browserVendor: "", browserEngine: "Gecko" }),
    "direct-surface",
  );
  assert.equal(
    selectLayoutPolicy({ browserEngine: "Gecko", workerSurfaceMode: "direct-surface" }),
    "single-visible",
  );
  assert.equal(
    selectLayoutPolicy({ browserEngine: "Gecko", workerSurfaceMode: "bitmap-bridge" }),
    "single-visible",
  );
  // Exactly one slot is ever composited: the plan never retains the inactive surface.
  assert.deepEqual(
    [...createLayoutPlan({ visible: true, surfaceSlot: 1, width: 640, height: 400 })]
      .map(({ accepted, zIndex }) => ({ accepted, zIndex })),
    [
      { accepted: false, zIndex: 0 },
      { accepted: true, zIndex: 1 },
    ],
  );
  assert.deepEqual(
    [...createLayoutPlan({ visible: true, surfaceSlot: 0, width: 640, height: 400 })]
      .map(({ accepted, zIndex }) => ({ accepted, zIndex })),
    [
      { accepted: true, zIndex: 1 },
      { accepted: false, zIndex: 0 },
    ],
  );
});

test("worker surface clip overdraw preserves one backing texel per device pixel", async () => {
  const context = await loadWorkerSurfaceUtilities();
  const createPixelLayout = context.CreateDonnerWorkerSurfacePixelLayout;
  assert.equal(typeof createPixelLayout, "function");

  const source = {
    clipBottom: 0.375,
    clipLeft: 0.375,
    clipRight: 0.25,
    clipTop: 0.125,
    height: 399.5,
    left: 100.25,
    top: 50.75,
    width: 639.5,
  };
  const backingSize = { height: 799, width: 1279 };
  const snapped = createPixelLayout(source, 2, backingSize);

  assert.deepEqual(
    {
      clipBottom: snapped.clipBottom,
      clipLeft: snapped.clipLeft,
      clipRight: snapped.clipRight,
      clipTop: snapped.clipTop,
      height: snapped.height,
      left: snapped.left,
      top: snapped.top,
      width: snapped.width,
    },
    {
      clipBottom: 0.25,
      clipLeft: 0.25,
      clipRight: 0.25,
      clipTop: -0.25,
      height: 399.5,
      left: 100.25,
      top: 50.75,
      width: 639.5,
    },
  );

  for (const devicePixelRatio of [1, 1.25, 1.5, 2, 3]) {
    const requested = {
      ...source,
      height: 799 / devicePixelRatio,
      width: 1279 / devicePixelRatio,
    };
    const backing = { height: 799, width: 1279 };
    const plan = createPixelLayout(requested, devicePixelRatio, backing);
    const surface = {
      left: plan.left * devicePixelRatio,
      top: plan.top * devicePixelRatio,
      right: (plan.left + plan.width) * devicePixelRatio,
      bottom: (plan.top + plan.height) * devicePixelRatio,
    };
    const clip = {
      left: (plan.left + plan.clipLeft) * devicePixelRatio,
      top: (plan.top + plan.clipTop) * devicePixelRatio,
      right: (plan.left + plan.width - plan.clipRight) * devicePixelRatio,
      bottom: (plan.top + plan.height - plan.clipBottom) * devicePixelRatio,
    };
    for (const edge of Object.values(clip)) {
      assert.ok(Math.abs(edge - Math.round(edge)) < 1e-9);
    }

    assert.equal(plan.left, requested.left);
    assert.equal(plan.top, requested.top);
    assert.equal(plan.width * devicePixelRatio, backing.width);
    assert.equal(plan.height * devicePixelRatio, backing.height);
    const rawRight = (requested.left + requested.width) * devicePixelRatio;
    const rawBottom = (requested.top + requested.height) * devicePixelRatio;
    const rawClipRight = rawRight - requested.clipRight * devicePixelRatio;
    const rawClipBottom = rawBottom - requested.clipBottom * devicePixelRatio;
    assert.ok(clip.left <= (requested.left + requested.clipLeft) * devicePixelRatio);
    assert.ok(clip.top <= (requested.top + requested.clipTop) * devicePixelRatio);
    assert.ok(clip.right >= rawClipRight);
    assert.ok(clip.bottom >= rawClipBottom);
    assert.ok(surface.right > surface.left);
    assert.ok(surface.bottom > surface.top);
  }
});

test("worker surface follows live viewport zoom while reusing the accepted backing store", async () => {
  const context = await loadWorkerSurfaceUtilities();
  const createPixelLayout = context.CreateDonnerWorkerSurfacePixelLayout;
  assert.equal(typeof createPixelLayout, "function");

  // The accepted worker frame was rasterized at 892x512 CSS pixels on a 2x display. While a
  // replacement frame is in flight, the editor immediately zooms that same document rect to
  // 1115x640 CSS pixels. The document canvas must follow the live viewport geometry just like the
  // ImGui path overlay; the backing dimensions describe texture resolution, not CSS geometry.
  const liveLayout = {
    clipBottom: 0,
    clipLeft: 0,
    clipRight: 206.5,
    clipTop: 0,
    height: 640,
    left: 85.5,
    top: 211.5,
    width: 1115,
  };
  const plan = createPixelLayout(liveLayout, 2, { height: 1024, width: 1784 });

  assert.equal(plan.left, liveLayout.left);
  assert.equal(plan.top, liveLayout.top);
  assert.equal(plan.width, liveLayout.width);
  assert.equal(plan.height, liveLayout.height);
  assert.equal((plan.left + plan.clipLeft) * 2 % 1, 0);
  assert.equal((plan.top + plan.clipTop) * 2 % 1, 0);
  assert.equal((plan.left + plan.width - plan.clipRight) * 2 % 1, 0);
  assert.equal((plan.top + plan.height - plan.clipBottom) * 2 % 1, 0);
});

test("worker surface never overdraws beyond an unclipped document edge", async () => {
  const context = await loadWorkerSurfaceUtilities();
  const createPixelLayout = context.CreateDonnerWorkerSurfacePixelLayout;
  const source = {
    clipBottom: 0,
    clipLeft: 0,
    clipRight: 0,
    clipTop: 0,
    height: 399.5,
    left: 100.25,
    top: 50.75,
    width: 639.5,
  };

  const plan = createPixelLayout(source, 2, { height: 799, width: 1279 });

  assert.equal(plan.clipLeft, 0);
  assert.equal(plan.clipTop, 0);
  assert.equal(plan.clipRight, 0);
  assert.equal(plan.clipBottom, 0);
});

test("Safari bitmap surface quantizes a fractional pan without resampling its backing", async () => {
  const context = await loadWorkerSurfaceUtilities();
  const createPixelLayout = context.CreateDonnerWorkerSurfacePixelLayout;
  const plan = createPixelLayout(
    {
      clipBottom: 0,
      clipLeft: 0,
      clipRight: 0,
      clipTop: 0,
      height: 400,
      left: 285.75,
      top: 274.125,
      width: 640,
    },
    2,
    { height: 800, snapToDevicePixels: true, width: 1280 },
  );

  assert.deepEqual(
    {
      bottom: (plan.top + plan.height - plan.clipBottom) * 2,
      height: plan.height,
      left: (plan.left + plan.clipLeft) * 2,
      right: (plan.left + plan.width - plan.clipRight) * 2,
      top: (plan.top + plan.clipTop) * 2,
      width: plan.width,
    },
    {
      bottom: 1348,
      height: 400,
      left: 572,
      right: 1852,
      top: 548,
      width: 640,
    },
  );
});

test("bitmap bridge retains zero-copy handoff and presents backing texels without stretching", async () => {
  const { bitmapTransfers, context, drawImageCalls, elements } = await loadReadyHandoff({
    devicePixelRatio: 2,
    workerSurfaceUtilities: true,
  });
  context.window.__donnerWorkerSurfaceMode = "bitmap-bridge";
  const accepted = elements["donner-document-canvas-back"];
  let bitmapClosed = false;
  const bitmap = {
    close() {
      bitmapClosed = true;
    },
  };
  const layout = {
    clipBottom: 0,
    clipLeft: 0,
    clipRight: 0,
    clipTop: 0,
    frameToken: 9,
    height: 399.5,
    left: 100.25,
    surfaceSlot: 1,
    top: 50.75,
    visible: true,
    width: 639.5,
  };
  context.Module.stageDonnerDocumentBitmap(9, 1, bitmap, 1279, 799);
  context.Module.updateDonnerBitmapSurfaceLayout(layout);
  context.Module.commitDonnerDocumentBitmap(9, 1);

  assert.equal(bitmapClosed, true);
  assert.equal(bitmapTransfers.length, 1);
  assert.equal(drawImageCalls.length, 0);
  assert.equal(accepted.width, 1279);
  assert.equal(accepted.height, 799);
  assert.equal(accepted.style.left, "0px");
  assert.equal(accepted.style.top, "0px");
  assert.equal(accepted.style.transform, "translate3d(100.5px, 51px, 0)");
  assert.equal(accepted.style.width, "639.5px");
  assert.equal(accepted.style.height, "399.5px");
  assert.equal(accepted.style.clipPath, "inset(0px 0px 0px 0px)");
  assert.equal(Number.parseFloat(accepted.style.width) * 2, accepted.width);
  assert.equal(Number.parseFloat(accepted.style.height) * 2, accepted.height);
  assert.equal(accepted.style.visibility, "visible");
  assert.equal(accepted.attributes.get("data-direct-surface-frame"), "9");
  assert.equal(
    elements["donner-document-canvas"].attributes.get("data-direct-surface-visible"),
    "false",
  );
});

test("bitmap worker frames draw only into the back buffer until their result is accepted", async () => {
  const context = await loadWorkerSurfaceUtilities();
  assert.equal(typeof context.CreateDonnerBitmapPresentationQueue, "function");

  const drawn = [];
  const visibleLayouts = [];
  const queue = context.CreateDonnerBitmapPresentationQueue(
    (slot, bitmap, width, height) => drawn.push([slot, bitmap.label, width, height]),
    (layout) => visibleLayouts.push({ ...layout }),
  );
  const bitmap = { label: "drag-7", close() {} };
  const layout = { visible: true, surfaceSlot: 1, frameToken: 7 };

  queue.stage(7, 1, bitmap, 80, 40);
  queue.updateLayout(layout);
  assert.deepEqual(drawn, [[1, "drag-7", 80, 40]]);
  assert.deepEqual(visibleLayouts, []);
  queue.commit(7, 1);
  assert.deepEqual(visibleLayouts, [layout]);
});

test("bitmap back-buffer staging handles late frames and closes canceled bitmaps", async () => {
  const context = await loadWorkerSurfaceUtilities();
  assert.equal(typeof context.CreateDonnerBitmapPresentationQueue, "function");

  const closed = [];
  const drawn = [];
  const visibleSlots = [];
  const bitmap = (label) => ({
    label,
    close() {
      closed.push(label);
    },
  });
  const queue = context.CreateDonnerBitmapPresentationQueue(
    (slot, frame) => {
      drawn.push([slot, frame.label]);
      frame.close();
    },
    (layout) => visibleSlots.push(layout.surfaceSlot),
  );

  queue.commit(8, 1);
  queue.updateLayout({ visible: true, surfaceSlot: 1, frameToken: 8 });
  assert.deepEqual(visibleSlots, []);
  queue.stage(8, 1, bitmap("committed-before-stage"), 80, 40);
  assert.deepEqual(visibleSlots, [1]);

  queue.stage(9, 0, bitmap("hidden-canceled"), 80, 40);
  queue.discard(9);
  queue.discard(10);
  queue.stage(10, 0, bitmap("late-after-cancel"), 80, 40);

  assert.deepEqual(drawn, [[1, "committed-before-stage"], [0, "hidden-canceled"]]);
  assert.deepEqual(closed, ["committed-before-stage", "hidden-canceled", "late-after-cancel"]);
  assert.deepEqual(visibleSlots, [1]);
});

test("touch pointer bridge emits one captured mouse drag", async () => {
  const { canvas, captured, dispatched, handlers, released } = await loadTouchPointerBridge();
  const pointer = (type, overrides = {}) => ({
    clientX: 12,
    clientY: 34,
    pointerId: 7,
    pointerType: "touch",
    preventDefault() {},
    type,
    ...overrides,
  });

  handlers.get("pointerdown")(pointer("pointerdown"));
  handlers.get("pointermove")(pointer("pointermove", { clientX: 20 }));
  handlers.get("pointerdown")(pointer("pointerdown", { pointerId: 8 }));
  handlers.get("pointerup")(pointer("pointerup"));

  assert.equal(canvas.style.touchAction, "none");
  assert.deepEqual(captured, [7]);
  assert.deepEqual(released, [7]);
  assert.deepEqual(
    dispatched.map((event) => [event.type, event.clientX, event.buttons]),
    [
      ["mousedown", 12, 1],
      ["mousemove", 20, 1],
      ["mouseup", 12, 0],
    ],
  );
});

test("trackpad gesture bridge prevents page zoom and emits editor wheel zoom", async () => {
  const { dispatched, handlers } = await loadTouchPointerBridge();
  let prevented = 0;
  const gesture = (type, scale) => ({
    clientX: 320,
    clientY: 240,
    preventDefault() {
      prevented += 1;
    },
    scale,
    type,
  });

  assert.equal(typeof handlers.get("gesturestart"), "function");
  assert.equal(typeof handlers.get("gesturechange"), "function");
  handlers.get("gesturestart")(gesture("gesturestart", 1));
  handlers.get("gesturechange")(gesture("gesturechange", 1.25));

  assert.equal(prevented, 2);
  assert.deepEqual(
    dispatched.map((event) => [event.type, event.ctrlKey, Math.sign(event.deltaY)]),
    [["wheel", true, -1]],
  );
});

test("loading screen remains until the editor presents its first frame", async () => {
  const { context, elements, focusCount, loadingClasses, timers, windowHandlers } =
    await loadReadyHandoff();

  context.Module.onRuntimeInitialized();
  assert.equal(elements["loading-screen"].hidden, false);
  assert.equal(focusCount(), 0);

  context.window.__donnerFirstFramePresented = true;
  windowHandlers.get("donner:first-frame-presented")();
  assert.equal(context.window.__donnerRuntimeInitializedAtMs, 102);
  assert.equal(context.window.__donnerFirstFramePresentedAtMs, 103);
  assert.equal(context.window.__donnerEditorRevealedAtMs, 104);
  assert.equal(loadingClasses.has("is-complete"), true);
  assert.equal(elements["loading-screen"].hidden, false);
  assert.equal(focusCount(), 1);

  timers.shift()();
  assert.equal(context.window.__donnerLoadingScreenHiddenAtMs, 105);
  assert.equal(elements["loading-screen"].hidden, true);
});

test("Geode loading waits for both the first frame and worker runtime", async () => {
  const { context, elements, focusCount, loadingClasses, timers, windowHandlers } =
    await loadReadyHandoff();

  context.window.__donnerRequiresWorkerRuntime = true;
  context.Module.onRuntimeInitialized();
  context.window.__donnerFirstFramePresented = true;
  windowHandlers.get("donner:first-frame-presented")();
  assert.equal(context.window.__donnerRuntimeInitializedAtMs, 102);
  assert.equal(context.window.__donnerFirstFramePresentedAtMs, 103);
  assert.equal(loadingClasses.has("is-complete"), false);
  assert.equal(elements["loading-screen"].hidden, false);
  assert.equal(focusCount(), 0);

  context.Module.publishDonnerWorkerRuntimeStats(42, 7, 1, 1, 1);
  assert.deepEqual({ ...context.window.__donnerWorkerRuntimeStats }, {
    ready: true,
    initializationMs: 42,
    maskPipelineMs: 7,
    initializationCount: 1,
    workerDeviceCreations: 1,
    readyAtMs: 104,
  });
  assert.equal(context.window.__donnerHeadlessDeviceCreations, 1);
  assert.equal(context.window.__donnerEditorRevealedAtMs, 105);
  assert.equal(loadingClasses.has("is-complete"), true);
  assert.equal(focusCount(), 1);

  timers.shift()();
  assert.equal(context.window.__donnerLoadingScreenHiddenAtMs, 106);
  assert.equal(elements["loading-screen"].hidden, true);
});

test("worker surface diagnostics retain acceptance for either message order", async () => {
  for (const diagnosticFirst of [true, false]) {
    const { context } = await loadReadyHandoff();
    const publishDiagnostic = () =>
      context.Module.publishDonnerWorkerSurfaceDiagnostic(7, 100, 20, 80, 255, 60, 12);
    const acceptSurface = () =>
      context.window.__donnerApplyWorkerDocumentSurfaceLayout({
        clipBottom: 0,
        clipLeft: 0,
        clipRight: 0,
        clipTop: 0,
        frameToken: 7,
        height: 360,
        left: 0,
        surfaceSlot: 0,
        top: 0,
        visible: true,
        width: 640,
      });

    if (diagnosticFirst) {
      publishDiagnostic();
      assert.equal(context.window.__donnerWorkerSurfaceDiagnostic.acceptedAtMs, 0);
      acceptSurface();
    } else {
      acceptSurface();
      publishDiagnostic();
    }

    assert.equal(context.window.__donnerAcceptedPresentation.token, 7);
    assert.equal(
      context.window.__donnerWorkerSurfaceDiagnostic.acceptedAtMs,
      context.window.__donnerAcceptedPresentation.presentedAtMs,
    );
    assert.ok(context.window.__donnerWorkerSurfaceDiagnostic.acceptedAtMs > 0);
  }
});

test("terminal worker-surface failure uses the capability error handoff", async () => {
  const { context, elements } = await loadReadyHandoff();

  assert.equal(typeof context.window.__donnerReportWorkerSurfaceUnavailable, "function");
  context.window.__donnerReportWorkerSurfaceUnavailable();

  assert.equal(elements["capability-error"].hidden, false);
  assert.equal(elements.canvas.hidden, true);
  assert.match(elements["capability-error-detail"].textContent, /WebGPU.*surface/i);
  assert.match(elements["capability-error-detail"].textContent, /Geode WebGPU/i);
  assert.match(elements["capability-error-detail"].textContent, /enable WebGPU/i);
  assert.doesNotMatch(elements["capability-error-detail"].textContent, /\?renderer=/);
});

test("renderer pthread can report terminal wake failure through the main handler", async () => {
  const { consoleMessages, context, elements } = await loadReadyHandoff();

  assert.equal(typeof context.Module.reportDonnerWorkerTaskWakeFailure, "function");
  context.Module.reportDonnerWorkerTaskWakeFailure(1, 0, 1, 0, 0);
  assert.ok(consoleMessages.warn.some((message) => /pthread wake rejected/.test(message)));
  assert.equal(
    consoleMessages.error.some((message) => /pthread wake rejected/.test(message)),
    false,
  );
  assert.equal(elements["capability-error"].hidden, true);

  context.Module.reportDonnerWorkerTaskWakeFailure(2, 0, 1, 0, 1);

  assert.deepEqual({ ...context.window.__donnerWorkerTaskWakeFailureStats }, {
    failureCount: 2,
    shuttingDown: false,
    renderRequestDropped: true,
    thumbnailDropped: false,
    surfaceUnavailable: true,
  });
  assert.equal(elements["capability-error"].hidden, false);
  assert.ok(consoleMessages.error.some((message) => /pthread wake rejected/.test(message)));
  assert.equal(elements.canvas.hidden, true);
  assert.match(elements["capability-error-detail"].textContent, /enable WebGPU/i);
});

test("bootstrap reports the capability error and skips the download without threads", async () => {
  let unhandledRejection;
  const onUnhandledRejection = (reason) => {
    unhandledRejection = reason;
  };
  process.on("unhandledRejection", onUnhandledRejection);
  const { elements, window } = await runBootstrapWithoutThreads();
  process.off("unhandledRejection", onUnhandledRejection);

  assert.equal(unhandledRejection, undefined);
  assert.equal(window.__donnerCanStartWasm, false);
  assert.equal(elements["capability-error"].hidden, false);
  assert.match(elements["capability-error-detail"].textContent, /SharedArrayBuffer/);
});
