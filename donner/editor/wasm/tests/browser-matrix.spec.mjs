import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { createRequire } from "node:module";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const require = createRequire(import.meta.url);
const testDirectory = path.dirname(fileURLToPath(import.meta.url));
const repositoryRoot = path.resolve(testDirectory, "../../../..");

test("CI discovers Firefox, WebKit, and real Safari compatibility regressions", () => {
  const config = require("./playwright.compatibility.config.js");
  const projects = new Map(config.projects.map((project) => [project.name, project]));
  assert.deepEqual(config.testMatch, [
    "smoke.spec.ts",
    "browser-presentation-regression.spec.ts",
  ]);

  const firefox = projects.get("firefox-geode-resize");
  assert.ok(firefox, "missing Firefox Geode resize project");
  assert.equal(firefox.use.browserName, "firefox");
  assert.match(
    String(firefox.grep),
    /Firefox keeps Basic Shapes resize pixels and outline synchronized/,
  );
  assert.match(
    String(firefox.grep),
    /Firefox retains every accepted document epoch during a drag/,
  );
  assert.match(
    String(firefox.grep),
    /Firefox keeps every browser-composited Splash drag frame coherent/,
  );

  const webkit = projects.get("webkit-tiny-skia-carousel");
  assert.ok(webkit, "missing WebKit TinySkia carousel project");
  assert.equal(webkit.use.browserName, "webkit");
  assert.match(
    String(webkit.grep),
    /carousel loads Basic Shapes on the first interactive frame/,
  );
  assert.match(
    String(webkit.grep),
    /WebKit TinySkia survives a burst of drag wakeups without fatal errors/,
  );

  const packageJson = JSON.parse(readFileSync(path.join(testDirectory, "package.json"), "utf8"));
  assert.equal(
    packageJson.scripts["test:compatibility"],
    "playwright test --config=playwright.compatibility.config.js",
  );
  assert.equal(
    packageJson.scripts["test:safari-geode"],
    "node safari-geode-regression.mjs",
  );
  assert.match(packageJson.scripts["test:selector"], /browser-matrix\.spec\.mjs/);

  const workflow = readFileSync(
    path.join(repositoryRoot, ".github/workflows/editor_wasm.yml"),
    "utf8",
  );
  const normalizedWorkflow = workflow.replace(/\\\s*\n\s*/g, " ").replace(/\s+/g, " ");
  assert.match(
    normalizedWorkflow,
    /bazelisk test --config=editor-wasm --test_output=errors --remote_download_outputs=all \/\/donner\/editor\/wasm:wasm_geode_package_size_tests/,
  );
  assert.match(
    normalizedWorkflow,
    /bazelisk test --config=editor-wasm-tiny-skia --test_output=errors --remote_download_outputs=all \/\/donner\/editor\/wasm:wasm_tiny_skia_package_size_tests/,
  );
  assert.match(
    normalizedWorkflow,
    /npm --prefix donner\/editor\/wasm\/tests run test:compatibility -- --project=firefox-geode-resize/,
  );
  assert.match(
    normalizedWorkflow,
    /npm --prefix donner\/editor\/wasm\/tests run test:compatibility -- --project=webkit-tiny-skia-carousel/,
  );
  assert.match(workflow, /npm --prefix donner\/editor\/wasm\/tests run test:ios/);
});

test("real Safari gate pins the served Wasm and scopes every visibility probe", () => {
  const harness = readFileSync(
    path.join(testDirectory, "safari-geode-regression.mjs"),
    "utf8",
  );

  assert.match(harness, /DONNER_SAFARI_EXPECTED_WASM_SHA256/);
  assert.match(harness, /DONNER_SAFARI_ALLOW_UNPINNED_PACKAGE/);
  assert.match(harness, /\^\[0-9a-f\]\{64\}\$/);
  assert.match(
    harness,
    /result\.packageArtifacts\.wasm\.sha256,[\s\S]*expectedWasmSha256/,
    "the expected digest must be compared with bytes fetched from the served editor.wasm",
  );
  assert.match(
    harness,
    /async function requireVisibleSafariAnimationFrame[\s\S]*finally\s*\{[\s\S]*cleanupVisibleSafariAnimationFrameProbe/,
    "the timer/rAF probe must be cleaned up on success, timeout, and error",
  );
  assert.match(
    harness,
    /\/refresh[\s\S]*installErrorCapture\(driver\);[\s\S]*requireVisibleSafariAnimationFrame\(driver\);[\s\S]*waitForEditor/,
    "a reload must pass the same visible-rAF preflight as initial navigation",
  );
});
