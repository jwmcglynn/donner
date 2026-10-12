import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { test } from "node:test";
import { setImmediate } from "node:timers/promises";
import { startApplicationHeartbeat } from "./browser-application-heartbeat.mjs";

function deferred() {
  let resolve;
  let reject;
  const promise = new Promise((yes, no) => {
    resolve = yes;
    reject = no;
  });
  return { promise, resolve, reject };
}

class ControlledWorker {
  constructor(replies) {
    this.replies = replies;
    this.discovered = false;
    this.calls = 0;
    this.pending = 0;
    this.maximumPending = 0;
  }

  evaluate() {
    if (!this.discovered) {
      this.discovered = true;
      return Promise.resolve(true);
    }
    const index = this.calls++;
    this.pending++;
    this.maximumPending = Math.max(this.maximumPending, this.pending);
    return Promise.resolve(this.replies[index] ?? { objects: index + 1 }).finally(() => {
      this.pending--;
    });
  }
}

function fixture(context, replies) {
  context.mock.timers.enable({ apis: ["setTimeout", "setInterval", "Date"], now: 5000 });
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), "application-heartbeat-test-"));
  const heartbeat = path.join(directory, "heartbeat.json");
  const worker = new ControlledWorker(replies);
  const starting = startApplicationHeartbeat({ workers: () => [worker] }, heartbeat);
  let stop;
  context.after(async () => {
    stop?.();
    for (const reply of replies) reply?.resolve?.({ objects: 0 });
    await setImmediate();
    fs.rmSync(directory, { recursive: true, force: true });
  });
  return {
    worker,
    heartbeat,
    read: () => JSON.parse(fs.readFileSync(heartbeat, "utf8")),
    async start() {
      stop = await starting;
      return stop;
    },
  };
}

async function advance(context, milliseconds) {
  context.mock.timers.tick(milliseconds);
  await setImmediate();
}

async function holdAcrossPulses(context) {
  for (let pulse = 0; pulse < 3; pulse++) {
    await advance(context, 1000);
    await advance(context, 500);
  }
}

test("application IPC stays pending across deadlines and late completion resumes freshness", async (context) => {
  const held = deferred();
  const state = fixture(context, [held.promise]);
  await setImmediate();
  assert.equal(state.worker.calls, 1);
  await advance(context, 500);
  await state.start();
  assert.equal(fs.existsSync(state.heartbeat), false);
  await holdAcrossPulses(context);
  assert.equal(state.worker.calls, 1);
  assert.equal(state.worker.pending, 1);
  assert.equal(state.worker.maximumPending, 1);
  assert.equal(fs.existsSync(state.heartbeat), false);

  held.resolve({ objects: 77 });
  await setImmediate();
  assert.equal(state.worker.pending, 0);
  await advance(context, 1000);
  assert.equal(state.read().phase, "active");
  assert.equal(state.read().gpu.objects, 2);
  assert.equal(state.worker.maximumPending, 1);
});

test("a held application diagnostic reply blocks overlapping application heartbeats", async (context) => {
  const held = deferred();
  const state = fixture(context, [{ objects: 1 }, held.promise]);
  await setImmediate();
  assert.equal(state.worker.calls, 2);
  await advance(context, 500);
  await state.start();
  const previous = state.read();
  await holdAcrossPulses(context);
  assert.equal(state.worker.calls, 2);
  assert.equal(state.worker.pending, 1);
  assert.equal(state.worker.maximumPending, 1);
  assert.deepEqual(state.read(), previous);

  held.resolve({ objects: 77 });
  await setImmediate();
  await advance(context, 1000);
  assert.equal(state.read().phase, "active");
  assert.ok(state.read().atMs > previous.atMs);
  assert.equal(state.worker.maximumPending, 1);
});

test("rejected application IPC releases the guard for the next acknowledged pulse", async (context) => {
  const failed = deferred();
  const state = fixture(context, [failed.promise]);
  await setImmediate();
  failed.reject(new Error("injected IPC rejection"));
  await state.start();
  assert.equal(state.worker.pending, 0);
  assert.equal(fs.existsSync(state.heartbeat), false);
  await advance(context, 1000);
  assert.equal(state.read().phase, "active");
  assert.equal(state.read().gpu.objects, 2);
  assert.equal(state.worker.maximumPending, 1);
});

test("stop preserves teardown while outstanding IPC completes without cancellation", async (context) => {
  const held = deferred();
  const state = fixture(context, [held.promise]);
  await setImmediate();
  await advance(context, 500);
  const stop = await state.start();
  stop();
  const teardown = state.read();
  assert.equal(teardown.phase, "teardown");
  assert.equal(fs.existsSync(`${state.heartbeat}.app-active`), false);
  assert.equal(state.worker.pending, 1);
  held.resolve({ objects: 77 });
  await setImmediate();
  await advance(context, 5000);
  assert.equal(state.worker.pending, 0);
  assert.equal(state.worker.calls, 1);
  assert.deepEqual(state.read(), teardown);
});
