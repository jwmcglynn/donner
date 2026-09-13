import assert from "node:assert/strict";
import { createHash, webcrypto } from "node:crypto";
import { readFile } from "node:fs/promises";
import test from "node:test";
import vm from "node:vm";

const source = await readFile(new URL("../catalog-fonts.js", import.meta.url), "utf8");
const context = vm.createContext({
  URL,
  Uint8Array,
  DataView,
  Response,
  AbortController,
  console,
  setTimeout,
  clearTimeout,
  crypto: webcrypto,
});
vm.runInContext(source, context, { filename: "catalog-fonts.js" });
const Broker = context.DonnerCatalogFontBroker;

function fixture(seed = 1) {
  const bytes = new Uint8Array(64).fill(seed);
  bytes.set([0x77, 0x4f, 0x46, 0x32]);
  new DataView(bytes.buffer).setUint32(16, 100);
  const id = createHash("sha256").update(bytes).digest("hex");
  return { bytes, asset: { id, path: `fonts/${id}.woff2`, encodedBytes: 64, decodedBytes: 100 } };
}

function deferred() {
  let resolve;
  let reject;
  const promise = new Promise((yes, no) => {
    resolve = yes;
    reject = no;
  });
  return { promise, resolve, reject };
}

function setup(t, overrides = {}) {
  const { bytes, asset } = fixture();
  const calls = [];
  const results = [];
  const done = deferred();
  const broker = new Broker({
    baseUrl: "https://editor.example/releases/a/",
    assets: [asset],
    storage: undefined,
    fetchImpl: async (url, options) => {
      calls.push({ url, options });
      return new Response(bytes, { headers: { "content-type": "font/woff2" } });
    },
    onResult: (result) => {
      results.push(result);
      done.resolve(result);
    },
    ...overrides,
  });
  t.after(() => broker.close());
  return { broker, bytes, asset, calls, results, done: done.promise };
}

test("metadata construction makes no requests and resolves assets under the package subpath", async (t) => {
  const { broker, asset, bytes, calls, done } = setup(t);
  assert.equal(calls.length, 0);
  assert.equal(broker.request(asset.id, 1), true);
  const result = await done;
  assert.deepEqual(result.bytes, bytes);
  assert.equal(calls[0].url, `https://editor.example/releases/a/fonts/${asset.id}.woff2`);
  assert.equal(calls[0].options.redirect, "error");
  assert.equal(calls[0].options.credentials, "same-origin");
  assert.equal(calls[0].options.referrerPolicy, "no-referrer");
});

test("only compiled identities and canonical content paths can be requested", (t) => {
  const { broker, asset } = setup(t);
  assert.equal(broker.request("document-chosen-font", 1), false);
  assert.equal(broker.request(asset.id, 0), false);
  assert.equal(broker.request(asset.id, Infinity), false);
  for (const path of ["../font.woff2", "https://other.example/font.woff2", "fonts/x.woff2"]) {
    assert.throws(() => setup(t, { assets: [{ ...asset, path }] }), /font-manifest/);
  }
  assert.throws(() => setup(t, { baseUrl: "blob:https://editor.example/abc" }), /font-base/);
});

test("startup demand waits for the first presented frame", async (t) => {
  const { broker, asset, done, calls } = setup(t, { enabled: false });
  broker.request(asset.id, 1);
  await new Promise(setImmediate);
  assert.equal(calls.length, 0);
  broker.start();
  assert.equal((await done).bytes.byteLength, asset.encodedBytes);
  assert.equal(calls.length, 1);
});

test("duplicate requests share one transfer and reject stale tokens", async (t) => {
  const { broker, asset, calls, done } = setup(t);
  assert.equal(broker.request(asset.id, 7), true);
  assert.equal(broker.request(asset.id, 7), true);
  assert.equal(broker.request(asset.id, 8), false);
  assert.equal((await done).token, 7);
  assert.equal(calls.length, 1);
});

