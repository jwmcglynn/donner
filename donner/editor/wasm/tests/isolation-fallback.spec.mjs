import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";
import vm from "node:vm";

const kFallbackScriptUrl = "https://example.test/enable-threads.js";

async function loadFallbackSource() {
  return readFile(new URL("../enable-threads.js", import.meta.url), "utf8");
}

async function runPageFallback({ controller = null, crossOriginIsolated = false } = {}) {
  const source = await loadFallbackSource();
  let registeredUrl = null;
  let reloads = 0;
  const registration = {
    active: {},
    addEventListener() {},
  };
  const document = { currentScript: { src: kFallbackScriptUrl } };
  const window = {
    crossOriginIsolated,
    document,
    isSecureContext: true,
    location: {
      reload() {
        reloads += 1;
      },
    },
  };
  const serviceWorker = {
    controller,
    async register(url) {
      registeredUrl = url;
      return registration;
    },
  };
  const context = vm.createContext({
    console,
    document,
    navigator: { serviceWorker },
    window,
  });

  vm.runInContext(source, context, { filename: "enable-threads.js" });
  await new Promise((resolve) => setImmediate(resolve));
  return { registeredUrl, reloads };
}

async function runWorkerFallback() {
  const source = await loadFallbackSource();
  const listeners = new Map();
  const context = vm.createContext({
    console,
    fetch: async () => new Response("editor"),
    Headers,
    Request,
    Response,
    self: {
      addEventListener(type, listener) {
        listeners.set(type, listener);
      },
      clients: { claim: async () => {} },
      skipWaiting() {},
    },
  });

  vm.runInContext(source, context, { filename: "enable-threads.js" });
  const fetchListener = listeners.get("fetch");
  assert.equal(typeof fetchListener, "function");

  let responsePromise;
  fetchListener({
    request: new Request("https://example.test/index.html"),
    respondWith(promise) {
      responsePromise = promise;
    },
  });
  assert.ok(responsePromise);
  return responsePromise;
}

test("secure unisolated pages register and activate the isolation fallback", async () => {
  const result = await runPageFallback();

  assert.deepEqual(result, {
    registeredUrl: kFallbackScriptUrl,
    reloads: 1,
  });
});

test("isolated pages do not register the fallback", async () => {
  const result = await runPageFallback({ crossOriginIsolated: true });

  assert.deepEqual(result, {
    registeredUrl: null,
    reloads: 0,
  });
});

test("newly controlled unisolated pages reload through the fallback", async () => {
  const result = await runPageFallback({ controller: {} });

  assert.deepEqual(result, {
    registeredUrl: kFallbackScriptUrl,
    reloads: 1,
  });
});

test("fallback responses provide the pthread isolation headers", async () => {
  const response = await runWorkerFallback();

  assert.equal(response.headers.get("Cross-Origin-Opener-Policy"), "same-origin");
  assert.equal(response.headers.get("Cross-Origin-Embedder-Policy"), "require-corp");
});
