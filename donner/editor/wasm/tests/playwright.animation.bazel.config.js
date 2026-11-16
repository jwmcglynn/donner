const base = require("./playwright.bazel.config.js");
const { devices } = require("@playwright/test");
const { browserExecutablePaths } = require("./prepare-browser-archives.js");
const { firefoxLaunchOptions } = require("./firefox-launch-env.cjs");
const { kBrowserLaunchTimeoutMs } = require("./test-timing.cjs");
const executables = browserExecutablePaths(process.env);

// Firefox only: the Editor WASM workflow's chromium-default lane runs every Chromium case of
// smoke.spec.ts, these included, so this lane runs the cases no other lane runs in Firefox.
module.exports = {
  ...base,
  use: {},
  // The archives the global setup extracts before probing one launch of each project's
  // browser (prepare-browser-archives.js, browser-launch-probe.js): only the browser run here.
  metadata: { pinnedBrowsers: ["firefox"] },
  globalSetup: require.resolve("./prepare-browser-archives.js"),
  projects: [
    {
      name: "firefox-animation",
      timeout: 90000,
      use: {
        ...devices["Desktop Firefox"],
        browserName: "firefox",
        headless: false,
        ignoreHTTPSErrors: true,
        launchOptions: {
          executablePath: executables.firefox,
          timeout: kBrowserLaunchTimeoutMs,
          ...firefoxLaunchOptions(process.env),
          firefoxUserPrefs: { "dom.webgpu.enabled": true },
        },
      },
    },
  ],
};
