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
import test from "node:test";

import { loadLibrary, until } from "./bridge-library-harness.mjs";

// A number this protocol assigns to nothing, for the cases that check a value is refused rather
// than guessed at.
const kUnassignedCode = 4000;

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
