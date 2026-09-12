const baseConfig = require("./playwright.bazel.config.js");

module.exports = {
  ...baseConfig,
  globalTimeout: 75000,
  timeout: 60000,
  use: {
    ...baseConfig.use,
    launchOptions: {
      ...baseConfig.use.launchOptions,
      timeout: 15000,
    },
  },
};
