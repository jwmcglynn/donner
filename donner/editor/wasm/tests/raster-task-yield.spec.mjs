import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";
import vm from "node:vm";
import { MessageChannel } from "node:worker_threads";

const source = readFileSync(new URL("../../AsyncRenderer.cc", import.meta.url), "utf8");

// Execute the embedded production bodies rather than maintaining a second yield implementation.
function loadYield(Channel) {
  const scope = {};
  const context = vm.createContext({
    globalThis: scope,
    MessageChannel: Channel,
    abort(message) {
      throw new Error(message);
    },
  });
  const load = (name, asynchronous = false) => {
    const pattern = new RegExp(
      `EM_(?:ASYNC_)?JS\\(void, ${name}, \\(\\), \\{\\n([\\s\\S]*?)\\n\\}\\);`,
    );
    const match = source.match(pattern);
    assert.ok(match, `embedded function ${name}`);
    return vm.runInContext(`(${asynchronous ? "async " : ""}function() {${match[1]}\n})`, context);
  };
  return {
    scope,
    initialize: load("InitializeRasterTaskYield"),
    yieldTask: load("YieldRasterTask", true),
    close: load("CloseRasterTaskYield"),
  };
}

function controlledChannel() {
  const channels = [];
  const messages = [];
  class Channel {
    constructor() {
      this.port1 = {
        onmessage: null,
        closed: false,
        close() {
          this.closed = true;
        },
      };
      this.port2 = {
        closed: false,
        close() {
          this.closed = true;
        },
        postMessage: () => {
          assert.equal(this.port2.closed, false);
          const handler = this.port1.onmessage;
          messages.push(() => handler({ data: 0 }));
        },
      };
      channels.push(this);
    }
  }
  return {
    Channel,
    channels,
    messages,
    flush() {
      assert.ok(messages.length);
      messages.shift()();
    },
  };
}

test("raster task yield reuses one channel and permits only one pending continuation", async () => {
  const channel = controlledChannel();
  const runtime = loadYield(channel.Channel);
  runtime.initialize();
  assert.throws(() => runtime.initialize(), /already initialized/);
  const first = runtime.yieldTask();
  assert.equal(channel.messages.length, 1);
  await assert.rejects(runtime.yieldTask(), /overlapping/);
  assert.throws(() => runtime.close(), /pending/);
  assert.equal(channel.channels[0].port1.closed, false);
  channel.flush();
  await first;
  const second = runtime.yieldTask();
  assert.equal(channel.channels.length, 1);
  channel.flush();
  await second;
  runtime.close();
  assert.equal(channel.channels[0].port1.closed, true);
  assert.equal(channel.channels[0].port2.closed, true);
  assert.equal(channel.channels[0].port1.onmessage, null);
  assert.equal(runtime.scope.__donnerRasterTaskYield, undefined);
  assert.equal(runtime.scope.__donnerRasterWorker, undefined);
});

test("a stale callback cannot resume a new pooled-worker lifetime", async () => {
  const channel = controlledChannel();
  const runtime = loadYield(channel.Channel);
  runtime.initialize();
  const stale = channel.channels[0].port1.onmessage;
  runtime.close();
  runtime.initialize();
  let resumed = false;
  const pending = runtime.yieldTask().then(() => {
    resumed = true;
  });
  stale({ data: 0 });
  await Promise.resolve();
  assert.equal(resumed, false);
  assert.equal(channel.messages.length, 1);
  channel.flush();
  await pending;
  assert.equal(resumed, true);
  runtime.close();
});

test("repeated worker lifetimes retire both ports and pending state", async () => {
  const channel = controlledChannel();
  const runtime = loadYield(channel.Channel);
  for (let lifetime = 0; lifetime < 16; ++lifetime) {
    runtime.initialize();
    const pending = runtime.yieldTask();
    channel.flush();
    await pending;
    runtime.close();
    assert.equal(runtime.scope.__donnerRasterTaskYield, undefined);
  }
  assert.equal(channel.messages.length, 0);
  assert.equal(channel.channels.length, 16);
  for (const current of channel.channels) {
    assert.equal(current.port1.closed, true);
    assert.equal(current.port2.closed, true);
  }
});

test("the real channel yields a task turn rather than only microtasks", async () => {
  const runtime = loadYield(MessageChannel);
  runtime.initialize();
  let resumed = false;
  const pending = runtime.yieldTask().then(() => {
    resumed = true;
  });
  try {
    for (let turn = 0; turn < 16; ++turn) await Promise.resolve();
    assert.equal(resumed, false);
    await pending;
    assert.equal(resumed, true);
  } finally {
    await pending;
    runtime.close();
  }
});
