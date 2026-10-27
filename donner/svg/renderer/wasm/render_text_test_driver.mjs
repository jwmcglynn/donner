import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { pathToFileURL } from "node:url";
import vm from "node:vm";

const [, , glue, wasm, assets, broker, loaderSource, mode] = process.argv;
vm.runInThisContext(readFileSync(broker, "utf8"), { filename: "catalog-fonts.js" });
vm.runInThisContext(readFileSync(loaderSource, "utf8"), {
  filename: "packaged-resource-loader.js",
});
const createModule = (await import(pathToFileURL(glue).href)).default;
const module = await createModule({ wasmBinary: readFileSync(wasm), print() {}, printErr() {} });
const manifest = globalThis.DonnerPackagedResourceLoader.compiledManifest(module);
assert.equal(manifest.schemaVersion, 1);
assert.ok(manifest.assets.some((asset) => asset.group === "fallback-font"));
const error = module.cwrap("donner_get_last_error", "string", []);
const render = module.cwrap("donner_render_svg", "number", ["string", "number", "number"]);
const freePixels = module.cwrap("donner_free_pixels", null, ["number"]);
const svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"128\" height=\"64\">"
  + "<text x=\"4\" y=\"42\" font-family=\"UninstalledFixtureFamily\" font-size=\"32\" fill=\"red\">Hi</text></svg>";

function install(asset, bytes) {
  const pointer = module._malloc(65 + bytes.byteLength);
  assert.notEqual(pointer, 0);
  try {
    const heap = module.HEAPU8;
    heap.set(new TextEncoder().encode(asset.sha256), pointer);
    heap[pointer + 64] = 0;
    heap.set(bytes, pointer + 65);
    return module._DonnerInstallPackagedResource(pointer, pointer + 65, bytes.byteLength);
  } finally {
    module._free(pointer);
  }
}

if (mode === "installed" || mode === "corrupt") {
  for (const asset of manifest.assets) {
    const bytes = readFileSync(join(assets, asset.path));
    if (mode === "corrupt") bytes[0] ^= 1;
    assert.equal(install(asset, bytes), mode === "installed" ? 1 : 0);
  }
} else if (mode === "manifest-mismatch") {
  const wrong = structuredClone(manifest);
  wrong.assets[0].encodedSize += 1;
  const loader = new globalThis.DonnerPackagedResourceLoader({
    baseUrl: "https://renderer.example/",
    manifest: wrong,
    brokerOptions: { enabled: false },
  });
  await assert.rejects(loader.install(module), /different packages/);
  loader.close();
} else {
  assert.equal(mode, "missing");
}

assert.equal(typeof module._donner_prepare_resources, "function");
assert.equal(module._donner_prepare_resources(), mode === "installed" ? 1 : 0);
module.ccall("donner_init", null, [], []);
const renderLength = module.cwrap("donner_render_svg_len", "number", [
  "number",
  "number",
  "number",
  "number",
]);
const bytes = new TextEncoder().encode(svg);
const input = module._malloc(bytes.byteLength);
assert.notEqual(input, 0);
module.HEAPU8.set(bytes, input);
try {
  // Both original public entry points must enforce the resource barrier independently.
  for (
    const invoke of [
      () => render(svg, 128, 64),
      () => renderLength(input, bytes.byteLength, 128, 64),
    ]
  ) {
    const pointer = invoke();
    if (mode === "installed") {
      assert.notEqual(pointer, 0, error());
      let red = 0;
      const pixels = module.HEAPU8.subarray(pointer, pointer + 128 * 64 * 4);
      for (let index = 0; index < pixels.length; index += 4) {
        if (
          pixels[index] > 128 && pixels[index + 1] < 64 && pixels[index + 2] < 64
          && pixels[index + 3] > 128
        ) ++red;
      }
      freePixels(pointer);
      assert.ok(red > 20, `Missing text pixels: ${red}`);
    } else {
      if (pointer) freePixels(pointer);
      assert.equal(pointer, 0, "Missing resources must not produce a successful blank bitmap");
      assert.match(error(), /packaged resources/i);
    }
  }
} finally {
  module._free(input);
}
console.log(`PASS=${mode}`);
