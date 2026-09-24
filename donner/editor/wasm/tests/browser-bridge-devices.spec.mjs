// Drives the device half of the GPU bridge library with more than one logical device in one
// worker, in a plain JavaScript runtime.
//
// A worker that renders through the browser backend holds several runtime devices over its one
// browser GPU device: the renderer's own, and the snapshot capture context opened beside it on the
// same thread to read tiles back. Each one reaches the library through a bridge of its own, named
// by the logical-device handle every entry point takes first. These cases pin what one logical
// device must never do to another: refuse it, overwrite its request, or take its device away
// when it is torn down.

import assert from "node:assert/strict";
import test from "node:test";

import { drain, loadLibrary, until } from "./bridge-library-harness.mjs";

// Logical-device handles. The runtime mints them and never reuses one; the library only keys its
// state by them.
const kFirst = 1;
const kSecond = 2;

/** The protocol's encoding of a copy-destination buffer. */
const kBufferCopyDst = 32;

/** Begins a request on the logical device `handle` and waits for it to settle ready. */
async function beginReady(bridge, handle) {
  const { entryPoints, state } = bridge;
  assert.equal(
    entryPoints.donner_gpu_begin_device_request(handle),
    state.kSuccess,
    `logical device ${handle} could not begin a request`,
  );
  await until(
    () => entryPoints.donner_gpu_device_request_state(handle) !== state.kRequestPending,
    `a settled request on logical device ${handle}`,
  );
  assert.equal(
    entryPoints.donner_gpu_device_request_state(handle),
    state.kRequestReady,
    `logical device ${handle} did not become ready`,
  );
}

test("tearing down a second logical device leaves the first one's device and objects in place", async () => {
  const bridge = loadLibrary();
  const { entryPoints, state } = bridge;
  await beginReady(bridge, kFirst);
  assert.equal(entryPoints.donner_gpu_create_buffer(kFirst, 7, 64, kBufferCopyDst), state.kSuccess);

  // Whatever the second logical device's request did, its teardown is its own: the capture context
  // is released while the renderer it sits beside goes on drawing.
  entryPoints.donner_gpu_begin_device_request(kSecond);
  entryPoints.donner_gpu_release_device(kSecond);

  assert.equal(
    entryPoints.donner_gpu_owns_device(kFirst),
    1,
    "releasing the second logical device took the first one's browser device away",
  );
  const payload = bridge.reserve(4);
  assert.equal(
    entryPoints.donner_gpu_write_buffer(kFirst, 7, 0, payload, 4),
    state.kSuccess,
    "the first logical device lost the buffer it had created",
  );
});

test("a request refused on one logical device leaves another's request state and reason alone", async () => {
  const bridge = loadLibrary();
  const { entryPoints, state } = bridge;
  await beginReady(bridge, kFirst);

  // A protocol table that disagrees with the library's refuses that logical device's request.
  const disagreeing = bridge.wordArray(new Array(state.protocolCodes.length).fill(0));
  assert.equal(
    entryPoints.donner_gpu_check_protocol(kSecond, disagreeing, state.protocolCodes.length),
    state.kFailed,
  );

  assert.equal(
    entryPoints.donner_gpu_device_request_state(kFirst),
    state.kRequestReady,
    "a refusal on the second logical device overwrote the first one's settled request",
  );
  const message = bridge.reserve(256);
  assert.equal(
    entryPoints.donner_gpu_read_request_error(kFirst, message, 256),
    0,
    "the first logical device reports the second one's refusal as its own",
  );
});

test("a second logical device joins the worker's browser device instead of being refused", async () => {
  const bridge = loadLibrary();
  const { entryPoints } = bridge;
  await beginReady(bridge, kFirst);
  await beginReady(bridge, kSecond);

  assert.equal(entryPoints.donner_gpu_owns_device(kSecond), 1);
  assert.equal(
    bridge.adapterRequests,
    1,
    "each logical device asked the browser for a GPU device of its own",
  );
});

/** Waits for the request of the logical device `handle` to settle, and returns its state. */
async function settled(bridge, handle) {
  const { entryPoints, state } = bridge;
  await until(
    () => entryPoints.donner_gpu_device_request_state(handle) !== state.kRequestPending,
    `a settled request on logical device ${handle}`,
  );
  return entryPoints.donner_gpu_device_request_state(handle);
}

test("logical devices that begin before the browser answers share its one request", async () => {
  const bridge = loadLibrary();
  const { entryPoints, state } = bridge;
  assert.equal(entryPoints.donner_gpu_begin_device_request(kFirst), state.kSuccess);
  assert.equal(entryPoints.donner_gpu_begin_device_request(kSecond), state.kSuccess);

  assert.equal(await settled(bridge, kFirst), state.kRequestReady);
  assert.equal(await settled(bridge, kSecond), state.kRequestReady);
  assert.equal(bridge.adapterRequests, 1, "a logical device that joined asked for its own device");
  assert.equal(state.device, bridge.device);
});