test("a new store token can refill evicted Ready bytes without changing content identity", async (t) => {
  const completed = [deferred(), deferred()];
  const results = [];
  const { broker, asset, calls } = setup(t, {
    onResult: (result) => {
      results.push(result);
      completed[results.length - 1].resolve(result);
    },
  });
  broker.request(asset.id, 1);
  await completed[0].promise;
  assert.equal(broker.request(asset.id, 2), true);
  const refilled = await completed[1].promise;
  assert.equal(refilled.id, asset.id);
  assert.equal(refilled.token, 2);
  assert.equal(calls.length, 2);
});

for (const corruption of ["hash", "length", "signature", "decoded-size"]) {
  test(`rejects ${corruption} without retrying or publishing bytes`, async (t) => {
    const { bytes } = fixture();
    if (corruption === "hash") bytes[63] ^= 1;
    if (corruption === "signature") bytes[0] = 0;
    if (corruption === "decoded-size") new DataView(bytes.buffer).setUint32(16, 200);
    let calls = 0;
    const { broker, asset, done } = setup(t, {
      fetchImpl: async () => {
        ++calls;
        return new Response(corruption === "length" ? bytes.subarray(0, 63) : bytes, {
          headers: { "content-type": "font/woff2" },
        });
      },
    });
    broker.request(asset.id, 1);
    const result = await done;
    assert.match(result.error, /^font-(integrity|content)$/);
    assert.equal(result.bytes, undefined);
    assert.equal(calls, 1);
    broker.request(asset.id, 1);
    assert.equal(calls, 1);
  });
}

test("stream overflow cancels before accepting the body despite a false Content-Length", async (t) => {
  let cancelled = false;
  const body = new ReadableStream({
    start(controller) {
      controller.enqueue(new Uint8Array(65));
    },
    cancel() {
      cancelled = true;
    },
  });
  const { broker, asset, done } = setup(t, {
    fetchImpl: async () =>
      new Response(body, {
        headers: { "content-type": "font/woff2", "content-length": "1" },
      }),
  });
  broker.request(asset.id, 1);
  assert.equal((await done).error, "font-size");
  assert.equal(cancelled, true);
});

test("redirects, bad MIME, and oversized declarations are terminal", async (t) => {
  for (const kind of ["redirect", "mime", "declared-size"]) {
    const { bytes } = fixture();
    let calls = 0;
    const { broker, asset, done } = setup(t, {
      fetchImpl: async () => {
        ++calls;
        const response = new Response(bytes, {
          headers: {
            "content-type": kind === "mime" ? "text/html" : "font/woff2",
            "content-length": kind === "declared-size" ? "10000000" : "64",
          },
        });
        if (kind === "redirect") Object.defineProperty(response, "redirected", { value: true });
        return response;
      },
    });
    broker.request(asset.id, 1);
    assert.equal((await done).bytes, undefined, kind);
    assert.equal(calls, 1, kind);
  }
});

test("at most two transfers run, and queued document demand outranks previews", async (t) => {
  const fonts = [1, 2, 3, 4].map(fixture);
  const replies = new Map();
  const started = [];
  const completed = deferred();
  const { broker } = setup(t, {
    assets: fonts.map((font) => font.asset),
    fetchImpl: (url, options) => {
      started.push(url);
      const reply = deferred();
      options.signal.addEventListener("abort", () => reply.reject(new Error("cancelled")), {
        once: true,
      });
      replies.set(url, reply);
      return reply.promise;
    },
    onResult: (result) => completed.resolve(result),
  });
  fonts.forEach((font, index) => broker.request(font.asset.id, index + 1, index === 3 ? 0 : 1));
  await new Promise(setImmediate);
  assert.equal(started.length, 2);
  replies.get(started[0]).resolve(
    new Response(fonts[0].bytes, { headers: { "content-type": "font/woff2" } }),
  );
  await completed.promise;
  await new Promise(setImmediate);
  assert.equal(started.length, 3);
  assert.match(started[2], new RegExp(fonts[3].asset.id));
});

test("cancellation and shutdown discard late successful callbacks", async (t) => {
  const reply = deferred();
  const { broker, asset, bytes, results } = setup(t, { fetchImpl: () => reply.promise });
  broker.request(asset.id, 1);
  await new Promise(setImmediate);
  broker.cancel(asset.id, 1);
  reply.resolve(new Response(bytes, { headers: { "content-type": "font/woff2" } }));
  await new Promise(setImmediate);
  broker.close();
  await new Promise(setImmediate);
  assert.equal(results.length, 0);
  assert.equal(broker.request(asset.id, 2), false);
});

