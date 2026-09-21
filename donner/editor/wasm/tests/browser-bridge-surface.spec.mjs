// Drives the canvas half of the GPU bridge library the WebAssembly build links against, in a
// plain JavaScript runtime.
//
// The C++ half of the bridge is covered on every host by its own tests through a fake bridge, and
// linking proves the two halves reference the same entry points, but neither runs the library's
// own code: a canvas call that throws, an identifier left behind, or a context that is never
// unconfigured would only show up in a browser. This stands a stub canvas and a stub GPU device
// in front of the real library text so the presentation contract - configure, acquire, abandon,
// destroy - is exercised here instead.

import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";
import vm from "node:vm";

const librarySource = readFileSync(
  new URL("../../../gpu/browser/library_donner_gpu.js", import.meta.url),
  "utf8",
);

// A number this protocol assigns to nothing, for the cases that check a value is refused rather
// than guessed at.
const kUnassignedCode = 4000;

/** A canvas whose WebGPU context records what the library did to it. */
function createCanvas() {
  const context = {
    configuration: null,
    unconfigureCount: 0,
    frames: 0,
    configure(descriptor) {
      this.configuration = descriptor;
    },
    unconfigure() {
      this.unconfigureCount += 1;
      this.configuration = null;
    },
    getCurrentTexture() {
      if (this.configuration === null) {
        throw new Error("the canvas context has not been configured");
      }
      this.frames += 1;
      return { label: `frame ${this.frames}` };
    },
  };
  return {
    width: 0,
    height: 0,
    context,
    getContext(kind) {
      return kind === "webgpu" ? context : null;
    },
  };
}

/** A GPU device the stub adapter hands out, with a loss promise the test resolves itself. */
function createDevice() {
  let lose = () => {};
  const lost = new Promise((resolve) => {
    lose = resolve;
  });
  return { device: { queue: {}, lost, destroy() {} }, lose };
}

/**
 * Runs the promise chain forward until `reached` is true, or fails naming what never happened.
 *
 * Every stub here settles in microtasks, so this returns on the first or second turn; waiting on
 * the state the assertions read, rather than on a fixed number of turns, is what keeps a change to
 * the library's promise chain a named failure instead of an intermittent one.
 */
async function until(reached, description) {
  for (let turn = 0; turn < 64; ++turn) {
    if (reached()) {
      return;
    }
    await new Promise((resolve) => setImmediate(resolve));
  }
  assert.fail(`the bridge never reached the state: ${description}`);
}

/**
 * Evaluates the library with a stub browser around it and returns what a test drives it through:
 * its entry points, its shared state, the canvases it can resolve, and a heap to pass strings and
 * out-parameters through.
 */
function loadLibrary() {
  const heap = new ArrayBuffer(64 * 1024);
  const bytes = new Uint8Array(heap);
  const words = new Uint32Array(heap);
  const encoder = new TextEncoder();
  const decoder = new TextDecoder();
  let next = 8;
  const allocate = (byteCount) => {
    const pointer = next;
    next = (next + byteCount + 7) & ~7;
    return pointer;
  };

  const canvases = new Map();
  const { device, lose } = createDevice();
  const sandbox = {
    HEAPU8: bytes,
    HEAPU32: words,
    UTF8ToString(pointer, byteCount) {
      return decoder.decode(bytes.subarray(pointer, pointer + byteCount));
    },
    lengthBytesUTF8(text) {
      return encoder.encode(text).length;
    },
    stringToUTF8(text, destination, capacity) {
      const encoded = encoder.encode(text);
      const written = Math.min(encoded.length, capacity - 1);
      bytes.set(encoded.subarray(0, written), destination);
      bytes[destination + written] = 0;
    },
    GPUTextureUsage: {
      RENDER_ATTACHMENT: 0x10,
      TEXTURE_BINDING: 0x04,
      COPY_SRC: 0x01,
      COPY_DST: 0x02,
      STORAGE_BINDING: 0x08,
    },
    navigator: {
      gpu: {
        getPreferredCanvasFormat: () => "bgra8unorm",
        requestAdapter: async () => ({ requestDevice: async () => device }),
      },
    },
    document: {
      querySelector: (selector) => canvases.get(selector) ?? null,
    },
    LibraryManager: { library: {} },
    mergeInto(target, source) {
      Object.assign(target, source);
    },
  };

  vm.createContext(sandbox);
  vm.runInContext(librarySource, sandbox);
  const entryPoints = sandbox.LibraryManager.library;
  const state = entryPoints.$DonnerGpu;
  sandbox.DonnerGpu = state;

  return {
    entryPoints,
    state,
    device,
    lose,
    words,
    /** Registers a canvas under the given selector and returns it. */
    addCanvas(selector) {
      const canvas = createCanvas();
      canvases.set(selector, canvas);
      return canvas;
    },
    /** Copies text into the heap and returns the pointer and byte length the library takes. */
    string(text) {
      const encoded = encoder.encode(text);
      const pointer = allocate(encoded.length);
      bytes.set(encoded, pointer);
      return { pointer, length: encoded.length };
    },
    /** Reserves a 32-bit out-parameter and returns its pointer. */
    outParameter() {
      return allocate(4);
    },
    /** Reads the out-parameter at the given pointer. */
    read(pointer) {
      return words[pointer >> 2];
    },
  };
}

