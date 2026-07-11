const { defineConfig, devices } = require("@playwright/test");

module.exports = defineConfig({
  testDir: ".",
  testMatch: "ios-runtime.spec.ts",
  timeout: 45000,
  use: {
    ...devices["iPhone 15 Pro"],
    browserName: "webkit",
    ignoreHTTPSErrors: true,
  },
});
