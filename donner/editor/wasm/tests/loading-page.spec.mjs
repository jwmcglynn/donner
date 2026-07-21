import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";

const html = readFileSync(new URL("../index.html", import.meta.url), "utf8");
const css = readFileSync(new URL("../editor.css", import.meta.url), "utf8");

test("loading page provides branded, accessible progress before Wasm starts", () => {
  assert.match(html, /<canvas[^>]+id="donner-worker-document-canvas"[^>]+aria-hidden="true"/);
  assert.match(html, /<canvas[^>]+id="donner-document-canvas"[^>]+aria-hidden="true"/);
  assert.match(html, /<canvas[^>]+id="donner-document-canvas-back"[^>]+aria-hidden="true"/);
  assert.match(html, /<main[^>]+id="loading-screen"[^>]+role="status"[^>]+aria-live="polite"/);
  assert.match(html, /<img[^>]+src="donner_icon\.svg"/);
  assert.match(html, /id="loading-title"/);
  assert.match(html, /id="status"/);
  assert.match(html, /src="worker-surface-selector\.js"/);
  assert.match(html, /role="progressbar"/);
  assert.match(css, /@media \(prefers-reduced-motion: reduce\)/);
  assert.doesNotMatch(html, /<style\b|<script(?![^>]+src=)|\son\w+=/);
});
