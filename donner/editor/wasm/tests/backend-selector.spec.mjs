import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";
import vm from "node:vm";

async function loadSelector() {
  const source = await readFile(new URL("../backend-selector.js", import.meta.url), "utf8");
  const context = vm.createContext({});
  vm.runInContext(source, context, { filename: "backend-selector.js" });
  assert.equal(typeof context.SelectDonnerBackend, "function");
  return context.SelectDonnerBackend;
}

async function loadBrowserSelector({ hasWebGl2 }) {
  const source = await readFile(new URL("../backend-selector.js", import.meta.url), "utf8");
  const window = { location: { search: "" } };
  const context = vm.createContext({
    URLSearchParams,
    document: {
      createElement: () => ({
        getContext: () =>
          hasWebGl2
            ? { getExtension: () => ({ loseContext() {} }) }
            : null,
      }),
    },
    navigator: {},
    window,
  });
  vm.runInContext(source, context, { filename: "backend-selector.js" });
  return { selection: window.__donnerBackendPromise };
}

test("auto mode prefers Geode when WebGPU has an adapter", async () => {
  const selectBackend = await loadSelector();
  const result = await selectBackend({
    requestedBackend: "auto",
    requestWebGpuAdapter: async () => ({}),
    hasWebGl2: () => true,
  });
  assert.deepEqual({ ...result }, { name: "geode", base: "geode/" });
});

test("auto mode falls back to TinySkia when WebGPU is absent", async () => {
  const selectBackend = await loadSelector();
  const result = await selectBackend({
    requestedBackend: "auto",
    requestWebGpuAdapter: null,
    hasWebGl2: () => true,
  });
  assert.deepEqual({ ...result }, { name: "tiny_skia", base: "tiny_skia/" });
});

test("auto mode falls back when WebGPU exposes no adapter", async () => {
  const selectBackend = await loadSelector();
  const result = await selectBackend({
    requestedBackend: "auto",
    requestWebGpuAdapter: async () => null,
    hasWebGl2: () => true,
  });
  assert.deepEqual({ ...result }, { name: "tiny_skia", base: "tiny_skia/" });
});

test("an explicit TinySkia request does not probe WebGPU", async () => {
  const selectBackend = await loadSelector();
  let webGpuProbes = 0;
  const result = await selectBackend({
    requestedBackend: "tiny_skia",
    requestWebGpuAdapter: async () => {
      webGpuProbes += 1;
      return {};
    },
    hasWebGl2: () => true,
  });
  assert.deepEqual({ ...result }, { name: "tiny_skia", base: "tiny_skia/" });
  assert.equal(webGpuProbes, 0);
});

test("rejects a forced Geode backend without a WebGPU adapter", async () => {
  const selectBackend = await loadSelector();
  await assert.rejects(
    selectBackend({
      requestedBackend: "geode",
      requestWebGpuAdapter: async () => null,
      hasWebGl2: () => true,
    }),
    /WebGPU adapter/,
  );
});

test("rejects TinySkia when WebGL2 is unavailable", async () => {
  const selectBackend = await loadSelector();
  await assert.rejects(
    selectBackend({
      requestedBackend: "auto",
      requestWebGpuAdapter: null,
      hasWebGl2: () => false,
    }),
    /WebGL2/,
  );
});

test("rejects unknown backend overrides", async () => {
  const selectBackend = await loadSelector();
  await assert.rejects(
    selectBackend({
      requestedBackend: "metal",
      requestWebGpuAdapter: async () => ({}),
      hasWebGl2: () => true,
    }),
    /Unknown renderer backend/,
  );
});

test("browser capability rejection is handled by the published promise", async () => {
  const { selection } = await loadBrowserSelector({ hasWebGl2: false });
  await assert.rejects(selection, /WebGL2/);
  await new Promise((resolve) => setImmediate(resolve));
});
