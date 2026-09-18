const baseConfig = require("./playwright.bazel.config.js");
const chromiumConfig = require("./playwright.composited-chromium.config.js");

module.exports = {
  ...baseConfig,
  ...chromiumConfig,
  // The repository default excludes composited specs; this target owns both full suites.
  testIgnore: [],
  use: {
    ...baseConfig.use,
    ...chromiumConfig.use,
    headless: false,
    launchOptions: {
      ...baseConfig.use.launchOptions,
      ...chromiumConfig.use.launchOptions,
      env: baseConfig.use.launchOptions.env,
    },
  },
};
