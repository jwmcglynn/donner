import assert from "node:assert/strict";
import { mkdtempSync, readFileSync, rmSync, statSync, writeFileSync } from "node:fs";
import { createRequire } from "node:module";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";
import vm from "node:vm";

const require = createRequire(import.meta.url);
const directory = path.dirname(fileURLToPath(import.meta.url));

function evaluateConfig(filename, baseConfig, environment, temporary) {
  const module = { exports: {} };
  const context = {
    module,
    require: (specifier) =>
      specifier.startsWith("./playwright.")
        ? baseConfig
        : require(specifier),
    process: { env: environment, cwd: () => temporary, execPath: process.execPath },
  };
  vm.runInNewContext(readFileSync(path.join(directory, filename), "utf8"), context, {
    filename,
  });
  return module.exports;
}

function fixture(t) {
  const temporary = mkdtempSync(path.join(os.tmpdir(), "playwright-config-"));
  t.after(() => rmSync(temporary, { recursive: true, force: true }));
  const environment = {
    TEST_TMPDIR: temporary,
    TEST_UNDECLARED_OUTPUTS_DIR: path.join(temporary, "outputs"),
    DONNER_WASM_TEST_SERVER: "remote-webserver.mjs",
    DONNER_WASM_PACKAGE_DIR: "package",
    HOME: "/unwritable-caller-home",
    CALLER_SETTING: "preserved",
    BREAKPAD_DUMP_LOCATION: "/unwritable-caller-home/crashes",
  };
  const baseConfig = {
    use: {
      viewport: { width: 100, height: 100 },
      launchOptions: {
        args: ["--existing-launch-argument"],
        timeout: 1234,
        env: { LAUNCH_SETTING: "preserved" },
      },
    },
  };
  return { temporary, environment, baseConfig };
}

test("Bazel config supplies a writable per-test Chromium crash database", (t) => {
  const { temporary, environment, baseConfig } = fixture(t);
  const config = evaluateConfig("playwright.bazel.config.js", baseConfig, environment, temporary);
  const expected = path.join(temporary, "chromium-crashpad");
  assert.equal(config.use.launchOptions.env?.BREAKPAD_DUMP_LOCATION, expected);
  assert.ok(statSync(expected).isDirectory());
  const probe = path.join(expected, "write-probe");
  writeFileSync(probe, "writable");
  assert.equal(readFileSync(probe, "utf8"), "writable");
  assert.equal(environment.BREAKPAD_DUMP_LOCATION, "/unwritable-caller-home/crashes");
});

test("Bazel browser launch preserves caller environment and launch options", (t) => {
  const { temporary, environment, baseConfig } = fixture(t);
  const config = evaluateConfig("playwright.bazel.config.js", baseConfig, environment, temporary);
  assert.equal(config.use.launchOptions.env?.HOME, environment.HOME);
  assert.equal(config.use.launchOptions.env?.CALLER_SETTING, "preserved");
  assert.equal(config.use.launchOptions.env?.LAUNCH_SETTING, "preserved");
  assert.equal(config.use.launchOptions.timeout, 1234);
  assert.deepEqual(Array.from(config.use.launchOptions.args), ["--existing-launch-argument"]);
  assert.equal(config.use.viewport.width, 100);
});

test("Bazel config requires an owned temporary directory", (t) => {
  const { temporary, environment, baseConfig } = fixture(t);
  delete environment.TEST_TMPDIR;
  assert.throws(
    () => evaluateConfig("playwright.bazel.config.js", baseConfig, environment, temporary),
    /TEST_TMPDIR must be supplied/,
  );
});

test("Crash directory creation errors fail before browser launch", (t) => {
  const { temporary, environment, baseConfig } = fixture(t);
  writeFileSync(path.join(temporary, "chromium-crashpad"), "occupied");
  assert.throws(
    () => evaluateConfig("playwright.bazel.config.js", baseConfig, environment, temporary),
    /EEXIST/,
  );
});

test("Boot config inherits the crash directory without changing its launch deadline", (t) => {
  const { temporary, environment, baseConfig } = fixture(t);
  const bazelConfig = evaluateConfig(
    "playwright.bazel.config.js",
    baseConfig,
    environment,
    temporary,
  );
  const bootConfig = evaluateConfig(
    "playwright.boot-presentation.bazel.config.js",
    bazelConfig,
    environment,
    temporary,
  );
  assert.equal(
    bootConfig.use.launchOptions.env?.BREAKPAD_DUMP_LOCATION,
    path.join(temporary, "chromium-crashpad"),
  );
  assert.equal(bootConfig.use.launchOptions.timeout, 15000);
  assert.equal(bootConfig.timeout, 60000);
});
