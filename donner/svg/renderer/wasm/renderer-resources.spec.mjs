import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";
import { pathToFileURL } from "node:url";

async function bootstrap(t, support = true) {
  const directory = await mkdtemp(join(tmpdir(), "renderer-resources-"));
  t.after(() => rm(directory, { recursive: true, force: true }));
  await writeFile(join(directory, "package.json"), "{\"type\":\"module\"}");
  // Copy bytes only; isolated modules do not need source ownership or timestamps.
  await writeFile(
    join(directory, "bootstrap.js"),
    await readFile(new URL("./renderer-resources.js", import.meta.url)),
  );
  if (support) {
    for (const name of ["catalog-fonts.js", "packaged-resource-loader.js"]) {
      await writeFile(
        join(directory, name),
        await readFile(new URL(`../../../editor/wasm/${name}`, import.meta.url)),
      );
    }
  }
  return (await import(pathToFileURL(join(directory, "bootstrap.js")).href))
    .prepareRendererResources;
}

function browser(t, fetch) {
  const before = new Map(
    ["location", "fetch"].map((key) => [key, Object.getOwnPropertyDescriptor(globalThis, key)]),
  );
  Object.defineProperty(globalThis, "location", {
    configurable: true,
    value: new URL("https://renderer.example/product/test.html"),
  });
  globalThis.fetch = fetch;
  t.after(() => {
    for (const [key, descriptor] of before) {
      if (descriptor) Object.defineProperty(globalThis, key, descriptor);
      else delete globalThis[key];
    }
  });
}

function fixture(accept = true) {
  const bytes = new TextEncoder().encode("owned font fixture bytes");
  const sha256 = createHash("sha256").update(bytes).digest("hex");
  const asset = {
    id: "kStandaloneFont",
    sha256,
    encodedSize: bytes.length,
    decodedSize: bytes.length,
    mediaType: "font/otf",
    group: "fallback-font",
    path: `assets/${sha256}.otf`,
  };
  const heap = new Uint8Array(new SharedArrayBuffer(128 * 1024));
  heap.set(new TextEncoder().encode(JSON.stringify({ schemaVersion: 1, assets: [asset] })), 16);
  const installed = [];
  const freed = [];
  const module = {
    HEAPU8: heap,
    prepareCalls: 0,
    _DonnerPackagedResourceManifest: () => 16,
    _malloc: () => 64 * 1024,
    _free: (pointer) => freed.push(pointer),
    _DonnerInstallPackagedResource(digestPointer, pointer, size) {
      const digest = new TextDecoder().decode(heap.slice(digestPointer, digestPointer + 64));
      const copy = heap.slice(pointer, pointer + size);
      assert.equal(digest, sha256);
      assert.deepEqual(copy, bytes);
      if (accept) installed.push(copy);
      return accept ? 1 : 0;
    },
    _donner_prepare_resources() {
      ++this.prepareCalls;
      assert.equal(installed.length, 1, "preflight ran before authenticated installation");
      return 1;
    },
  };
  return { bytes, asset, module, installed, freed };
}

function response(bytes) {
  return new Response(bytes, { headers: { "content-type": "font/otf" } });
}

test("no-text modules do not import transport or inspect inactive metadata", async (t) => {
  const prepare = await bootstrap(t, false);
  const module = {
    _DonnerPackagedResourceManifest() {
      throw new Error("inactive registry read");
    },
  };
  assert.equal(await prepare(module), null);
});

test("browser wrapper installs same-origin verified bytes without starting editor main", async (t) => {
  const prepare = await bootstrap(t);
  const { bytes, asset, module, installed, freed } = fixture();
  const requests = [];
  browser(t, async (url, options) => {
    requests.push(url);
    assert.equal(options.redirect, "error");
    assert.equal(options.credentials, "same-origin");
    return response(bytes);
  });
  const session = await prepare(module);
  t.after(() => session.close());
  assert.deepEqual(requests, [`https://renderer.example/product/${asset.path}`]);
  assert.equal(installed.length, 1);
  assert.equal(module.prepareCalls, 1);
  assert.deepEqual(freed, [64 * 1024]);
  assert.equal(Object.hasOwn(module, "callMain"), false);
});

test("cross-origin resource roots fail before any transport or installation", async (t) => {
  const prepare = await bootstrap(t, false);
  browser(t, () => {
    throw new Error("cross-origin fetch attempted");
  });
  await assert.rejects(
    prepare(fixture().module, { baseUrl: "https://outside.example/" }),
    /same-origin/,
  );
});

test("missing resource never reaches core preflight", async (t) => {
  const prepare = await bootstrap(t);
  const { module, installed } = fixture();
  browser(t, async () => new Response(null, { status: 404 }));
  await assert.rejects(prepare(module), /resource unavailable/i);
  assert.equal(installed.length, 0);
  assert.equal(module.prepareCalls, 0);
});

test("stream overrun is rejected at the compiled encoded byte bound", async (t) => {
  const prepare = await bootstrap(t);
  const { bytes, module, installed } = fixture();
  browser(t, async () => response(new Uint8Array(bytes.length + 1)));
  await assert.rejects(prepare(module), /resource unavailable/i);
  assert.equal(installed.length, 0);
  assert.equal(module.prepareCalls, 0);
});

test("corrupt resource bytes never reach installation", async (t) => {
  const prepare = await bootstrap(t);
  const { bytes, module, installed } = fixture();
  const corrupt = bytes.slice();
  corrupt[0] ^= 1;
  browser(t, async () => response(corrupt));
  await assert.rejects(prepare(module), /resource unavailable/i);
  assert.equal(installed.length, 0);
  assert.equal(module.prepareCalls, 0);
});

test("a core authentication rejection fails startup and frees the temporary heap buffer", async (t) => {
  const prepare = await bootstrap(t);
  const { bytes, module, installed, freed } = fixture(false);
  browser(t, async () => response(bytes));
  await assert.rejects(prepare(module), /installation failed/);
  assert.equal(installed.length, 0);
  assert.equal(module.prepareCalls, 0);
  assert.deepEqual(freed, [64 * 1024]);
});

test("closing during a request cancels startup without any later installation", async (t) => {
  const prepare = await bootstrap(t);
  const { bytes, module, installed } = fixture();
  let started;
  const requestStarted = new Promise((resolve) => {
    started = resolve;
  });
  let deliver;
  browser(t, async () => {
    started();
    return await new Promise((resolve) => {
      deliver = resolve;
    });
  });
  const lifetime = new AbortController();
  const operation = prepare(module, { signal: lifetime.signal });
  await requestStarted;
  lifetime.abort();
  await assert.rejects(operation, /closed|cancelled/i);
  deliver(response(bytes));
  await new Promise((resolve) => setTimeout(resolve, 0));
  assert.equal(installed.length, 0);
  assert.equal(module.prepareCalls, 0);
});
