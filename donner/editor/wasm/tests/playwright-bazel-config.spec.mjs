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
const {
  firefoxAquaLauncherPath,
  firefoxExecutablePath,
  writeFirefoxAquaLauncher,
} = require("./firefox-aqua-setup.js");

function evaluateConfig(filename, baseConfig, environment, temporary) {
  const module = { exports: {} };
  const context = {
    module,
    require: (specifier) =>
      specifier.startsWith("./playwright.")
        ? (baseConfig[specifier] ?? baseConfig)
        : require(specifier),
    process: { env: environment, cwd: () => temporary, execPath: process.execPath, pid: 4242 },
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

test("Bazel web server binds an OS-selected port and exports its URL", (t) => {
  const { temporary, environment, baseConfig } = fixture(t);
  const config = evaluateConfig("playwright.bazel.config.js", baseConfig, environment, temporary);
  assert.match(config.webServer.command, /--port 0(?: |$)/);
  assert.equal(config.webServer.url, undefined);
  assert.equal(config.webServer.reuseExistingServer, undefined);
  const ready = config.webServer.wait.stdout.exec(
    "DONNER_WASM_BASE_URL=http://127.0.0.1:49152",
  );
  assert.equal(ready?.groups?.DONNER_WASM_BASE_URL, "http://127.0.0.1:49152");
  assert.equal(environment.DONNER_WASM_BASE_URL, undefined);
});

test("Bazel browser launch preserves caller environment and launch options", (t) => {
  const { temporary, environment, baseConfig } = fixture(t);
  const config = evaluateConfig("playwright.bazel.config.js", baseConfig, environment, temporary);
  assert.equal(config.use.launchOptions.env?.HOME, environment.HOME);
  assert.equal(config.use.launchOptions.env?.CALLER_SETTING, "preserved");
  assert.equal(config.use.launchOptions.timeout, 1234);
  assert.equal(Array.from(config.use.launchOptions.args)[0], "--existing-launch-argument");
  assert.equal(config.use.viewport.width, 100);
});

test("only the hosted job's Chromium writes its own log into the test outputs", (t) => {
  const { temporary, environment, baseConfig } = fixture(t);
  const quiet = evaluateConfig("playwright.bazel.config.js", baseConfig, environment, temporary);
  assert.deepEqual(Array.from(quiet.use.launchOptions.args), ["--existing-launch-argument"]);

  environment.DONNER_BROWSER_STALL_DIAGNOSTICS = "1";
  const config = evaluateConfig("playwright.bazel.config.js", baseConfig, environment, temporary);
  const args = Array.from(config.use.launchOptions.args);
  assert.equal(args.length, 3, args.join(" "));
  assert.equal(args[1], "--enable-logging");
  const logFile = args[2].replace(/^--log-file=/, "");
  assert.notEqual(logFile, args[2], "the third argument must name the log file");
  assert.equal(path.dirname(logFile), environment.TEST_UNDECLARED_OUTPUTS_DIR);
  assert.equal(path.basename(logFile), "chromium-4242.log", "one log per worker process");
});

test("Explicit launch environments retain their filtering", (t) => {
  const { temporary, environment, baseConfig } = fixture(t);
  baseConfig.use.launchOptions.env = { LAUNCH_SETTING: "preserved" };
  const config = evaluateConfig("playwright.bazel.config.js", baseConfig, environment, temporary);
  assert.equal(config.use.launchOptions.env.LAUNCH_SETTING, "preserved");
  assert.equal(config.use.launchOptions.env.CALLER_SETTING, undefined);
  assert.equal(config.use.launchOptions.env.HOME, undefined);
  assert.equal(
    config.use.launchOptions.env.BREAKPAD_DUMP_LOCATION,
    path.join(temporary, "chromium-crashpad"),
  );
});

test("Bazel config requires an owned temporary directory", (t) => {
  const { temporary, environment, baseConfig } = fixture(t);
  delete environment.TEST_TMPDIR;
  assert.throws(
    () => evaluateConfig("playwright.bazel.config.js", baseConfig, environment, temporary),
    /TEST_TMPDIR must be supplied/,
  );
});

test("Firefox Aqua bootstrap derives only test-owned and pinned-runtime paths", (t) => {
  const { temporary, environment } = fixture(t);
  environment.PLAYWRIGHT_BROWSERS_PATH = path.join(temporary, "pinned-browsers");
  assert.equal(firefoxAquaLauncherPath(environment), path.join(temporary, "firefox-aqua.sh"));
  assert.equal(
    firefoxExecutablePath(environment),
    path.join(
      temporary,
      "pinned-browsers/firefox-1538/firefox/Nightly.app/Contents/MacOS/firefox",
    ),
  );
  assert.throws(() => firefoxExecutablePath({}), /PLAYWRIGHT_BROWSERS_PATH must be supplied/);
});

test("Firefox Aqua bootstrap writes a real shebang and preserves launcher arguments", (t) => {
  const { temporary, environment } = fixture(t);
  const executable = path.join(temporary, "pinned browser's/firefox");
  writeFirefoxAquaLauncher(environment, 501, executable);
  const launcher = firefoxAquaLauncherPath(environment);
  const script = readFileSync(launcher, "utf8");
  assert.equal(script.includes("\\n"), false, "the launcher must contain real newline bytes");
  const lines = script.split("\n");
  assert.equal(lines[0], "#!/bin/sh");
  assert.match(lines[1], /^exec \/bin\/launchctl asuser 501 '/);
  assert.match(lines[1], /pinned browser'"'"'s\/firefox/);
  assert.ok(lines[1].endsWith(' "$@"'), "the launcher must preserve every Playwright argument");
  assert.equal(lines[2], "", "the launcher must end with a newline");
  assert.equal(statSync(launcher).mode & 0o777, 0o700);
});

test("Crash directory creation errors fail before browser launch", (t) => {
  const { temporary, environment, baseConfig } = fixture(t);
  writeFileSync(path.join(temporary, "chromium-crashpad"), "occupied");
  assert.throws(
    () => evaluateConfig("playwright.bazel.config.js", baseConfig, environment, temporary),
    /EEXIST/,
  );
});

test("Boot config inherits the crash directory and budgets a slow cold launch", (t) => {
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
  assert.equal(bootConfig.use.launchOptions.timeout, 30000);
  assert.equal(bootConfig.globalTimeout, 120000);
  assert.equal(bootConfig.timeout, 60000);
});

test("Composited Chromium Bazel config keeps headed Metal and enables its specs", (t) => {
  const { temporary, environment, baseConfig } = fixture(t);
  baseConfig.testIgnore = [/composited/];
  const bazelConfig = evaluateConfig(
    "playwright.bazel.config.js",
    baseConfig,
    environment,
    temporary,
  );
  const chromiumConfig = {
    testIgnore: [/composited/],
    use: {
      channel: "chromium",
      headless: false,
      launchOptions: { args: ["--enable-unsafe-webgpu", "--use-angle=metal"] },
    },
  };
  const config = evaluateConfig(
    "playwright.composited-chromium.bazel.config.js",
    {
      "./playwright.bazel.config.js": bazelConfig,
      "./playwright.composited-chromium.config.js": chromiumConfig,
    },
    environment,
    temporary,
  );
  // The adapter is evaluated against its Bazel base in production; pin the behavior it must add.
  assert.deepEqual(Array.from(config.testIgnore), []);
  assert.equal(config.use.channel, "chromium");
  assert.equal(config.use.headless, false);
  assert.deepEqual(Array.from(config.use.launchOptions.args), [
    "--enable-unsafe-webgpu",
    "--use-angle=metal",
  ]);
  assert.deepEqual(config.webServer, bazelConfig.webServer);
  assert.equal(
    config.use.launchOptions.env.BREAKPAD_DUMP_LOCATION,
    path.join(temporary, "chromium-crashpad"),
  );
});

test("WebKit adapter uses the pinned launcher without Chromium launch settings", (t) => {
  const { temporary, environment, baseConfig } = fixture(t);
  environment.DONNER_WEBKIT_EXECUTABLE = path.join(temporary, "pinned-webkit", "pw_run.sh");
  environment.DONNER_BROWSER_HEADLESS = "1";
  baseConfig.webServer = { command: "bazel-owned-server" };
  const config = evaluateConfig(
    "playwright.webkit.bazel.config.js",
    {
      "./playwright.bazel.config.js": baseConfig,
      "./playwright.compatibility.config.js": {
        projects: [
          { name: "firefox", use: { browserName: "firefox" } },
          {
            name: "webkit",
            use: { browserName: "webkit", viewport: { width: 1280, height: 900 } },
          },
        ],
      },
    },
    environment,
    temporary,
  );
  assert.equal(config.webServer, baseConfig.webServer);
  assert.equal(config.use.channel, undefined);
  assert.equal(config.use.launchOptions, undefined);
  assert.equal(config.projects.length, 1);
  const project = config.projects[0];
  assert.equal(project.name, "webkit");
  assert.equal(project.use.browserName, "webkit");
  assert.equal(project.use.headless, true);
  assert.equal(project.use.viewport.width, 1280);
  assert.equal(project.use.launchOptions.executablePath, environment.DONNER_WEBKIT_EXECUTABLE);
  assert.equal(project.use.launchOptions.args, undefined);
});

test("Firefox presentation adapter keeps the compatibility project and Bazel server", (t) => {
  const { temporary, environment, baseConfig } = fixture(t);
  environment.PLAYWRIGHT_BROWSERS_PATH = path.join(temporary, "pinned-browsers");
  baseConfig.webServer = { command: "bazel-owned-server" };
  const firefoxProject = {
    name: "firefox-geode-resize",
    timeout: 90000,
    grep: [/Splash letter/],
    use: { browserName: "firefox", viewport: { width: 1200, height: 750 } },
  };
  const config = evaluateConfig(
    "playwright.firefox-presentation.bazel.config.js",
    {
      "./playwright.bazel.config.js": baseConfig,
      "./playwright.compatibility.config.js": {
        timeout: 30000,
        projects: [
          firefoxProject,
          { name: "webkit-geode-carousel", use: { browserName: "webkit" } },
        ],
      },
    },
    environment,
    temporary,
  );
  assert.equal(config.webServer, baseConfig.webServer);
  assert.equal(config.globalSetup, "./firefox-aqua-setup.js");
  assert.equal(config.use.channel, undefined);
  assert.equal(config.use.launchOptions, undefined);
  assert.equal(config.projects.length, 1);
  const project = config.projects[0];
  assert.equal(project.name, "firefox-geode-resize");
  assert.equal(project.timeout, 90000);
  assert.deepEqual(Array.from(project.grep), firefoxProject.grep);
  assert.equal(project.use.browserName, "firefox");
  assert.equal(project.use.headless, false);
  assert.equal(
    project.use.launchOptions.executablePath,
    path.join(temporary, "firefox-aqua.sh"),
  );
  assert.equal(project.use.launchOptions.timeout, 15000);
});
