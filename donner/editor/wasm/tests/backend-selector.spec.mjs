import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";
import vm from "node:vm";

const bootstrapUrl = new URL("../editor-bootstrap.js", import.meta.url);

function makeCanvas(properties = {}) {
  const canvas = {
    addEventListener() {},
    focusCount: 0,
    focus() {
      canvas.focusCount += 1;
    },
    height: 150,
    hidden: false,
    style: {},
    transferControlToOffscreen() {},
    width: 300,
    ...properties,
  };
  return canvas;
}

function makeWindow(properties = {}) {
  const windowHandlers = new Map();
  const window = {
    addEventListener(type, handler) {
      windowHandlers.set(type, handler);
    },
    ...properties,
  };
  return { window, windowHandlers };
}

// Every harness below runs the same script in a fresh vm context and differs
// only in which browser surfaces the script is allowed to see, so the context
// is built once here. The page effects they all care about - appended nodes,
// pending timers and animation frames, console output, loader classes - are
// captured for every caller; a caller that does not read one simply ignores it.
async function runBootstrap({ canvas, window, elements = {}, globals = {}, now } = {}) {
  const source = await readFile(bootstrapUrl, "utf8");
  const animationFrames = [];
  const appended = [];
  const consoleMessages = { error: [], log: [], warn: [] };
  const loadingClasses = new Set();
  const timers = [];
  let defaultNowMs = 100;
  const pageElements = {
    canvas,
    "loading-screen": {
      classList: { add: (name) => loadingClasses.add(name) },
      hidden: false,
    },
    status: { hidden: false, textContent: "" },
    "loading-progress": {
      classList: { add() {}, remove() {} },
      removeAttribute() {},
      setAttribute() {},
    },
    "loading-progress-fill": { style: {} },
    "loading-detail": { textContent: "" },
    "capability-error": { hidden: true },
    "capability-error-detail": { textContent: "" },
    ...elements,
  };
  const context = vm.createContext({
    console: {
      error: (...args) => consoleMessages.error.push(args.join(" ")),
      log: (...args) => consoleMessages.log.push(args.join(" ")),
      warn: (...args) => consoleMessages.warn.push(args.join(" ")),
    },
    document: {
      body: { appendChild: (node) => appended.push(node) },
      createElement: (tagName) => ({ tagName: tagName.toUpperCase(), addEventListener() {} }),
      getElementById: (id) => pageElements[id],
      head: { appendChild: (node) => appended.push(node) },
    },
    Event: function Event(type) {
      this.type = type;
    },
    performance: { now: now ?? (() => ++defaultNowMs), timeOrigin: 0 },
    requestAnimationFrame: (callback) => animationFrames.push(callback),
    setTimeout: (callback, delayMs) => timers.push([callback, delayMs]),
    window,
    ...globals,
  });
  vm.runInContext(source, context, { filename: "editor-bootstrap.js" });
  return {
    animationFrames,
    appended,
    canvas,
    consoleMessages,
    context,
    elements: pageElements,
    loadingClasses,
    timers,
  };
}

async function runBootstrapWithoutThreads() {
  const { window, windowHandlers } = makeWindow({ isSecureContext: false });
  const harness = await runBootstrap({
    canvas: makeCanvas(),
    globals: { SharedArrayBuffer: undefined },
    window,
  });
  await new Promise((resolve) => setImmediate(resolve));
  return { ...harness, window, windowHandlers };
}

async function loadTouchPointerBridge() {
  const handlers = new Map();
  const dispatched = [];
  const captured = [];
  const released = [];
  const canvas = makeCanvas({
    addEventListener(type, handler) {
      handlers.set(type, handler);
    },
    dispatchEvent(event) {
      dispatched.push(event);
    },
    releasePointerCapture(pointerId) {
      released.push(pointerId);
    },
    setPointerCapture(pointerId) {
      captured.push(pointerId);
    },
  });
  class MouseEvent {
    constructor(type, init) {
      this.type = type;
      Object.assign(this, init);
    }
  }
  class WheelEvent extends MouseEvent {}
  const { window } = makeWindow({
    isSecureContext: false,
    PointerEvent: function PointerEvent() {},
  });
  await runBootstrap({
    canvas,
    globals: { MouseEvent, SharedArrayBuffer: undefined, WheelEvent },
    window,
  });
  await new Promise((resolve) => setImmediate(resolve));
  return { canvas, captured, dispatched, handlers, released };
}