test("a request let go before the browser answers installs nothing, and the next starts over", async () => {
  const bridge = loadLibrary();
  const { entryPoints, state } = bridge;
  assert.equal(entryPoints.donner_gpu_begin_device_request(kFirst), state.kSuccess);
  entryPoints.donner_gpu_release_device(kFirst);
  await drain();

  assert.equal(bridge.devices.length, 1, "the browser was never asked for the device");
  assert.equal(
    state.device,
    null,
    "a request that settled after its device was let go installed it",
  );

  const kThird = 3;
  await beginReady(bridge, kThird);
  assert.equal(bridge.adapterRequests, 2);
  assert.equal(state.device, bridge.devices[1], "the next request inherited the abandoned device");
  assert.equal(entryPoints.donner_gpu_device_identity(kThird), kThird);
});

test("a pending request survives the logical device that began it while another has joined", async () => {
  const bridge = loadLibrary();
  const { entryPoints, state } = bridge;
  assert.equal(entryPoints.donner_gpu_begin_device_request(kFirst), state.kSuccess);
  assert.equal(entryPoints.donner_gpu_begin_device_request(kSecond), state.kSuccess);
  entryPoints.donner_gpu_release_device(kFirst);

  assert.equal(await settled(bridge, kSecond), state.kRequestReady);
  assert.equal(entryPoints.donner_gpu_owns_device(kSecond), 1);
  assert.equal(bridge.adapterRequests, 1);
  // The browser device keeps the identity of the request that obtained it.
  assert.equal(entryPoints.donner_gpu_device_identity(kSecond), kFirst);
});

/** The protocol's encoding of rgba8unorm, and of a sampled, copyable texture. */
const kRgba8Unorm = 1;
const kTextureSampledCopySrc = 2 | 4;

/** Creates a 4x4 texture on `handle` under `id`. */
function createTexture(bridge, handle, id) {
  return bridge.entryPoints.donner_gpu_create_texture(
    handle,
    id,
    4,
    4,
    kRgba8Unorm,
    kTextureSampledCopySrc,
  );
}

/** Shares texture `id` of `handle` and returns the share identifier, failing on a refusal. */
function share(bridge, handle, id) {
  const out = bridge.outParameter();
  assert.equal(
    bridge.entryPoints.donner_gpu_share_texture(handle, id, out),
    bridge.state.kSuccess,
    `texture ${id} of logical device ${handle} could not be shared`,
  );
  return bridge.read(out);
}

/** Two ready logical devices over one browser device. */
async function twoDevices() {
  const bridge = loadLibrary();
  await beginReady(bridge, kFirst);
  await beginReady(bridge, kSecond);
  return bridge;
}

test("each logical device numbers its identifiers on its own", async () => {
  const bridge = await twoDevices();
  const { entryPoints, state } = bridge;
  assert.equal(entryPoints.donner_gpu_create_buffer(kFirst, 1, 64, kBufferCopyDst), state.kSuccess);
  assert.equal(
    entryPoints.donner_gpu_create_buffer(kSecond, 1, 64, kBufferCopyDst),
    state.kSuccess,
  );
  assert.notEqual(bridge.objects(kFirst).get(1).object, bridge.objects(kSecond).get(1).object);

  assert.equal(
    entryPoints.donner_gpu_destroy_object(kSecond, state.kBuffer, 1),
    state.kSuccess,
  );
  assert.equal(bridge.objects(kFirst).get(1).object.destroyed, false);
  assert.equal(bridge.objects(kSecond).has(1), false);
});

test("each logical device reports the completion of its own submissions", async () => {
  const bridge = await twoDevices();
  const { entryPoints, state } = bridge;
  assert.equal(entryPoints.donner_gpu_begin_command_buffer(kFirst, 1, 0), state.kSuccess);
  assert.equal(entryPoints.donner_gpu_end_command_buffer(kFirst, 1), state.kSuccess);
  assert.equal(entryPoints.donner_gpu_submit_command_buffers(kFirst, 1), state.kSuccess);
  await until(
    () => entryPoints.donner_gpu_completed_serial(kFirst) === 1,
    "the first logical device's submission reported done",
  );

  assert.equal(entryPoints.donner_gpu_completed_serial(kSecond), 0);
  // A recording is its logical device's own: the second one has nothing open to finish.
  assert.equal(entryPoints.donner_gpu_end_command_buffer(kSecond, 1), state.kFailed);
});

