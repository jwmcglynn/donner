const baseConfig = require("./playwright.bazel.config.js");
const { browserExecutablePaths } = require("./prepare-browser-archives.js");
const executables = browserExecutablePaths(process.env);

function firefoxProfileEnvironment() {
  if (process.env.DONNER_FIREFOX_PROFILE !== "1") return process.env;
  return {
    ...process.env,
    MOZ_PROFILER_STARTUP: "1",
    MOZ_PROFILER_STARTUP_ENTRIES: "8MiB",
    MOZ_PROFILER_STARTUP_INTERVAL: "2",
    MOZ_PROFILER_STARTUP_FEATURES:
      "js,stackwalk,memory,nativeallocations,processcpu,samplingallthreads",
    MOZ_PROFILER_SHUTDOWN: require("node:path").join(
      process.env.TEST_UNDECLARED_OUTPUTS_DIR,
      "firefox-profile.json",
    ),
  };
}

const sharedUse = {
  headless: process.env.DONNER_BROWSER_HEADLESS === "1",
  ignoreHTTPSErrors: true,
  viewport: { width: 1280, height: 900 },
  deviceScaleFactor: 2,
  screenshot: "only-on-failure",
  trace: "off",
  video: "off",
};

module.exports = {
  ...baseConfig,
  globalSetup: require.resolve("./browser-watchdog-setup.js"),
  testMatch: "browser-responsiveness.perf.ts",
  timeout: 120000,
  globalTimeout: 600000,
  use: sharedUse,
  projects: [
    {
      name: "chromium",
      use: {
        ...baseConfig.use,
        ...sharedUse,
        browserName: "chromium",
        launchOptions: {
          ...baseConfig.use.launchOptions,
          executablePath: executables.chromium,
          timeout: 15000,
        },
      },
    },
    {
      name: "firefox",
      use: {
        ...sharedUse,
        browserName: "firefox",
        launchOptions: {
          executablePath: process.env.DONNER_FIREFOX_GUI_BOOTSTRAP === "1"
            ? require("node:path").join(process.env.TEST_TMPDIR, "firefox-aqua.sh")
            : executables.firefox,
          timeout: 15000,
          env: firefoxProfileEnvironment(),
          firefoxUserPrefs: {
            "dom.webgpu.enabled": true,
            "layout.css.devPixelsPerPx": "2.0",
          },
        },
      },
    },
    {
      name: "chromium-dpr1",
      use: {
        ...sharedUse,
        deviceScaleFactor: 1,
        browserName: "chromium",
        launchOptions: {
          ...baseConfig.use.launchOptions,
          executablePath: executables.chromium,
          timeout: 15000,
        },
      },
    },
    {
      name: "firefox-dpr1",
      use: {
        ...sharedUse,
        deviceScaleFactor: 1,
        browserName: "firefox",
        launchOptions: {
          executablePath: process.env.DONNER_FIREFOX_GUI_BOOTSTRAP === "1"
            ? require("node:path").join(process.env.TEST_TMPDIR, "firefox-aqua.sh")
            : executables.firefox,
          timeout: 15000,
          env: firefoxProfileEnvironment(),
          firefoxUserPrefs: { "dom.webgpu.enabled": true, "layout.css.devPixelsPerPx": "1.0" },
        },
      },
    },
  ],
};
