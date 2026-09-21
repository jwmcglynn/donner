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

/** Flushes the promise chain the device request runs on. */
async function settle() {
  for (let tick = 0; tick < 8; ++tick) {
    await new Promise((resolve) => setImmediate(resolve));
  }
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
    /** Registers a canvas under \p selector and returns it. */
    addCanvas(selector) {
      const canvas = createCanvas();
      canvases.set(selector, canvas);
      return canvas;
    },
    /** Copies \p text into the heap and returns the pointer and byte length the library takes. */
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
    /** Reads the out-parameter at \p pointer. */
    read(pointer) {
      return words[pointer >> 2];
    },
  };
}

/** Loads the library and drives it to a ready device, the state every surface call needs. */
async function readyBridge() {
  const bridge = loadLibrary();
  assert.equal(bridge.entryPoints.donner_gpu_begin_device_request(), bridge.state.kSuccess);
  await settle();
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

/** Configures the surface \p id at \p size with the alpha mode \p alphaModeCode. */
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
    bridge.entryPoints.donner_gpu_surface_supports_alpha_mode(id, 4000, supported),
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
    configure(bridge, id, { width: 8, height: 8 }, 4000),
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
  assert.equal(bridge.state.objects.has(2), false);
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
  await settle();
  assert.equal(bridge.entryPoints.donner_gpu_is_device_lost(), 1);

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
