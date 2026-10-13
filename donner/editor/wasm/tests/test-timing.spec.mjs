import assert from "node:assert/strict";
import { mkdtempSync, readdirSync, readFileSync, rmSync } from "node:fs";
import { createRequire } from "node:module";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const require = createRequire(import.meta.url);
const testDirectory = path.dirname(fileURLToPath(import.meta.url));
const { kCiTimeScale, kStallGuardMs, scaledMs, stallGuardPollOptions } = require(
  "./test-timing.cjs",
);

test("the stall guard is a generous hang bound, not a latency budget", () => {
  assert.equal(kStallGuardMs, 60_000);
  assert.ok(kStallGuardMs >= 10_000, "a stall guard must be tens of seconds");
});

test("stall-guarded polls carry the guard and name the state they wait for", () => {
  assert.deepEqual(stallGuardPollOptions("the worker goes idle"), {
    message: "the worker goes idle",
    timeout: kStallGuardMs,
  });
  assert.throws(() => stallGuardPollOptions(""), /needs a label/);
  assert.throws(() => stallGuardPollOptions(undefined), /needs a label/);
});

test("the CI time scale keeps its existing values", () => {
  assert.equal(kCiTimeScale, process.env.CI ? 4 : 1);
  assert.equal(scaledMs(250), 250 * kCiTimeScale);
});

test("the Playwright wrapper polls through the shared stall guard", () => {
  const source = readFileSync(path.join(testDirectory, "test-timing.ts"), "utf8");
  assert.match(source, /from "\.\/test-timing\.cjs"/);
  assert.match(source, /expect\.poll\(fn, stallGuardPollOptions\(label\)\)/);
});

// Every config, the derived Bazel configs included, must bound web-first assertions by the stall
// guard: Playwright's own 5 s default applies otherwise, unscaled, on any host.
test("every Playwright config bounds web-first assertions by the stall guard", (t) => {
  const temporary = mkdtempSync(path.join(process.env.TEST_TMPDIR ?? testDirectory, "configs-"));
  t.after(() => rmSync(temporary, { recursive: true, force: true }));
  Object.assign(process.env, {
    TEST_TMPDIR: temporary,
    TEST_UNDECLARED_OUTPUTS_DIR: path.join(temporary, "outputs"),
    DONNER_WASM_TEST_SERVER: "remote-webserver.mjs",
    DONNER_WASM_PACKAGE_DIR: "package",
    DONNER_WEBKIT_EXECUTABLE: "webkit",
  });
  const configs = readdirSync(testDirectory).filter((name) => /^playwright\..*config\.js$/.test(name));
  assert.ok(configs.length >= 10, `found only ${configs.join(", ")}`);
  for (const config of configs) {
    const loaded = require(path.join(testDirectory, config));
    assert.equal(loaded.expect?.timeout, kStallGuardMs, `${config} expect.timeout`);
  }
});

// A wall-clock scale or bound copied into a spec drifts from the shared one and hides how many
// timing assumptions the suites still make. Every timing constant comes from test-timing.cjs.
const kTimingDefinitions = [
  /\b(?:const|let|var|function)\s+(?:kCiTimeScale|scaledMs|kStallGuardMs)\b/,
  // A CI-only multiplier that leaves local runs at 1, written inline.
  /process\.env\.CI\s*\?\s*\d+(?:\.\d+)?\s*:\s*1\s*[;),]/,
];

function duplicatedTimingDefinitions(files) {
  const found = [];
  for (const [name, source] of files) {
    if (name === "test-timing.cjs" || name === "test-timing.spec.mjs") continue;
    for (const [index, line] of source.split("\n").entries()) {
      if (kTimingDefinitions.some((pattern) => pattern.test(line))) {
        found.push(`${name}:${index + 1}: ${line.trim()}`);
      }
    }
  }
  return found;
}

test("the duplicate scan recognizes each way a spec copied the CI time scale", () => {
  assert.deepEqual(
    duplicatedTimingDefinitions([
      ["a.spec.ts", "const kCiTimeScale = process.env.CI ? 4 : 1;"],
      ["b.spec.ts", "const scaledMs = (ms: number) => ms * (process.env.CI ? 4 : 1);"],
      ["c.ts", "function scaledMs(ms) { return ms; }"],
      ["d.mjs", "let kStallGuardMs = 5000;"],
      ["f.spec.ts", "const kMinimumSamples = process.env.CI ? 12 : 16;"],
      ["e.spec.ts", "import { scaledMs } from \"./test-timing\";\nawait waitFor(scaledMs(10));"],
      ["test-timing.cjs", "const kCiTimeScale = process.env.CI ? 4 : 1;"],
    ]),
    [
      "a.spec.ts:1: const kCiTimeScale = process.env.CI ? 4 : 1;",
      "b.spec.ts:1: const scaledMs = (ms: number) => ms * (process.env.CI ? 4 : 1);",
      "c.ts:1: function scaledMs(ms) { return ms; }",
      "d.mjs:1: let kStallGuardMs = 5000;",
    ],
  );
});

test("no test module defines its own CI time scale or stall guard", () => {
  const files = readdirSync(testDirectory)
    .filter((name) => /\.(?:[cm]?js|ts)$/.test(name))
    .map((name) => [name, readFileSync(path.join(testDirectory, name), "utf8")]);
  assert.deepEqual(
    duplicatedTimingDefinitions(files),
    [],
    "import kStallGuardMs, waitForState or scaledMs from ./test-timing instead",
  );
});