test("a shared texture registers on another logical device as an alias of the same texture", async () => {
  const bridge = await twoDevices();
  const { entryPoints, state } = bridge;
  assert.equal(createTexture(bridge, kFirst, 5), state.kSuccess);
  const texture = bridge.objects(kFirst).get(5).object;

  const held = share(bridge, kFirst, 5);
  assert.equal(entryPoints.donner_gpu_register_shared_texture(kSecond, 9, held), state.kSuccess);
  assert.equal(entryPoints.donner_gpu_create_texture_view(kSecond, 10, 9), state.kSuccess);
  assert.equal(bridge.objects(kSecond).get(10).object.texture, texture);
  assert.equal(texture.views, 1);
});

test("a shared texture outlives its producer's release until the share is released", async () => {
  const bridge = await twoDevices();
  const { entryPoints, state } = bridge;
  assert.equal(createTexture(bridge, kFirst, 5), state.kSuccess);
  const texture = bridge.objects(kFirst).get(5).object;
  const held = share(bridge, kFirst, 5);
  assert.equal(entryPoints.donner_gpu_register_shared_texture(kSecond, 9, held), state.kSuccess);

  assert.equal(entryPoints.donner_gpu_destroy_object(kFirst, state.kTexture, 5), state.kSuccess);
  assert.equal(texture.destroyed, false, "the producer's release destroyed a shared texture");
  assert.equal(entryPoints.donner_gpu_destroy_object(kSecond, state.kTexture, 9), state.kSuccess);
  assert.equal(texture.destroyed, false, "releasing an alias destroyed the texture it names");

  entryPoints.donner_gpu_release_texture_share(kFirst, held);
  assert.equal(texture.destroyed, true, "the texture outlived its last holder");
});

test("a share released first leaves the texture to its producer", async () => {
  const bridge = await twoDevices();
  const { entryPoints, state } = bridge;
  assert.equal(createTexture(bridge, kFirst, 5), state.kSuccess);
  const texture = bridge.objects(kFirst).get(5).object;
  entryPoints.donner_gpu_release_texture_share(kFirst, share(bridge, kFirst, 5));
  assert.equal(texture.destroyed, false);

  assert.equal(entryPoints.donner_gpu_destroy_object(kFirst, state.kTexture, 5), state.kSuccess);
  assert.equal(texture.destroyed, true);
});

test("a texture is shared once, from the logical device that allocated it", async () => {
  const bridge = await twoDevices();
  const { entryPoints, state } = bridge;
  assert.equal(createTexture(bridge, kFirst, 5), state.kSuccess);
  const held = share(bridge, kFirst, 5);
  const out = bridge.outParameter();
  assert.equal(entryPoints.donner_gpu_share_texture(kFirst, 5, out), state.kFailed);

  assert.equal(entryPoints.donner_gpu_register_shared_texture(kSecond, 9, held), state.kSuccess);
  assert.equal(entryPoints.donner_gpu_share_texture(kSecond, 9, out), state.kFailed);
  assert.equal(
    entryPoints.donner_gpu_register_shared_texture(kSecond, 10, held + 1000),
    state.kUnknownObject,
  );
});

test("a share of a canvas frame never destroys the canvas's texture", async () => {
  const bridge = await twoDevices();
  const { entryPoints, state } = bridge;
  bridge.addCanvas("#canvas");
  const name = bridge.string("#canvas");
  assert.equal(
    entryPoints.donner_gpu_create_surface(kFirst, 1, name.pointer, name.length),
    state.kSuccess,
  );
  assert.equal(
    entryPoints.donner_gpu_configure_surface(kFirst, 1, 2, 1, 8, 8, state.kAlphaModeOpaque),
    state.kSuccess,
  );
  const status = bridge.outParameter();
  assert.equal(
    entryPoints.donner_gpu_acquire_current_texture(kFirst, 1, 2, status),
    state.kSuccess,
  );
  const frame = bridge.objects(kFirst).get(2).object;
  const held = share(bridge, kFirst, 2);

  assert.equal(entryPoints.donner_gpu_abandon_current_texture(kFirst, 1), state.kSuccess);
  entryPoints.donner_gpu_release_texture_share(kFirst, held);
  assert.equal(frame.destroyed, false, "releasing the share destroyed the canvas's own texture");
});

/** Configures a canvas surface on `handle` under `surfaceId` and takes its frame as `frameId`. */
function acquireFrame(bridge, handle, surfaceId, frameId) {
  const { entryPoints, state } = bridge;
  bridge.addCanvas("#canvas");
  const name = bridge.string("#canvas");
  assert.equal(
    entryPoints.donner_gpu_create_surface(handle, surfaceId, name.pointer, name.length),
    state.kSuccess,
  );
  assert.equal(
    entryPoints.donner_gpu_configure_surface(handle, surfaceId, 2, 1, 8, 8, state.kAlphaModeOpaque),
    state.kSuccess,
  );
  const status = bridge.outParameter();
  assert.equal(
    entryPoints.donner_gpu_acquire_current_texture(handle, surfaceId, frameId, status),
    state.kSuccess,
  );
  return bridge.objects(handle).get(frameId).object;
}

