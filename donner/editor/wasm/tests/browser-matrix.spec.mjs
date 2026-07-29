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
    /Firefox presents every accepted drag epoch on one stable surface/,
  );
  assert.match(
    String(firefox.grep),
    /Firefox never exposes the checkerboard while dragging a Splash letter/,
  );
  assert.match(
    String(firefox.grep),
    /Firefox bakes the Splash letter and overlay into one accepted surface epoch/,
  );

  const webkit = projects.get("webkit-geode-carousel");
  assert.ok(webkit, "missing WebKit Geode carousel project");
  assert.equal(webkit.use.browserName, "webkit");
  assert.match(
    String(webkit.grep),
    /carousel loads Basic Shapes on the first interactive frame/,
  );
  assert.match(
    String(webkit.grep),
    /WebKit Geode survives a burst of drag wakeups without fatal errors/,
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
  assert.match(workflow, /geode_excludes_tiny_skia_audit/);
  assert.doesNotMatch(
    workflow,
    /editor-wasm-tiny-skia|wasm_tiny_skia|package-tiny_skia|backend:\s*tiny_skia/,
  );
  assert.match(
    normalizedWorkflow,
    /npm --prefix donner\/editor\/wasm\/tests run test:compatibility -- --project=firefox-geode-resize/,
  );
  assert.match(
    normalizedWorkflow,
    /npm --prefix donner\/editor\/wasm\/tests run test:compatibility -- --project=webkit-geode-carousel/,
  );
  assert.doesNotMatch(workflow, /test:ios|playwright\\.ios/);
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

test("real Safari memory gate clicks Donner Splash and dwells for five minutes", () => {
  const harness = readFileSync(
    path.join(testDirectory, "safari-geode-regression.mjs"),
    "utf8",
  );

  assert.match(
    harness,
    /kMemoryOnly[\s\S]*id:\s*"donner-splash"[\s\S]*await click\(driver,\s*sampleClickPoint\.x,\s*sampleClickPoint\.y\)[\s\S]*activeSample\?\.sampleId === sample\.id/,
    "the memory gate must enter the Splash through the trusted carousel click path",
  );
  assert.match(
    harness,
    /const kMemoryDwellMs = 5 \* 60 \* 1_000;/,
    "the Safari significant-memory gate must cover the five-minute reload window",
  );
  assert.match(
    harness,
    /result\.memoryDwell\.length >= kMemoryDwellMs \/ kMemorySampleIntervalMs/,
    "the gate must prove its sampler stayed alive throughout the five-minute dwell",
  );
  assert.match(
    harness,
    /const kMemoryMaxWebContentRssBytes = 512 \* 1024 \* 1024;/,
    "the gate must fail before Safari's WebContent process reaches termination pressure",
  );
  assert.match(
    harness,
    /result\.memoryWebContentSamples\.push[\s\S]*rssBytes[\s\S]*kMemoryMaxWebContentRssBytes/,
    "the gate must observe WebContent RSS outside the page's Wasm and WebGPU counters",
  );
  assert.match(
    harness,
    /Safari WebContent process exited during the memory dwell/,
    "a WebContent process replacement must be reported as a reload or termination",
  );
  assert.match(
    harness,
    /__donnerSafariRegressionPageLifetimeToken[\s\S]*finalState\.pageLifetimeToken[\s\S]*kPageLifetimeToken/,
    "reload detection must use a stable page token rather than Safari's drifting time origin",
  );
});