test("one transport retry uses backoff, and failures require an explicit cooled-down retry", async (t) => {
  const delayed = [];
  let now = 100000;
  let calls = 0;
  const timers = {
    setTimeout(callback, ms) {
      const timer = { callback, ms };
      delayed.push(timer);
      return timer;
    },
    clearTimeout(timer) {
      timer.cancelled = true;
    },
  };
  const { broker, asset, done } = setup(t, {
    now: () => now,
    timers,
    fetchImpl: async () => {
      ++calls;
      throw new TypeError("disconnected");
    },
  });
  broker.request(asset.id, 1);
  await new Promise(setImmediate);
  assert.equal(calls, 1);
  assert.deepEqual(delayed.filter((timer) => !timer.cancelled).map((timer) => timer.ms), [1000]);
  delayed.find((timer) => timer.ms === 1000).callback();
  assert.equal((await done).error, "font-network");
  assert.equal(calls, 2);
  assert.equal(broker.retry(asset.id, 2), false);
  now += 30000;
  assert.equal(broker.retry(asset.id, 2), true);
});

test("warm offline cache is reverified; tampering is evicted before network recovery", async (t) => {
  for (const tampered of [false, true]) {
    const { bytes, asset } = fixture();
    const cached = bytes.slice();
    if (tampered) cached[63] ^= 1;
    const deleted = [];
    let fetches = 0;
    const { broker, done } = setup(t, {
      storage: {
        open: async () => ({
          match: async () => new Response(cached),
          delete: async (key) => deleted.push(key),
        }),
      },
      fetchImpl: async () => {
        ++fetches;
        return new Response(bytes, { headers: { "content-type": "font/woff2" } });
      },
    });
    broker.request(asset.id, 1);
    assert.deepEqual((await done).bytes, bytes);
    assert.equal(fetches, tampered ? 1 : 0);
    assert.equal(deleted.length, tampered ? 1 : 0);
  }
});

test("storage denial leaves the verified network path usable", async (t) => {
  const { broker, asset, done } = setup(t, {
    storage: {
      open: async () => {
        throw new Error("storage denied");
      },
    },
  });
  broker.request(asset.id, 1);
  assert.equal((await done).bytes.byteLength, asset.encodedBytes);
});

test("a stalled cache cannot stall a network load", async (t) => {
  const delayed = [];
  const timers = {
    setTimeout(callback, ms) {
      const timer = { callback, ms };
      delayed.push(timer);
      return timer;
    },
    clearTimeout(timer) {
      timer.cancelled = true;
    },
  };
  const { broker, asset, done, calls } = setup(t, {
    timers,
    storage: { open: () => new Promise(() => {}) },
  });
  broker.request(asset.id, 1);
  assert.equal(calls.length, 0);
  delayed.find((timer) => timer.ms === 250).callback();
  assert.equal((await done).bytes.byteLength, asset.encodedBytes);
  assert.equal(calls.length, 1);
});

