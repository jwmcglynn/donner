const baseConfig = require("./playwright.bazel.config.js");

const sharedUse = {
  headless: false,
  ignoreHTTPSErrors: true,
  viewport: { width: 1280, height: 900 },
  deviceScaleFactor: 2,
  screenshot: "off",
  trace: "off",
  video: "off",
};

module.exports = {
  ...baseConfig,
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
        launchOptions: { ...baseConfig.use.launchOptions, timeout: 15000 },
      },
    },
    {
      name: "firefox",
      use: {
        ...sharedUse,
        browserName: "firefox",
        launchOptions: {
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
