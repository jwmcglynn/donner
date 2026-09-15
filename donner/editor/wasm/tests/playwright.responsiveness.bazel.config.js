const baseConfig = require("./playwright.bazel.config.js");
const { browserExecutablePaths } = require("./prepare-browser-archives.js");
const executables = browserExecutablePaths(process.env);

const sharedUse = {
  headless: false,
  ignoreHTTPSErrors: true,
  viewport: { width: 1280, height: 900 },
  deviceScaleFactor: 2,
  screenshot: "only-on-failure",
  trace: "off",
  video: "off",
};

module.exports = {
  ...baseConfig,
  globalSetup: require.resolve("./prepare-browser-archives.js"),
  testMatch: "browser-responsiveness.perf.ts",
  timeout: 90000,
  globalTimeout: 240000,
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
          executablePath: executables.firefox,
          timeout: 15000,
          firefoxUserPrefs: {
            "dom.webgpu.enabled": true,
            "layout.css.devPixelsPerPx": "2.0",
          },
        },
      },
    },
  ],
};