/** Loads the library and drives it to a ready device, the state every surface call needs. */
async function readyBridge() {
  const bridge = loadLibrary();
  assert.equal(bridge.entryPoints.donner_gpu_begin_device_request(), bridge.state.kSuccess);
  await until(
    () => bridge.entryPoints.donner_gpu_device_request_state() !== bridge.state.kRequestPending,
    "a settled device request",
  );
  assert.equal(
    bridge.entryPoints.donner_gpu_device_request_state(),
    bridge.state.kRequestReady,
    bridge.state.requestError,
  );
  return bridge;
}

/** Creates a surface over a canvas the document resolves, and returns the bridge and the canvas. */
async function surfaceBridge({ selector = "#canvas", id = 1 } = {}) {
  const bridge = await readyBridge();
  const canvas = bridge.addCanvas(selector);
  const name = bridge.string(selector);
  assert.equal(
    bridge.entryPoints.donner_gpu_create_surface(id, name.pointer, name.length),
    bridge.state.kSuccess,
  );
  return { bridge, canvas, id };
}

/** Configures the named surface at a size with one alpha mode. */
function configure(bridge, id, size, alphaModeCode) {
  return bridge.entryPoints.donner_gpu_configure_surface(
    id,
    bridge.state.formatCode("bgra8unorm"),
    bridge.state.kUsageRenderAttachment,
    size.width,
    size.height,
    alphaModeCode,
  );
}

test("the usage and alpha mode names stand for the numbers the protocol table sends", async () => {
  const bridge = await readyBridge();
  // Copied into this realm's own array: the library is evaluated in a separate one, and a strict
  // comparison against an array built there fails on the prototype rather than the values.
  const codes = [...bridge.state.protocolCodes];

  // The runtime comparison checks this table's contents, which a name bound to the wrong number
  // would still satisfy, so the names are pinned to their places in it here. The offsets are where
  // each group starts: texture formats occupy the first four entries, and the alpha modes are the
  // last group before the object kinds.
  assert.deepEqual(codes.slice(4, 9), [
    bridge.state.kUsageRenderAttachment,
    bridge.state.kUsageTextureBinding,
    bridge.state.kUsageCopySrc,
    bridge.state.kUsageCopyDst,
    bridge.state.kUsageStorageBinding,
  ]);
  assert.deepEqual(codes.slice(58, 61), [
    bridge.state.kAlphaModeOpaque,
    bridge.state.kAlphaModePremultiplied,
    bridge.state.kAlphaModeInherit,
  ]);
  assert.equal(bridge.state.alphaMode(bridge.state.kAlphaModeOpaque), "opaque");
  assert.equal(bridge.state.alphaMode(bridge.state.kAlphaModePremultiplied), "premultiplied");
});

test("a surface takes the WebGPU context of the canvas its selector names", async () => {
  const { bridge, canvas, id } = await surfaceBridge();
  assert.equal(bridge.state.objects.get(id).kind, bridge.state.kSurface);
  assert.equal(bridge.state.objects.get(id).object.context, canvas.context);
});

test("a selector naming no canvas is refused rather than registered", async () => {
  const bridge = await readyBridge();
  const name = bridge.string("#missing");
  assert.equal(
    bridge.entryPoints.donner_gpu_create_surface(1, name.pointer, name.length),
    bridge.state.kFailed,
  );
  assert.equal(bridge.state.objects.has(1), false);
});