test("persistent retention is shared across versions and evicts least-recently used bodies", async (t) => {
  const stored = new Map();
  const prefix = "https://editor.example/.donner-font-cache/v1/";
  for (let index = 1; index <= 3; ++index) {
    stored.set(
      prefix + String(index).repeat(64),
      new Response(new Uint8Array(2 * 1024 * 1024), {
        headers: { "content-length": String(2 * 1024 * 1024), "x-donner-used": String(index) },
      }),
    );
  }
  const lockNames = [];
  const cache = {
    match: async (key) => stored.get(typeof key === "string" ? key : key.url)?.clone(),
    put: async (key, response) => {
      stored.set(key, response.clone());
    },
    keys: async () => Array.from(stored.keys(), (url) => ({ url })),
    delete: async (key) => stored.delete(typeof key === "string" ? key : key.url),
  };
  const { broker, asset, done } = setup(t, {
    storage: {
      open: async (name) => {
        assert.equal(name, "donner-catalog-fonts-v1");
        return cache;
      },
    },
    lockManager: {
      request: async (name, options, callback) => {
        lockNames.push(name);
        assert.equal(options.ifAvailable, true);
        return callback({ name });
      },
    },
  });
  broker.request(asset.id, 1);
  await done;
  await broker.cacheWork;
  assert.deepEqual(lockNames, ["donner-catalog-fonts-v1"]);
  assert.equal(stored.has(prefix + "1".repeat(64)), false);
  assert.equal(stored.has(prefix + "2".repeat(64)), false);
  assert.equal(stored.has(prefix + "3".repeat(64)), true);
  assert.equal(stored.has(prefix + asset.id), true);
  const retained = Array.from(stored.values()).reduce(
    (sum, response) => sum + Number(response.headers.get("content-length")),
    0,
  );
  assert.equal(retained, 2 * 1024 * 1024 + asset.encodedBytes);
});

test("cache writes require cross-tab locking and never gate font delivery", async (t) => {
  let writes = 0;
  const { broker, asset, done } = setup(t, {
    storage: {
      open: async () => ({
        match: async () => undefined,
        put: async () => {
          ++writes;
        },
      }),
    },
    lockManager: undefined,
  });
  broker.request(asset.id, 1);
  await done;
  await broker.cacheWork;
  assert.equal(writes, 0);
});

test("a contended persistence lock skips caching without retaining the payload", async (t) => {
  const held = deferred();
  t.after(() => held.resolve());
  let opens = 0;
  const { broker, asset, bytes } = setup(t, {
    storage: {
      open: async () => {
        ++opens;
      },
    },
    lockManager: {
      request: (name, options, callback) => {
        if (typeof options === "function") return held.promise.then(() => options({ name }));
        assert.equal(options.ifAvailable, true);
        return Promise.resolve(callback(null));
      },
    },
  });
  broker.persist(asset, bytes);
  let settled = false;
  void broker.cacheWork.then(() => {
    settled = true;
  });
  await new Promise(setImmediate);
  assert.equal(settled, true, "a held cross-tab lock must not queue cache work");
  assert.equal(opens, 0);
  assert.equal(broker.cacheWritePending, false);
});

test("closing during cache open drops the payload and prohibits a late write", async (t) => {
  const opened = deferred();
  const operations = [];
  const cache = {
    keys: async () => {
      operations.push("keys");
      return [];
    },
    put: async () => {
      operations.push("put");
    },
  };
  t.after(() => opened.resolve(cache));
  const { broker, asset, bytes } = setup(t, {
    storage: { open: () => opened.promise },
    lockManager: {
      request: (name, options, callback) => Promise.resolve((callback || options)({ name })),
    },
  });
  broker.persist(asset, bytes);
  await new Promise(setImmediate);
  const payload = broker.cachePersistence;
  let settled = false;
  void broker.cacheWork.then(() => {
    settled = true;
  });
  broker.close();
  await new Promise(setImmediate);
  assert.equal(
    settled,
    true,
    "close must settle best-effort cache work without waiting for storage",
  );
  assert.equal(payload?.bytes, null, "close must release the queued encoded payload");
  opened.resolve(cache);
  await new Promise(setImmediate);
  assert.deepEqual(operations, []);
});

