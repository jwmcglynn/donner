import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";
import vm from "node:vm";

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

async function loadReadyHandoff({ devicePixelRatio } = {}) {
  const source = await readFile(new URL("../editor-bootstrap.js", import.meta.url), "utf8");
  const windowHandlers = new Map();
  const timers = [];
  const loadingClasses = new Set();
  const consoleMessages = { error: [], warn: [] };
  let focusCount = 0;
  let nowMs = 100;
  const canvas = {
    addEventListener() {},
    focus() {
      focusCount += 1;
    },
    hidden: false,
    style: {},
    transferControlToOffscreen() {},
  };
  const loadingProgress = {
    classList: { add() {}, remove() {} },
    removeAttribute() {},
    setAttribute() {},
  };
  const elements = {
    canvas,
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
    Event: function Event(type) {
      this.type = type;
    },
    OffscreenCanvas: function OffscreenCanvas() {},
    SharedArrayBuffer: function SharedArrayBuffer() {},
    performance: { now: () => ++nowMs, timeOrigin: 0 },
    setTimeout(callback) {
      timers.push(callback);
    },
    window,
  });
  vm.runInContext(source, context, { filename: "editor-bootstrap.js" });
  assert.equal(context.window.__donnerBootstrapStartedAtMs, 101);
  return {
    context,
    consoleMessages,
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
    style: {},
    transferControlToOffscreen() {},
  };
  const loadingProgress = {
    classList: { add() {}, remove() {} },
    removeAttribute() {},
    setAttribute() {},
  };
  const elements = {
    canvas,
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
    OffscreenCanvas: function OffscreenCanvas() {},
    performance: { now: () => 0, timeOrigin: 0 },
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

test("trackpad gesture bridge synthesizes ungained pinch wheel deltas", async () => {
  // The WebKit gesture bridge is a shape adapter, not a gain stage: it must emit
  // the same UNGAINED deltaY that Chromium and Gecko synthesize natively, so the
  // editor's pinch discriminator stays the one gain authority. Pre-multiplying
  // by 1/ln(1.1) here (deltaY = -1049.2059 * ln(scale)) gains Safari pinch input
  // twice. See donner/editor/PinchZoomPolicy.h.
  const kWheelPixelsPerScrollUnit = 100;
  const { dispatched, handlers } = await loadTouchPointerBridge();
  const gesture = (type, scale) => ({
    clientX: 320,
    clientY: 240,
    preventDefault() {},
    scale,
    type,
  });

  // WebKit reports `scale` relative to the gesture start, and the bridge emits
  // the incremental scale since the previous event.
  const incrementalScales = [1.05, 1.2, 0.8];
  handlers.get("gesturestart")(gesture("gesturestart", 1));
  let absoluteScale = 1;
  for (const incrementalScale of incrementalScales) {
    absoluteScale *= incrementalScale;
    handlers.get("gesturechange")(gesture("gesturechange", absoluteScale));
  }

  assert.equal(dispatched.length, incrementalScales.length);
  for (const [index, incrementalScale] of incrementalScales.entries()) {
    const expectedDeltaY = -Math.log(incrementalScale) * kWheelPixelsPerScrollUnit;
    assert.ok(
      Math.abs(dispatched[index].deltaY - expectedDeltaY) < 1e-6,
      `gesturechange with incremental scale ${incrementalScale} must synthesize the ungained `
        + `deltaY ${expectedDeltaY}, got ${dispatched[index].deltaY}`,
    );
  }
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
  assert.match(elements["capability-error-detail"].textContent, /secure context/);
});
