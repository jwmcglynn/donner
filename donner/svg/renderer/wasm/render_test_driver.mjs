// Headless render driver for the tiny_skia Donner SVG WebAssembly module.
//
// Loads the Emscripten ES6 glue exactly as a browser would (the same default
// module factory), but supplies the .wasm bytes directly via `wasmBinary` so
// the web-targeted module runs under Node without a browser or a node-specific
// build. It then renders a nontrivial SVG through the public C API
// (donner_init / donner_render_svg / donner_free_pixels / donner_get_last_error),
// reads the RGBA pixels back out of the wasm heap via the exported HEAPU8 view,
// and prints a stable FNV-1a hash plus a non-zero byte count.
//
// This exercises the full JS-glue marshaling path (cwrap, UTF8ToString string
// passing, HEAPU8 memory access, the exported C functions), so a glue-level
// regression -- for example closure minification renaming a runtime method --
// makes it fail. The pixel hash itself comes from the wasm module and is
// therefore invariant to glue changes; only a real renderer change moves it.
//
// Usage: node render_test_driver.mjs <glue.mjs> <module.wasm> <width> <height>
// Output on success (stdout):
//   HASH=<8 hex digits>
//   NONZERO=<n>/<total>
//   BYTES=<total>
// Any failure prints a line beginning with "ERROR:" and exits non-zero.

import { readFileSync } from 'node:fs';
import { pathToFileURL } from 'node:url';

function fail(message) {
  console.error('ERROR: ' + message);
  process.exit(2);
}

const [, , gluePath, wasmPath, widthArg, heightArg] = process.argv;
if (!gluePath || !wasmPath) {
  fail('usage: render_test_driver.mjs <glue.mjs> <module.wasm> [width] [height]');
}
const width = Number.parseInt(widthArg ?? '64', 10);
const height = Number.parseInt(heightArg ?? '64', 10);

const SVG = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
  <defs>
    <linearGradient id="g" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0%" stop-color="#89b4fa"/>
      <stop offset="100%" stop-color="#f38ba8"/>
    </linearGradient>
  </defs>
  <rect width="200" height="200" rx="24" fill="url(#g)"/>
  <circle cx="100" cy="90" r="40" fill="#1e1e2e" opacity="0.85"/>
  <path d="M70 130 Q100 170 130 130" stroke="#a6e3a1" stroke-width="4" fill="none" stroke-linecap="round"/>
</svg>`;

let wasmBinary;
try {
  wasmBinary = readFileSync(wasmPath);
} catch (err) {
  fail('could not read wasm file ' + wasmPath + ': ' + err.message);
}

let createModule;
try {
  createModule = (await import(pathToFileURL(gluePath).href)).default;
} catch (err) {
  fail('could not import glue module ' + gluePath + ': ' + err.message);
}
if (typeof createModule !== 'function') {
  fail('glue module default export is not a factory function');
}

let Module;
try {
  Module = await createModule({ wasmBinary, print: () => {}, printErr: () => {} });
} catch (err) {
  fail('module instantiation failed: ' + err.message);
}

for (const name of ['cwrap', 'HEAPU8']) {
  if (Module[name] === undefined) {
    fail('runtime method/view "' + name + '" is missing from the module (glue regression?)');
  }
}

const donnerInit = Module.cwrap('donner_init', null, []);
const donnerRenderSvg = Module.cwrap('donner_render_svg', 'number', ['string', 'number', 'number']);
const donnerFreePixels = Module.cwrap('donner_free_pixels', null, ['number']);
const donnerGetError = Module.cwrap('donner_get_last_error', 'string', []);

donnerInit();

const ptr = donnerRenderSvg(SVG, width, height);
if (ptr === 0) {
  fail('donner_render_svg returned null: ' + donnerGetError());
}

const total = width * height * 4;
const pixels = Module.HEAPU8.subarray(ptr, ptr + total);
if (pixels.length !== total) {
  fail('pixel view has wrong length: ' + pixels.length + ' != ' + total);
}

let hash = 0x811c9dc5;
let nonZero = 0;
for (let i = 0; i < pixels.length; i++) {
  if (pixels[i] !== 0) nonZero++;
  hash ^= pixels[i];
  hash = Math.imul(hash, 0x01000193) >>> 0;
}
donnerFreePixels(ptr);

console.log('HASH=' + hash.toString(16).padStart(8, '0'));
console.log('NONZERO=' + nonZero + '/' + total);
console.log('BYTES=' + total);