// A page that can start the editor, so the reveal path is reachable.
async function loadReadyHandoff({ now } = {}) {
  const { window, windowHandlers } = makeWindow({ isSecureContext: true });
  const harness = await runBootstrap({
    canvas: makeCanvas(),
    globals: {
      OffscreenCanvas: function OffscreenCanvas() {},
      SharedArrayBuffer: function SharedArrayBuffer() {},
    },
    now,
    window,
  });
  return { ...harness, windowHandlers };
}

// Drives the reveal to the point where the app has reported its first presented
// frame and the page is waiting for that frame to reach the canvas.
async function loadReportedFirstFrame(options) {
  const harness = await loadReadyHandoff(options);
  harness.context.Module.onRuntimeInitialized();
  harness.context.window.__donnerFirstFramePresented = true;
  harness.windowHandlers.get("donner:first-frame-presented")();
  return harness;
}

test("bootstrap publishes the Geode-only served-page backend", async () => {
  const { context } = await loadReadyHandoff();
  assert.equal(context.window.__donnerBackend, "geode");
});

test("bootstrap starts the Wasm download before loading JavaScript glue", async () => {
  const { appended } = await loadReadyHandoff();
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

test("the loader stays up until the presented frame reaches the canvas", async () => {
  const { animationFrames, canvas, context, elements, loadingClasses, timers } =
    await loadReportedFirstFrame();

  assert.equal(
    context.window.__donnerBootstrapStartedAtMs,
    101,
    "the bootstrap stamps its start time from the page clock",
  );
  assert.equal(context.window.__donnerRuntimeInitializedAtMs, 102, "runtime init is stamped");
  assert.equal(context.window.__donnerFirstFramePresentedAtMs, 103, "the report is stamped");
  assert.equal(
    loadingClasses.has("is-complete"),
    false,
    "the report alone is not evidence that the frame reached the page",
  );
  assert.equal(canvas.focusCount, 0, "focus moves to the canvas only at the reveal");
  assert.equal(
    animationFrames.length,
    1,
    "the bootstrap must watch for the presented frame instead of revealing on a timer",
  );

  for (let frame = 1; frame <= 5; ++frame) {
    animationFrames.shift()();
    assert.equal(
      loadingClasses.has("is-complete"),
      false,
      `frame ${frame}: the canvas still reports its boot size, so the loader must stay up`,
    );
    assert.equal(
      elements["loading-screen"].hidden,
      false,
      `frame ${frame}: the loader must still be in the page`,
    );
  }

  canvas.width = 1390;
  canvas.height = 1121;
  animationFrames.shift()();
  assert.equal(
    loadingClasses.has("is-complete"),
    true,
    "a changed backing size is the presented frame reaching the page, so reveal",
  );
  assert.equal(canvas.focusCount, 1, "the reveal moves focus to the canvas exactly once");
  assert.equal(
    elements["loading-screen"].hidden,
    false,
    "the loader leaves the page only after its fade",
  );
  assert.equal(animationFrames.length, 0, "the watch stops once it has revealed");
  assert.ok(
    context.window.__donnerEditorRevealedAtMs > context.window.__donnerFirstFramePresentedAtMs,
    "the reveal must be stamped after the first-frame report",
  );

  const [hideLoader, fadeDelayMs] = timers.shift();
  assert.equal(
    fadeDelayMs,
    220,
    "the loader is hidden after its 160ms opacity transition, not on the next tick",
  );
  hideLoader();
  assert.equal(elements["loading-screen"].hidden, true, "the fade timer removes the loader");
  assert.ok(
    context.window.__donnerLoadingScreenHiddenAtMs >= context.window.__donnerEditorRevealedAtMs,
    "the loader is hidden no earlier than the reveal",
  );
});

test("the editor reveals at once when the presented frame arrived before the report", async () => {
  const { animationFrames, canvas, context, loadingClasses, windowHandlers } =
    await loadReadyHandoff();
  context.Module.onRuntimeInitialized();
  canvas.width = 1390;
  canvas.height = 1121;
  context.window.__donnerFirstFramePresented = true;
  windowHandlers.get("donner:first-frame-presented")();

  assert.equal(
    loadingClasses.has("is-complete"),
    true,
    "the evidence was already on the page, so the reveal must not cost a frame",
  );
  assert.equal(animationFrames.length, 0, "and no watch is needed");
  assert.equal(canvas.focusCount, 1, "the reveal moves focus to the canvas");
});

test("the reveal falls back after a bounded run of frames and says why exactly once", async () => {
  const { animationFrames, canvas, consoleMessages, elements, loadingClasses, timers } =
    await loadReportedFirstFrame();

  assert.equal(
    animationFrames.length,
    1,
    "the reveal must be watching for the presented frame, not sitting on a timer",
  );
  for (let frame = 1; frame < 120; ++frame) {
    animationFrames.shift()();
    assert.equal(
      loadingClasses.has("is-complete"),
      false,
      `frame ${frame}: inside the bound the page keeps waiting for evidence`,
    );
  }
  assert.equal(consoleMessages.warn.length, 0, "no warning before the bound is reached");

  animationFrames.shift()();
  assert.equal(
    loadingClasses.has("is-complete"),
    true,
    "the bound must reveal so an engine that never reports the size still boots",
  );
  assert.equal(canvas.focusCount, 1, "the fallback reveal is a normal reveal");
  assert.equal(animationFrames.length, 0, "the watch stops once the bound has fired");
  assert.equal(consoleMessages.warn.length, 1, "the fallback warns exactly once");
  assert.match(
    consoleMessages.warn[0],
    /without evidence that its first frame reached the page/,
    "the warning must name the condition",
  );
  assert.match(
    consoleMessages.warn[0],
    /300x150/,
    "the warning must report the size it still sees",
  );
  assert.match(consoleMessages.warn[0], /120 animation frames/, "and how long it waited");

  timers.shift()[0]();
  assert.equal(elements["loading-screen"].hidden, true, "the fallback still completes the fade");
});

// A tab switch during the download is the ordinary interruption here: the
// browser stops delivering animation frames, wall-clock time runs on without
// the page observing any of it, and frames resume when the tab comes back.
test("a hidden page does not spend its bound while no frames are delivered", async () => {
  let clockMs = 100;
  const { animationFrames, canvas, consoleMessages, loadingClasses } = await loadReportedFirstFrame(
    {
      now: () => clockMs,
    },
  );

  animationFrames.shift()();
  assert.equal(loadingClasses.has("is-complete"), false, "no evidence yet, so no reveal");

  clockMs += 60000;
  animationFrames.shift()();
  assert.equal(
    loadingClasses.has("is-complete"),
    false,
    "the first frame back must not spend a bound the page was never awake for",
  );
  assert.equal(
    consoleMessages.warn.length,
    0,
    "and must not warn about a wait the page never experienced",
  );

  canvas.width = 1390;
  canvas.height = 1121;
  animationFrames.shift()();
  assert.equal(
    loadingClasses.has("is-complete"),
    true,
    "the resumed page reveals on the evidence, as it would have without the interruption",
  );
  assert.equal(consoleMessages.warn.length, 0, "with no warning, because the evidence arrived");
});

test("a first-frame report cannot reveal a page that cannot start the editor", async () => {
  const { animationFrames, context, loadingClasses, windowHandlers } =
    await runBootstrapWithoutThreads();

  assert.equal(context.window.__donnerCanStartWasm, false, "this page lacks the capabilities");
  context.window.__donnerFirstFramePresented = true;
  windowHandlers.get("donner:first-frame-presented")();

  assert.equal(
    animationFrames.length,
    0,
    "no presented-frame watch may start on a page that cannot run the editor",
  );
  assert.equal(loadingClasses.has("is-complete"), false, "and nothing may be revealed");
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
