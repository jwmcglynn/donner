import assert from "node:assert/strict";
import fs from "node:fs";
import { EventEmitter } from "node:events";
import os from "node:os";
import path from "node:path";
import { test } from "node:test";
import { setImmediate } from "node:timers/promises";
import { createRequire } from "node:module";
import {
  endApplicationTeardown,
  startApplicationHeartbeat,
} from "./browser-application-heartbeat.mjs";

const { applicationActiveMarker, refreshIdleHeartbeat, writeHeartbeat } =
  createRequire(import.meta.url)("./browser-heartbeat.cjs");

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
    const reply = this.replies[index] ?? { objects: index + 1 };
    return Promise.resolve(reply.promise ?? reply).finally(() => {
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
  const state = fixture(context, [held]);
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
  const state = fixture(context, [{ objects: 1 }, held]);
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
  const state = fixture(context, [failed]);
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
  const state = fixture(context, [held]);
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

class IdentityWorker extends EventEmitter {
  constructor(identity) {
    super();
    this.identity = identity;
    this.calls = 0;
    this.pending = 0;
    this.maximumPending = 0;
  }

  evaluate() {
    const reply = this.calls++ === 0 ? this.identity : { objects: 1 };
    this.pending++;
    this.maximumPending = Math.max(this.maximumPending, this.pending);
    return Promise.resolve(reply?.promise ?? reply).finally(() => this.pending--);
  }
}

function discoveryFixture(context, workers) {
  context.mock.timers.enable({ apis: ["setTimeout", "setInterval", "Date"], now: 5000 });
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), "worker-discovery-test-"));
  const heartbeat = path.join(directory, "heartbeat.json");
  const page = new EventEmitter();
  page.workers = () => workers;
  page.evaluate = () => Promise.resolve({});
  let stop;
  let failure;
  const starting = startApplicationHeartbeat(page, heartbeat).then(
    (value) => { stop = value; },
    (error) => { failure = error; },
  );
  context.after(async () => {
    stop?.();
    await setImmediate();
    fs.rmSync(directory, { recursive: true, force: true });
  });
  return { page, heartbeat, starting, stop: () => stop, failure: () => failure };
}

test("slow identity is unknown, not a negative application response", async (context) => {
  const pending = deferred();
  const owner = new IdentityWorker(pending);
  const other = new IdentityWorker(false);
  const state = discoveryFixture(context, [other, owner]);
  await setImmediate();
  await advance(context, 1001);
  assert.equal(state.failure(), undefined);
  assert.equal(state.stop(), undefined);
  assert.equal(fs.existsSync(state.heartbeat), false);
  assert.equal(owner.calls, 1);
  state.page.emit("worker", owner);
  assert.equal(owner.calls, 1);
  pending.resolve(true);
  await state.starting;
  assert.equal(state.failure(), undefined);
  assert.equal(typeof state.stop(), "function");
  assert.equal(owner.maximumPending, 1);
  assert.equal(state.page.listenerCount("worker"), 0);
});

test("unanswered discovery expires at the liveness ceiling without a late heartbeat", async (context) => {
  const pending = deferred();
  const worker = new IdentityWorker(pending);
  const state = discoveryFixture(context, [worker]);
  await setImmediate();
  await advance(context, 5000);
  await state.starting;
  assert.match(state.failure().message, /discovery deadline/);
  assert.match(state.failure().message, /"unresolved":1/);
  assert.equal(worker.calls, 1);
  assert.equal(state.page.listenerCount("worker"), 0);
  assert.equal(worker.listenerCount("close"), 0);
  pending.resolve(true);
  await setImmediate();
  await advance(context, 5000);
  assert.equal(state.stop(), undefined);
  assert.equal(fs.existsSync(state.heartbeat), false);
  assert.equal(fs.existsSync(`${state.heartbeat}.app-active`), false);
});

test("explicit negative replies are distinguished from unanswered discovery", async (context) => {
  const state = discoveryFixture(context, [new IdentityWorker(false), new IdentityWorker(false)]);
  await setImmediate();
  await advance(context, 5000);
  await state.starting;
  assert.match(state.failure().message, /"false":2/);
  assert.match(state.failure().message, /"unresolved":0/);
  assert.equal(fs.existsSync(state.heartbeat), false);
});

test("destroyed workers cannot identify themselves through a late reply", async (context) => {
  const pending = deferred();
  const worker = new IdentityWorker(pending);
  const state = discoveryFixture(context, [worker]);
  await setImmediate();
  worker.emit("close");
  pending.resolve(true);
  await setImmediate();
  assert.equal(state.stop(), undefined);
  await advance(context, 5000);
  await state.starting;
  assert.match(state.failure().message, /"failed":1/);
  assert.match(state.failure().message, /"unresolved":0/);
  assert.equal(worker.maximumPending, 1);
});

