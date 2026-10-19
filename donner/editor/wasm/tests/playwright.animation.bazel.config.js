const base = require("./playwright.bazel.config.js");
const { devices } = require("@playwright/test");
const { browserExecutablePaths } = require("./prepare-browser-archives.js");
const { firefoxLaunchEnvironment } = require("./firefox-launch-env.cjs");
const executables = browserExecutablePaths(process.env);

module.exports = {
  ...base,
  use: {},
  globalSetup: require.resolve("./prepare-browser-archives.js"),
  projects: [
    {
      name: "chromium-animation",
      use: {
        ...base.use,
        launchOptions: { ...base.use.launchOptions, executablePath: executables.chromium },
      },
    },
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
          env: firefoxLaunchEnvironment(process.env),
          firefoxUserPrefs: { "dom.webgpu.enabled": true },
        },
      },
    },
  ],
};
