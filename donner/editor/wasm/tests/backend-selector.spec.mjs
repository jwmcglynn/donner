import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";
import vm from "node:vm";

async function loadSelector() {
  const source = await readFile(new URL("../backend-selector.js", import.meta.url), "utf8");
  const context = vm.createContext({});
  vm.runInContext(source, context, { filename: "backend-selector.js" });
  assert.equal(typeof context.SelectDonnerBackend, "function");
  return context.SelectDonnerBackend;
}

async function loadBrowserSelector({ hasWebGl2 }) {
  const source = await readFile(new URL("../backend-selector.js", import.meta.url), "utf8");
  const window = { location: { search: "" } };
  const context = vm.createContext({
    URLSearchParams,
    document: {
      createElement: () => ({
        getContext: () =>
          hasWebGl2
            ? { getExtension: () => ({ loseContext() {} }) }
            : null,
      }),
    },
    navigator: {},
    window,
  });
  vm.runInContext(source, context, { filename: "backend-selector.js" });
  return { selection: window.__donnerBackendPromise };
}

async function runBootstrapWithoutThreads() {
  const source = await readFile(new URL("../editor-bootstrap.js", import.meta.url), "utf8");
  const elements = {
    canvas: {
      addEventListener() {},
      focus() {},
      hidden: false,
    },
    status: { hidden: false, textContent: "" },
    "capability-error": { hidden: true },
    "capability-error-detail": { textContent: "" },
  };
  const window = {
    __donnerBackendPromise: Promise.reject(new Error("WebGL2 is unavailable")),
    isSecureContext: false,
  };
  const context = vm.createContext({
    console,
    document: {
      body: { appendChild() {} },
      getElementById: (id) => elements[id],
    },
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
    status: { hidden: false, textContent: "" },
    "capability-error": { hidden: true },
    "capability-error-detail": { textContent: "" },
  };
  const window = {
    __donnerBackendPromise: Promise.reject(new Error("WebGL2 is unavailable")),
    isSecureContext: false,
    PointerEvent: function PointerEvent() {},
  };
  class MouseEvent {
    constructor(type, init) {
      this.type = type;
      Object.assign(this, init);
    }
  }
  const context = vm.createContext({
    console,
    document: {
      body: { appendChild() {} },
      getElementById: (id) => elements[id],
    },
    MouseEvent,
    SharedArrayBuffer: undefined,
    window,
  });
  vm.runInContext(source, context, { filename: "editor-bootstrap.js" });
  await new Promise((resolve) => setImmediate(resolve));
  return { canvas, captured, dispatched, handlers, released };
}

test("auto mode prefers Geode when WebGPU has an adapter", async () => {
  const selectBackend = await loadSelector();
  const result = await selectBackend({
    requestedBackend: "auto",
    requestWebGpuAdapter: async () => ({}),
    hasWebGl2: () => true,
  });
  assert.deepEqual({ ...result }, { name: "geode", base: "geode/" });
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

test("auto mode falls back to TinySkia when WebGPU is absent", async () => {
  const selectBackend = await loadSelector();
  const result = await selectBackend({
    requestedBackend: "auto",
    requestWebGpuAdapter: null,
    hasWebGl2: () => true,
  });
  assert.deepEqual({ ...result }, { name: "tiny_skia", base: "tiny_skia/" });
});

test("auto mode falls back when WebGPU exposes no adapter", async () => {
  const selectBackend = await loadSelector();
  const result = await selectBackend({
    requestedBackend: "auto",
    requestWebGpuAdapter: async () => null,
    hasWebGl2: () => true,
  });
  assert.deepEqual({ ...result }, { name: "tiny_skia", base: "tiny_skia/" });
});

test("an explicit TinySkia request does not probe WebGPU", async () => {
  const selectBackend = await loadSelector();
  let webGpuProbes = 0;
  const result = await selectBackend({
    requestedBackend: "tiny_skia",
    requestWebGpuAdapter: async () => {
      webGpuProbes += 1;
      return {};
    },
    hasWebGl2: () => true,
  });
  assert.deepEqual({ ...result }, { name: "tiny_skia", base: "tiny_skia/" });
  assert.equal(webGpuProbes, 0);
});

test("rejects a forced Geode backend without a WebGPU adapter", async () => {
  const selectBackend = await loadSelector();
  await assert.rejects(
    selectBackend({
      requestedBackend: "geode",
      requestWebGpuAdapter: async () => null,
      hasWebGl2: () => true,
    }),
    /WebGPU adapter/,
  );
});

test("rejects TinySkia when WebGL2 is unavailable", async () => {
  const selectBackend = await loadSelector();
  await assert.rejects(
    selectBackend({
      requestedBackend: "auto",
      requestWebGpuAdapter: null,
      hasWebGl2: () => false,
    }),
    /WebGL2/,
  );
});

test("rejects unknown backend overrides", async () => {
  const selectBackend = await loadSelector();
  await assert.rejects(
    selectBackend({
      requestedBackend: "metal",
      requestWebGpuAdapter: async () => ({}),
      hasWebGl2: () => true,
    }),
    /Unknown renderer backend/,
  );
});

test("browser capability rejection is handled by the published promise", async () => {
  const { selection } = await loadBrowserSelector({ hasWebGl2: false });
  await assert.rejects(selection, /WebGL2/);
  await new Promise((resolve) => setImmediate(resolve));
});

test("bootstrap consumes backend rejection when threads are unavailable", async () => {
  let unhandledRejection;
  const onUnhandledRejection = (reason) => {
    unhandledRejection = reason;
  };
  process.on("unhandledRejection", onUnhandledRejection);
  const { elements } = await runBootstrapWithoutThreads();
  process.off("unhandledRejection", onUnhandledRejection);

  assert.equal(unhandledRejection, undefined);
  assert.equal(elements["capability-error"].hidden, false);
  assert.match(elements["capability-error-detail"].textContent, /SharedArrayBuffer/);
});