for (const stop of ["close", "deadline"]) {
  test(`${stop} during cache put prevents follow-up work and overlapping writes`, async (t) => {
    const putting = deferred();
    t.after(() => putting.resolve());
    const delayed = [];
    const operations = [];
    let lockHeld = false;
    const cache = {
      keys: async () => {
        operations.push("keys");
        return [];
      },
      put: () => {
        operations.push("put");
        return putting.promise;
      },
    };
    const { broker, asset, bytes } = setup(t, {
      storage: { open: async () => cache },
      lockManager: {
        request: async (name, options, callback) => {
          lockHeld = true;
          try {
            return await callback({ name });
          } finally {
            lockHeld = false;
          }
        },
      },
      timers: {
        setTimeout(callback, ms) {
          const timer = { callback, ms };
          delayed.push(timer);
          return timer;
        },
        clearTimeout(timer) {
          timer.cancelled = true;
        },
      },
    });
    broker.persist(asset, bytes);
    await new Promise(setImmediate);
    assert.equal(operations.filter((operation) => operation === "put").length, 1);
    const beforeStop = [...operations];
    const payload = broker.cachePersistence;
    let settled = false;
    void broker.cacheWork.then(() => {
      settled = true;
    });
    if (stop === "close") broker.close();
    else {
      const deadline = delayed.find((timer) => !timer.cancelled);
      assert.ok(deadline, "persistence must own a deadline");
      assert.ok(deadline.ms <= 1000, "best-effort storage cannot hold a payload indefinitely");
      deadline.callback();
    }
    await new Promise(setImmediate);
    assert.equal(settled, true);
    assert.equal(payload?.bytes, null);
    assert.equal(lockHeld, true, "an issued mutation must retain its cross-tab lock");
    broker.persist(asset, bytes);
    await new Promise(setImmediate);
    assert.deepEqual(operations, beforeStop, "an unresolved put must never admit another write");
    putting.resolve();
    await new Promise(setImmediate);
    assert.deepEqual(
      operations,
      beforeStop,
      "late completion must not start trim or another write",
    );
    assert.equal(lockHeld, false);
  });
}

test("a trim deadline cancels a held body read before issuing any cache mutation", async (t) => {
  const delayed = [];
  let cancelled = false;
  let lockHeld = false;
  const operations = [];
  const response = new Response(
    new ReadableStream({
      pull() {
        return new Promise(() => {});
      },
      cancel() {
        cancelled = true;
      },
    }),
    { headers: { "content-length": "64", "x-donner-used": "1" } },
  );
  const cache = {
    keys: async () => ["https://editor.example/old-font"],
    match: async () => response,
    put: async () => operations.push("put"),
    delete: async () => operations.push("delete"),
  };
  const { broker, asset, bytes } = setup(t, {
    storage: { open: async () => cache },
    lockManager: {
      request: async (name, options, callback) => {
        lockHeld = true;
        try {
          return await callback({ name });
        } finally {
          lockHeld = false;
        }
      },
    },
    timers: {
      setTimeout(callback, ms) {
        const timer = { callback, ms };
        delayed.push(timer);
        return timer;
      },
      clearTimeout(timer) {
        timer.cancelled = true;
      },
    },
  });
  broker.persist(asset, bytes);
  await new Promise(setImmediate);
  assert.equal(lockHeld, true);
  delayed.find((timer) => !timer.cancelled).callback();
  await broker.cacheWork;
  await new Promise(setImmediate);
  assert.equal(cancelled, true);
  assert.equal(lockHeld, false);
  assert.deepEqual(operations, []);
});

test("a trim deadline holds an issued delete lock and never follows it with a put", async (t) => {
  const deleting = deferred();
  t.after(() => deleting.resolve());
  const delayed = [];
  const operations = [];
  let lockHeld = false;
  const cache = {
    keys: async () => ["https://editor.example/invalid-font"],
    match: async () => new Response(new Uint8Array(0)),
    put: async () => operations.push("put"),
    delete: () => {
      operations.push("delete");
      return deleting.promise;
    },
  };
  const { broker, asset, bytes } = setup(t, {
    storage: { open: async () => cache },
    lockManager: {
      request: async (name, options, callback) => {
        lockHeld = true;
        try {
          return await callback({ name });
        } finally {
          lockHeld = false;
        }
      },
    },
    timers: {
      setTimeout(callback, ms) {
        const timer = { callback, ms };
        delayed.push(timer);
        return timer;
      },
      clearTimeout(timer) {
        timer.cancelled = true;
      },
    },
  });
  broker.persist(asset, bytes);
  await new Promise(setImmediate);
  assert.deepEqual(operations, ["delete"]);
  delayed.find((timer) => !timer.cancelled).callback();
  await broker.cacheWork;
  assert.equal(lockHeld, true);
  broker.persist(asset, bytes);
  deleting.resolve();
  await new Promise(setImmediate);
  assert.equal(lockHeld, false);
  assert.deepEqual(operations, ["delete"]);
});