test("page closure ends discovery and ignores later worker replies", async (context) => {
  const pending = deferred();
  const worker = new IdentityWorker(pending);
  const state = discoveryFixture(context, [worker]);
  await setImmediate();
  state.page.emit("close");
  await state.starting;
  assert.match(state.failure().message, /page closed during/);
  pending.resolve(true);
  await setImmediate();
  assert.equal(state.stop(), undefined);
  assert.equal(state.page.listenerCount("worker"), 0);
  assert.equal(state.page.listenerCount("close"), 0);
  assert.equal(fs.existsSync(`${state.heartbeat}.app-active`), false);
});

test("a newly observed application worker is identified only once", async (context) => {
  const workers = [new IdentityWorker(false)];
  const state = discoveryFixture(context, workers);
  await setImmediate();
  await advance(context, 1000);
  const owner = new IdentityWorker(true);
  workers.push(owner);
  state.page.emit("worker", owner);
  state.page.emit("worker", owner);
  await state.starting;
  assert.equal(state.failure(), undefined);
  assert.equal(typeof state.stop(), "function");
  assert.equal(owner.maximumPending, 1);
  assert.equal(state.page.listenerCount("worker"), 0);
});

test("new workers do not extend the absolute discovery deadline", async (context) => {
  const workers = [];
  const state = discoveryFixture(context, workers);
  await setImmediate();
  await advance(context, 4900);
  const pending = deferred();
  const worker = new IdentityWorker(pending);
  workers.push(worker);
  state.page.emit("worker", worker);
  await setImmediate();
  await advance(context, 100);
  await state.starting;
  assert.match(state.failure().message, /discovery deadline/);
  assert.equal(worker.calls, 1);
  pending.resolve(true);
  await setImmediate();
  assert.equal(state.stop(), undefined);
});

test("losing identification IPC cannot overlap later worker diagnostics", async (context) => {
  const pending = deferred();
  const other = new IdentityWorker(pending);
  const owner = new IdentityWorker(true);
  const state = discoveryFixture(context, [other, owner]);
  await state.starting;
  await holdAcrossPulses(context);
  assert.equal(other.calls, 1);
  assert.equal(other.maximumPending, 1);
  pending.resolve(false);
  await setImmediate();
  await advance(context, 1000);
  assert.ok(other.calls > 1);
  assert.equal(other.maximumPending, 1);
});


test("late identity cannot beat an expired deadline when timer delivery is delayed", async (context) => {
  const pending = deferred();
  const state = discoveryFixture(context, [new IdentityWorker(pending)]);
  await setImmediate();
  context.mock.timers.setTime(11000);
  pending.resolve(true);
  await state.starting;
  assert.match(state.failure().message, /discovery deadline/);
  assert.equal(state.stop(), undefined);
  assert.equal(fs.existsSync(state.heartbeat), false);
});

test("the next case ends a finished session's teardown with a new launch session", () => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), "application-heartbeat-test-"));
  try {
    const heartbeat = path.join(directory, "heartbeat.json");
    const read = () => JSON.parse(fs.readFileSync(heartbeat, "utf8"));
    endApplicationTeardown(heartbeat);
    assert.equal(fs.existsSync(heartbeat), false);
    writeHeartbeat(heartbeat, { phase: "teardown", sessionId: "first", atMs: 1 });
    endApplicationTeardown(heartbeat);
    const launch = read();
    assert.equal(launch.phase, "launch");
    assert.equal(typeof launch.sessionId, "string");
    assert.notEqual(launch.sessionId, "first");
    endApplicationTeardown(heartbeat);
    assert.deepEqual(read(), launch, "only a teardown record is ended");
  } finally {
    fs.rmSync(directory, { recursive: true, force: true });
  }
});

test("idle refreshes keep liveness without rewriting the record", () => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), "application-heartbeat-test-"));
  try {
    const heartbeat = path.join(directory, "heartbeat.json");
    refreshIdleHeartbeat(heartbeat);
    assert.equal(JSON.parse(fs.readFileSync(heartbeat, "utf8")).phase, "launch");
    const record = JSON.stringify({ phase: "launch", sessionId: "second", atMs: 1 });
    fs.writeFileSync(heartbeat, record);
    const old = new Date(Date.now() - 60000);
    fs.utimesSync(heartbeat, old, old);
    refreshIdleHeartbeat(heartbeat);
    assert.equal(fs.readFileSync(heartbeat, "utf8"), record);
    assert.ok(Date.now() - fs.statSync(heartbeat).mtimeMs < 30000, "the refresh renews liveness");
    fs.writeFileSync(applicationActiveMarker(heartbeat), "active");
    fs.utimesSync(heartbeat, old, old);
    refreshIdleHeartbeat(heartbeat);
    assert.ok(
      Date.now() - fs.statSync(heartbeat).mtimeMs >= 30000,
      "an active session owns its own liveness",
    );
  } finally {
    fs.rmSync(directory, { recursive: true, force: true });
  }
});
