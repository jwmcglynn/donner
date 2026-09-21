const baseConfig = require("./playwright.bazel.config.js");

// A cold Chrome for Testing launch can exceed 15s while the browser is healthy
// (a first exec pays macOS code-sign validation), so restore Playwright's 30s
// launch default and let the suite cap cover a slow launch plus the 60s test.
module.exports = {
  ...baseConfig,
  globalTimeout: 120000,
  timeout: 60000,
  use: {
    ...baseConfig.use,
    launchOptions: {
      ...baseConfig.use.launchOptions,
      timeout: 30000,
    },
  },
};
