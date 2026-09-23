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

import { loadLibrary, until } from "./bridge-library-harness.mjs";

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