test("capabilities report the canvas's preferred format and what a frame can be used for", async () => {
  const { bridge, id } = await surfaceBridge();
  const format = bridge.outParameter();
  const usage = bridge.outParameter();
  assert.equal(
    bridge.entryPoints.donner_gpu_surface_capabilities(id, format, usage),
    bridge.state.kSuccess,
  );
  assert.equal(bridge.read(format), bridge.state.formatCode("bgra8unorm"));
  assert.equal(
    bridge.read(usage),
    bridge.state.kUsageRenderAttachment | bridge.state.kUsageCopySrc,
  );
});

test("the alpha modes reported are the ones the canvas context can be configured with", async () => {
  const { bridge, id } = await surfaceBridge();
  const supported = bridge.outParameter();
  for (
    const alphaModeCode of [
      bridge.state.kAlphaModeOpaque,
      bridge.state.kAlphaModePremultiplied,
      bridge.state.kAlphaModeInherit,
    ]
  ) {
    assert.equal(
      bridge.entryPoints.donner_gpu_surface_supports_alpha_mode(id, alphaModeCode, supported),
      bridge.state.kSuccess,
    );
    assert.equal(bridge.read(supported), 1);
  }

  assert.equal(
    bridge.entryPoints.donner_gpu_surface_supports_alpha_mode(id, kUnassignedCode, supported),
    bridge.state.kSuccess,
  );
  assert.equal(bridge.read(supported), 0);
});

test("configuring sizes the canvas and applies the alpha mode the runtime chose", async () => {
  const { bridge, canvas, id } = await surfaceBridge();
  assert.equal(
    configure(bridge, id, { width: 640, height: 480 }, bridge.state.kAlphaModePremultiplied),
    bridge.state.kSuccess,
  );
  assert.equal(canvas.width, 640);
  assert.equal(canvas.height, 480);
  assert.equal(canvas.context.configuration.format, "bgra8unorm");
  assert.equal(canvas.context.configuration.alphaMode, "premultiplied");
  assert.equal(canvas.context.configuration.device, bridge.device);
});

test("an alpha mode this protocol assigns no code is refused before the canvas is touched", async () => {
  const { bridge, canvas, id } = await surfaceBridge();
  assert.equal(
    configure(bridge, id, { width: 8, height: 8 }, kUnassignedCode),
    bridge.state.kFailed,
  );
  assert.equal(canvas.context.configuration, null);
  assert.equal(canvas.width, 0);
});

test("a frame is taken from the canvas and abandoning it gives the identifier back", async () => {
  const { bridge, canvas, id } = await surfaceBridge();
  assert.equal(
    configure(bridge, id, { width: 8, height: 8 }, bridge.state.kAlphaModeOpaque),
    bridge.state.kSuccess,
  );

  const status = bridge.outParameter();
  assert.equal(
    bridge.entryPoints.donner_gpu_acquire_current_texture(id, 2, status),
    bridge.state.kSuccess,
  );
  assert.equal(bridge.read(status), bridge.state.kSurfaceSuccess);
  assert.equal(bridge.state.objects.get(2).kind, bridge.state.kTexture);
  assert.equal(canvas.context.frames, 1);

  assert.equal(bridge.entryPoints.donner_gpu_abandon_current_texture(id), bridge.state.kSuccess);
  assert.deepEqual([...bridge.state.objects.keys()], [id]);
});

test("acquiring before the context is configured reports the canvas as lost", async () => {
  const { bridge, id } = await surfaceBridge();
  const status = bridge.outParameter();
  assert.equal(
    bridge.entryPoints.donner_gpu_acquire_current_texture(id, 2, status),
    bridge.state.kSuccess,
  );
  assert.equal(bridge.read(status), bridge.state.kSurfaceLost);
  assert.equal(bridge.state.objects.has(2), false);
});

test("destroying a surface unconfigures the canvas context it held", async () => {
  const { bridge, canvas, id } = await surfaceBridge();
  assert.equal(
    configure(bridge, id, { width: 8, height: 8 }, bridge.state.kAlphaModeOpaque),
    bridge.state.kSuccess,
  );

  assert.equal(
    bridge.entryPoints.donner_gpu_destroy_object(bridge.state.kSurface, id),
    bridge.state.kSuccess,
  );
  assert.equal(canvas.context.unconfigureCount, 1);
  assert.equal(bridge.state.objects.has(id), false);
});

