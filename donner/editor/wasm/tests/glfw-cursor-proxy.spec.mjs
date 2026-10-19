// The editor's canvas-owning pthread sets the OS cursor through the vendored GLFW port on every
// frame while ImGui owns the cursor. A synchronous proxy blocks that pthread on the main thread,
// and Firefox's canvas snapshot blocks the main thread on that pthread, so the two deadlock until
// the snapshot times out. Cursor setters must stay fire-and-forget.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import test from "node:test";

const libraryPath = process.env.DONNER_GLFW_JS_LIBRARY;
assert.ok(libraryPath, "DONNER_GLFW_JS_LIBRARY must be set by the Bazel test target");
const library = readFileSync(resolve(libraryPath), "utf8");

function proxyMode(name) {
  const match = new RegExp(`\\b${name}__proxy:\\s*'([a-z]+)'`).exec(library);
  assert.ok(match, `${name} declares a proxy mode`);
  return match[1];
}

test("GLFW cursor setters proxy to the main thread asynchronously", () => {
  for (const name of [
    "emscripten_glfw3_window_set_standard_cursor",
    "emscripten_glfw3_window_set_custom_cursor",
  ]) {
    assert.equal(proxyMode(name), "async", name);
  }
});
