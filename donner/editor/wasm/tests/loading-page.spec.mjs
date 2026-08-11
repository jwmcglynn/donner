import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";

const html = readFileSync(new URL("../index.html", import.meta.url), "utf8");
const css = readFileSync(new URL("../editor.css", import.meta.url), "utf8");

test("loading page provides branded, accessible progress before Wasm starts", () => {
  assert.match(html, /<main[^>]+id="loading-screen"[^>]+role="status"[^>]+aria-live="polite"/);
  assert.match(html, /<img[^>]+src="donner_icon\.svg"/);
  assert.match(html, /id="loading-title"/);
  assert.match(html, /id="status"/);
  assert.match(html, /role="progressbar"/);
  assert.match(css, /@media \(prefers-reduced-motion: reduce\)/);
  assert.doesNotMatch(html, /<style\b|<script(?![^>]+src=)|\son\w+=/);
});

// the single-canvas architecture: one canvas, and no CSS between document space and the screen.
// The page used to ship three canvases (a worker-owned bitmap surface plus a
// front/back document pair) that the browser composited under the UI canvas,
// positioned and clipped from JS. Every presented pixel is now produced by
// Geode inside this one canvas's WebGPU frame, so a second canvas element or a
// CSS rule that places rendered content is a regression, not a refactor.
test("served page has exactly one canvas and no CSS placement of rendered content", () => {
  const canvases = html.match(/<canvas\b[^>]*>/g) ?? [];
  assert.equal(canvases.length, 1, `expected one canvas, got ${canvases.join(", ")}`);
  assert.match(canvases[0], /id="canvas"/);
  assert.doesNotMatch(html, /donner-document-canvas|donner-worker-document-canvas/);
  assert.doesNotMatch(html, /worker-surface-selector\.js/);
  assert.doesNotMatch(css, /donner-document-canvas|donner-worker-document-canvas/);
  assert.doesNotMatch(css, /clip-path/);
});