test("no share outlives the last logical device, and the textures shares held go with it", async () => {
  const bridge = await twoDevices();
  const { entryPoints, state } = bridge;
  assert.equal(createTexture(bridge, kFirst, 5), state.kSuccess);
  const texture = bridge.objects(kFirst).get(5).object;
  const frame = acquireFrame(bridge, kFirst, 1, 2);
  // Neither share is released: a holder dropped on a thread that does not own the device cannot
  // reach this worker to release it.
  share(bridge, kFirst, 5);
  share(bridge, kFirst, 2);
  assert.equal(entryPoints.donner_gpu_destroy_object(kFirst, state.kTexture, 5), state.kSuccess);

  entryPoints.donner_gpu_release_device(kFirst);
  entryPoints.donner_gpu_release_device(kSecond);
  assert.equal(state.shares.size, 0, "a share outlived the browser device it belongs to");
  assert.equal(texture.destroyed, true, "a texture a share held outlived its browser device");
  assert.equal(frame.destroyed, false, "the device's teardown destroyed the canvas's own texture");
});

test("a share of an earlier browser device is refused on the next one", async () => {
  const bridge = await twoDevices();
  const { entryPoints, state } = bridge;
  assert.equal(createTexture(bridge, kFirst, 5), state.kSuccess);
  const stale = share(bridge, kFirst, 5);
  entryPoints.donner_gpu_release_device(kFirst);
  entryPoints.donner_gpu_release_device(kSecond);

  const kThird = 3;
  await beginReady(bridge, kThird);
  assert.equal(
    entryPoints.donner_gpu_register_shared_texture(kThird, 1, stale),
    state.kUnknownObject,
    "a share of the released browser device registered on its successor",
  );
  assert.equal(bridge.objects(kThird).has(1), false);
});

test("a share is released only by the logical device that made it", async () => {
  // Two workers, each with its own copy of the library. Share numbers are each worker's own, while
  // logical-device handles are unique across workers.
  const workerA = loadLibrary();
  const workerB = loadLibrary();
  await beginReady(workerA, kFirst);
  await beginReady(workerB, kSecond);
  assert.equal(createTexture(workerA, kFirst, 5), workerA.state.kSuccess);
  assert.equal(createTexture(workerB, kSecond, 5), workerB.state.kSuccess);
  const shareA = share(workerA, kFirst, 5);
  const shareB = share(workerB, kSecond, 5);
  assert.equal(shareA, shareB, "the case needs both workers' shares to carry the same number");

  // Worker A's share is dropped on a thread that reaches worker B's library instead.
  workerB.entryPoints.donner_gpu_release_texture_share(kFirst, shareA);
  assert.equal(
    workerB.state.shares.has(shareB),
    true,
    "releasing another worker's share released this worker's share of the same number",
  );
  workerB.entryPoints.donner_gpu_release_texture_share(kSecond, shareB);
  assert.equal(workerB.state.shares.has(shareB), false);
});

test("the browser device goes with the last logical device over it", async () => {
  const bridge = await twoDevices();
  const { entryPoints } = bridge;
  entryPoints.donner_gpu_release_device(kFirst);
  assert.equal(entryPoints.donner_gpu_owns_device(kSecond), 1);
  entryPoints.donner_gpu_release_device(kSecond);

  const kThird = 3;
  await beginReady(bridge, kThird);
  assert.equal(bridge.adapterRequests, 2, "a later request inherited the released device");
});

test("losing the browser device loses every logical device over it", async () => {
  const bridge = await twoDevices();
  const { entryPoints, state } = bridge;
  bridge.lose({ reason: "destroyed", message: "the tab was discarded" });
  await until(
    () => entryPoints.donner_gpu_is_device_lost(kFirst) === 1,
    "the device reported lost",
  );
  assert.equal(entryPoints.donner_gpu_is_device_lost(kSecond), 1);
  assert.equal(
    entryPoints.donner_gpu_create_buffer(kSecond, 1, 64, kBufferCopyDst),
    state.kDeviceLost,
  );
});

test("a handle this worker never opened owns nothing here", async () => {
  const bridge = loadLibrary();
  const { entryPoints, state } = bridge;
  await beginReady(bridge, kFirst);
  const kElsewhere = 99;
  assert.equal(entryPoints.donner_gpu_owns_device(kElsewhere), 0);
  assert.equal(entryPoints.donner_gpu_device_identity(kElsewhere), 0);
  assert.equal(
    entryPoints.donner_gpu_create_buffer(kElsewhere, 1, 64, kBufferCopyDst),
    state.kNotOwner,
  );
  assert.equal(entryPoints.donner_gpu_device_identity(kFirst), kFirst);
});