test("destroying a surface drops a frame identifier it still named", async () => {
  const { bridge, id } = await surfaceBridge();
  assert.equal(
    configure(bridge, id, { width: 8, height: 8 }, bridge.state.kAlphaModeOpaque),
    bridge.state.kSuccess,
  );
  const status = bridge.outParameter();
  assert.equal(
    bridge.entryPoints.donner_gpu_acquire_current_texture(id, 2, status),
    bridge.state.kSuccess,
  );

  assert.equal(
    bridge.entryPoints.donner_gpu_destroy_object(bridge.state.kSurface, id),
    bridge.state.kSuccess,
  );
  assert.equal(bridge.state.objects.has(2), false);
  assert.equal(bridge.state.objects.size, 0);
});

test("a surface identifier is refused once it names nothing, and so is one of another kind", async () => {
  const { bridge, id } = await surfaceBridge();
  const format = bridge.outParameter();
  const usage = bridge.outParameter();
  assert.equal(
    bridge.entryPoints.donner_gpu_destroy_object(bridge.state.kSurface, id),
    bridge.state.kSuccess,
  );
  assert.equal(
    bridge.entryPoints.donner_gpu_surface_capabilities(id, format, usage),
    bridge.state.kUnknownObject,
  );

  const name = bridge.string("#canvas");
  assert.equal(
    bridge.entryPoints.donner_gpu_create_surface(3, name.pointer, name.length),
    bridge.state.kSuccess,
  );
  assert.equal(
    bridge.entryPoints.donner_gpu_destroy_object(bridge.state.kTexture, 3),
    bridge.state.kWrongObjectKind,
  );
});

test("reconfiguring gives up the frame the canvas had handed over", async () => {
  const { bridge, canvas, id } = await surfaceBridge();
  assert.equal(
    configure(bridge, id, { width: 8, height: 8 }, bridge.state.kAlphaModeOpaque),
    bridge.state.kSuccess,
  );
  const status = bridge.outParameter();
  assert.equal(
    bridge.entryPoints.donner_gpu_acquire_current_texture(id, 2, status),
    bridge.state.kSuccess,
  );

  // Reconfiguring replaces what the context presents, so the frame taken under the old
  // configuration is no longer anything the runtime can be holding.
  assert.equal(
    configure(bridge, id, { width: 16, height: 16 }, bridge.state.kAlphaModeOpaque),
    bridge.state.kSuccess,
  );
  assert.deepEqual([...bridge.state.objects.keys()], [id]);
  assert.equal(canvas.width, 16);

  assert.equal(
    bridge.entryPoints.donner_gpu_acquire_current_texture(id, 3, status),
    bridge.state.kSuccess,
  );
  assert.deepEqual([...bridge.state.objects.keys()], [id, 3]);
});

test("a second frame inside one frame is refused rather than replacing the first", async () => {
  const { bridge, id } = await surfaceBridge();
  assert.equal(
    configure(bridge, id, { width: 8, height: 8 }, bridge.state.kAlphaModeOpaque),
    bridge.state.kSuccess,
  );
  const status = bridge.outParameter();
  assert.equal(
    bridge.entryPoints.donner_gpu_acquire_current_texture(id, 2, status),
    bridge.state.kSuccess,
  );

  // The first identifier still names the canvas's frame; replacing it would leave nothing able to
  // give that frame back.
  assert.equal(
    bridge.entryPoints.donner_gpu_acquire_current_texture(id, 3, status),
    bridge.state.kFailed,
  );
  assert.deepEqual([...bridge.state.objects.keys()], [id, 2]);
});

test("a lost device refuses another frame but still takes back the one it handed over", async () => {
  const { bridge, id } = await surfaceBridge();
  assert.equal(
    configure(bridge, id, { width: 8, height: 8 }, bridge.state.kAlphaModeOpaque),
    bridge.state.kSuccess,
  );
  const status = bridge.outParameter();
  assert.equal(
    bridge.entryPoints.donner_gpu_acquire_current_texture(id, 2, status),
    bridge.state.kSuccess,
  );

  bridge.lose({ reason: "destroyed", message: "the tab was discarded" });
  await until(
    () => bridge.entryPoints.donner_gpu_is_device_lost() === 1,
    "the device reported lost",
  );

  assert.equal(
    bridge.entryPoints.donner_gpu_acquire_current_texture(id, 3, status),
    bridge.state.kDeviceLost,
  );
  assert.equal(bridge.entryPoints.donner_gpu_abandon_current_texture(id), bridge.state.kSuccess);
  assert.equal(bridge.state.objects.has(2), false);
  assert.equal(
    bridge.entryPoints.donner_gpu_destroy_object(bridge.state.kSurface, id),
    bridge.state.kSuccess,
  );
  assert.equal(bridge.state.objects.size, 0);
});
