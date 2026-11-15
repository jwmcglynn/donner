const base = require("./playwright.bazel.config.js");
const { devices } = require("@playwright/test");
const { browserExecutablePaths } = require("./prepare-browser-archives.js");
const { firefoxLaunchOptions } = require("./firefox-launch-env.cjs");
const executables = browserExecutablePaths(process.env);

// Firefox only: the Editor WASM workflow's chromium-default lane runs every Chromium case of
// smoke.spec.ts, these included, so this lane runs the cases no other lane runs in Firefox.
module.exports = {
  ...base,
  use: {},
  // The archives prepare-browser-archives.js extracts before any case: only the browser run here.
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
          timeout: 30000,
          ...firefoxLaunchOptions(process.env),
          firefoxUserPrefs: { "dom.webgpu.enabled": true },
        },
      },
    },
  ],
};