const bridgeSource = await readFile(
  new URL("../../WholeAppWorkerBridge.cc", import.meta.url),
  "utf8",
);
function bridgeBody(name) {
  const definition = bridgeSource.search(new RegExp(`\\n(?:uint32_t|void) ${name}\\(`));
  assert.notEqual(definition, -1);
  const start = bridgeSource.indexOf("MAIN_THREAD_ASYNC_EM_ASM(", definition);
  const nextDefinition = bridgeSource.indexOf("\nvoid ", start);
  const end = nextDefinition < 0 ? bridgeSource.length : nextDefinition;
  const bodyStart = bridgeSource.indexOf("{", start);
  const bodyEnd = bridgeSource.lastIndexOf("},", end);
  assert.ok(bodyEnd > bodyStart, `${name} must contain an embedded JavaScript body`);
  return bridgeSource.slice(bodyStart + 1, bodyEnd);
}

for (const name of ["InstallCatalogFonts", "RequestCatalogFont", "UninstallCatalogFonts"]) {
  test(`${name} embedded JavaScript remains valid after C++ formatting`, () => {
    assert.doesNotThrow(() => new Function("$0", "$1", "$2", "$3", "$4", bridgeBody(name)), name);
  });
}

test("teardown before the first frame removes the broker's startup observer", () => {
  const listeners = new Map();
  let broker;
  const window = {
    __donnerFirstFramePresented: false,
    __donnerCatalogPackageUrl: "https://editor.example/releases/a/",
    DonnerCatalogFontBroker: class {
      constructor() {
        broker = this;
        this.closed = false;
      }
      start() {
        throw new Error("a destroyed editor must not start font work");
      }
      close() {
        this.closed = true;
      }
    },
    addEventListener(name, callback) {
      listeners.set(name, callback);
    },
    removeEventListener(name, callback) {
      if (listeners.get(name) === callback) listeners.delete(name);
    },
  };
  const scope = vm.createContext({
    window,
    $0: 1,
    $1: 1,
    UTF8ToString: () => "[]",
    _free: () => {},
  });
  vm.runInContext(`{ ${bridgeBody("InstallCatalogFonts")} }`, scope);
  assert.equal(listeners.size, 1);
  vm.runInContext(`{ ${bridgeBody("UninstallCatalogFonts")} }`, scope);
  assert.equal(broker.closed, true);
  assert.equal(window.__donnerCatalogBrokers.size, 0);
  assert.equal(listeners.size, 0);
});

function embeddedBodies(source) {
  const macros =
    /\b(?:MAIN_THREAD_(?:ASYNC_)?EM_ASM(?:_INT)?|EM_ASM(?:_INT)?)\s*\(\s*\{|\bEM_JS\s*\([^{};]*\{/g;
  const bodies = [];
  for (const match of source.matchAll(macros)) {
    const start = match.index + match[0].length;
    // Quoted JavaScript and comments may contain braces which do not close the macro body.
    const tokens =
      /"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|`(?:\\.|[^`\\])*`|\/\/[^\n]*|\/\*[\s\S]*?\*\/|[{}]/g;
    tokens.lastIndex = start;
    let depth = 1;
    for (let token = tokens.exec(source); token; token = tokens.exec(source)) {
      if (token[0] === "{") ++depth;
      if (token[0] === "}" && --depth === 0) {
        bodies.push({
          body: source.slice(start, token.index),
          line: source.slice(0, start).split("\n").length,
        });
        break;
      }
    }
    assert.equal(depth, 0, `unterminated embedded JavaScript at ${start}`);
  }
  return bodies;
}

test("every worker bridge JavaScript body survives whole-file C++ formatting", () => {
  const bodies = embeddedBodies(bridgeSource);
  assert.ok(
    bodies.length >= 20,
    "expected the full worker bridge, including existing frame probes",
  );
  for (const { body, line } of bodies) {
    assert.doesNotThrow(
      () => new Function(...Array.from({ length: 16 }, (_, i) => `$${i}`), body),
      `WholeAppWorkerBridge.cc:${line}`,
    );
  }
});
