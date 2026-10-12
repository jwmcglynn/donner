import assert from "node:assert/strict";
import { EventEmitter } from "node:events";
import { test } from "node:test";
import { setImmediate } from "node:timers/promises";
import { createSurfaceProbeRegistry } from "./surface-probe-requests.mjs";

const installProbe = () => true;
const readOwner = () => true;
const readProbe = () => ({ frames: 1 });
function deferred() {
  let resolve;
  let reject;
  const promise = new Promise((yes, no) => {
    resolve = yes;
    reject = no;
  });
  return { promise, resolve, reject };
}
class Worker extends EventEmitter {
  constructor({ installed = true, owner = true, replies = new Map() } = {}) {
    super();
    this.installed = installed;
    this.owner = owner;
    this.replies = replies;
    this.calls = [];
    this.pending = 0;
    this.maximumPending = 0;
    this.closed = false;
  }
  url() {
    return "https://example.invalid/shared-worker.js";
  }
  evaluate(fn) {
    this.calls.push(fn);
    if (this.closed) return Promise.reject(new Error("worker closed"));
    this.pending++;
    this.maximumPending = Math.max(this.maximumPending, this.pending);
    const reply = this.replies.get(fn)?.shift() ?? (fn === installProbe ? this.installed : this.owner);
    return Promise.resolve(reply.promise ?? reply).finally(() => this.pending--);
  }
  close() {
    this.closed = true;
    this.emit("close");
  }
}
function fixture(context, workers, controls = []) {
  context.mock.timers.enable({ apis: ["setTimeout", "Date"], now: 5000 });
  context.after(async () => {
    for (const worker of workers) worker.close();
    for (const control of controls) control.resolve(false);
    await setImmediate();
  });
  return {
    registry: createSurfaceProbeRegistry(1000),
    page: { current: workers, workers() { return [...this.current]; } },
  };
}
async function advance(context, milliseconds) {
  context.mock.timers.tick(milliseconds);
  await setImmediate();
}

test("an installation answer after the first bound remains visible under the setup budget", async (context) => {
  const reply = deferred();
  const worker = new Worker({ replies: new Map([[installProbe, [reply]]]) });
  const { registry, page } = fixture(context, [worker], [reply]);
  const deadlineAt = Date.now() + 5000;
  const first = registry.install(page, installProbe);
  await advance(context, 1000);
  assert.equal(await first, 0);
  assert.equal(worker.pending, 1);
  await advance(context, 500);
  reply.resolve(true);
  await setImmediate();
  assert.ok(Date.now() < deadlineAt);
  assert.deepEqual(registry.workers(page), [worker]);
  assert.equal(await registry.install(page, installProbe), 1);
  assert.equal(worker.calls.length, 1);
});

test("pending installation and observations never enqueue a second IPC", async (context) => {
  const reply = deferred();
  const worker = new Worker({ replies: new Map([[installProbe, [reply, reply]]]) });
  const { registry, page } = fixture(context, [worker], [reply]);
  const first = registry.install(page, installProbe);
  const repeated = registry.install(page, installProbe);
  const observation = registry.evaluate([worker], readProbe);
  await advance(context, 1000);
  await Promise.all([first, repeated, observation]);
  assert.equal(worker.calls.length, 1);
  assert.equal(worker.maximumPending, 1);
  assert.equal(worker.pending, 1);
});

test("late owner discovery is consumed once without issuing another request", async (context) => {
  const late = deferred();
  const next = deferred();
  const worker = new Worker({ replies: new Map([[readOwner, [late, next]]]) });
  const { registry, page } = fixture(context, [worker], [late, next]);
  assert.equal(await registry.install(page, installProbe), 1);
  const first = registry.findOwner(page, readOwner);
  await advance(context, 1000);
  assert.equal(await first, null);
  late.resolve(true);
  await setImmediate();
  const second = registry.findOwner(page, readOwner);
  await advance(context, 1000);
  assert.equal(await second, worker);
  assert.equal(worker.calls.length, 2);
  assert.equal(worker.maximumPending, 1);
});

test("closed installed workers are removed and late replies cannot resurrect them", async (context) => {
  const worker = new Worker();
  const { registry, page } = fixture(context, [worker]);
  assert.equal(await registry.install(page, installProbe), 1);
  worker.close();
  page.current = [];
  assert.deepEqual(registry.workers(page), []);
  assert.equal(await registry.findOwner(page, readOwner), null);
  assert.equal(worker.calls.length, 1);
});

test("replacement workers with the same URL retain distinct identities", async (context) => {
  const late = deferred();
  const original = new Worker({ replies: new Map([[installProbe, [late]]]) });
  const replacement = new Worker();
  assert.equal(original.url(), replacement.url());
  const { registry, page } = fixture(context, [original, replacement], [late]);
  page.current = [original];
  const first = registry.install(page, installProbe);
  await advance(context, 1000);
  assert.equal(await first, 0);
  original.close();
  page.current = [replacement];
  assert.equal(await registry.install(page, installProbe), 1);
  late.resolve(true);
  await setImmediate();
  assert.deepEqual(registry.workers(page), [replacement]);
  assert.equal(await registry.findOwner(page, readOwner), replacement);
});

test("nonowner replies and multiple acknowledged owners fail closed", async (context) => {
  const owner = new Worker();
  const other = new Worker({ owner: false });
  const { registry, page } = fixture(context, [owner, other]);
  assert.equal(await registry.install(page, installProbe), 2);
  assert.equal(await registry.findOwner(page, readOwner), owner);
  other.owner = true;
  assert.equal(await registry.findOwner(page, readOwner), null);
  owner.owner = false;
  other.owner = false;
  assert.equal(await registry.findOwner(page, readOwner), null);
});
