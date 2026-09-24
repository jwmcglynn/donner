// Evaluates the GPU bridge library the WebAssembly build links against, in a plain JavaScript
// runtime, with a stub browser in front of it.
//
// Shared by the bridge's node tests so each drives the real library text rather than a copy of
// its behavior. The stubs record what the library asked of them and settle every promise in
// microtasks, so a test waits on the state it asserts rather than on a number of turns.

import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import vm from "node:vm";

const librarySource = readFileSync(
  new URL("../../../gpu/browser/library_donner_gpu.js", import.meta.url),
  "utf8",
);

/** A canvas whose WebGPU context records what the library did to it. */
export function createCanvas() {
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
      const frame = {
        label: `frame ${this.frames}`,
        destroyed: false,
        destroy() {
          this.destroyed = true;
        },
        createView() {
          return { texture: this };
        },
      };
      return frame;
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

/** A GPU texture that records whether it was destroyed and which views were made of it. */
function createTexture(descriptor) {
  return {
    descriptor,
    destroyed: false,
    views: 0,
    destroy() {
      this.destroyed = true;
    },
    createView() {
      this.views += 1;
      return { texture: this };
    },
  };
}

/** A command encoder that finishes into a buffer naming what was recorded on it. */
function createCommandEncoder() {
  return {
    finish() {
      return { finished: true };
    },
  };
}

/**
 * A GPU device the stub adapter hands out, a fresh one for every request, with a loss promise the
 * test resolves itself. Its queue reports submitted work done once the test lets it, so a completed
 * serial can be observed.
 */
export function createDevice() {
  let lose = () => {};
  const lost = new Promise((resolve) => {
    lose = resolve;
  });
  const queue = {
    submissions: 0,
    writes: 0,
    submit() {
      this.submissions += 1;
    },
    writeBuffer() {
      this.writes += 1;
    },
    onSubmittedWorkDone() {
      return Promise.resolve();
    },
  };
  const device = {
    queue,
    lost,
    destroy() {},
    createBuffer(descriptor) {
      return {
        descriptor,
        destroyed: false,
        destroy() {
          this.destroyed = true;
        },
      };
    },
    createTexture,
    createCommandEncoder,
  };
  return { device, lose };
}

/**
 * Runs the promise chain forward until `reached` is true, or fails naming what never happened.
 *
 * Every stub here settles in microtasks, so this returns on the first or second turn; waiting on
 * the state the assertions read, rather than on a fixed number of turns, is what keeps a change to
 * the library's promise chain a named failure instead of an intermittent one.
 */
export async function until(reached, description) {
  for (let turn = 0; turn < 64; ++turn) {
    if (reached()) {
      return;
    }
    await new Promise((resolve) => setImmediate(resolve));
  }
  assert.fail(`the bridge never reached the state: ${description}`);
}

/**
 * Lets every callback the stubs have settled so far run. They all settle in microtasks, which run
 * before the next turn of the event loop, so one turn is enough. For asserting that something did
 * not happen, where there is no state to wait for.
 */
export async function drain() {
  await new Promise((resolve) => setImmediate(resolve));
}

/**
 * Evaluates the library with a stub browser around it and returns what a test drives it through:
 * its entry points, its shared state, the canvases it can resolve, the devices the stub adapter has
 * handed out and how many times one was asked for, and a heap to pass strings and out-parameters
 * through.
 */
export function loadLibrary() {
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
  const handedOut = [];
  let adapterRequests = 0;
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
    GPUBufferUsage: {
      MAP_READ: 0x0001,
      COPY_SRC: 0x0004,
      COPY_DST: 0x0008,
      INDEX: 0x0010,
      VERTEX: 0x0020,
      UNIFORM: 0x0040,
      STORAGE: 0x0080,
    },
    navigator: {
      gpu: {
        getPreferredCanvasFormat: () => "bgra8unorm",
        requestAdapter: async () => {
          adapterRequests += 1;
          return {
            requestDevice: async () => {
              const created = createDevice();
              handedOut.push(created);
              return created.device;
            },
          };
        },
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
    words,
    /** The device the stub adapter handed out last, or null before any request settles. */
    get device() {
      return handedOut.length === 0 ? null : handedOut[handedOut.length - 1].device;
    },
    /** Every device the stub adapter has handed out, in order. */
    get devices() {
      return handedOut.map((created) => created.device);
    },
    /** Reports the device handed out last lost, with `info` as the browser's reason. */
    lose(info) {
      assert.ok(handedOut.length > 0, "no device has been handed out to lose");
      handedOut[handedOut.length - 1].lose(info);
    },
    /** The identifiers the logical device `handle` holds, or fails the test if it holds none. */
    objects(handle) {
      const record = state.logical?.get(handle);
      assert.ok(record, `logical device ${handle} is not open`);
      return record.objects;
    },
    /** How many times the library asked the browser for an adapter. */
    get adapterRequests() {
      return adapterRequests;
    },
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
    /** Copies 32-bit words into the heap and returns the pointer the library takes. */
    wordArray(values) {
      const pointer = allocate(values.length * 4);
      words.set(values, pointer >> 2);
      return pointer;
    },
    /** Reserves `byteCount` zeroed bytes in the heap and returns their pointer. */
    reserve(byteCount) {
      return allocate(byteCount);
    },
    /** Reserves a 32-bit out-parameter and returns its pointer. */
    outParameter() {
      return allocate(4);
    },
    /** Reads the out-parameter at the given pointer. */
    read(pointer) {
      return words[pointer >> 2];
    },
    /** Reads the text a message entry point wrote into the given buffer. */
    text(pointer, byteCount) {
      return decoder.decode(bytes.subarray(pointer, pointer + byteCount));
    },
  };
}
