const { defineConfig, devices } = require("@playwright/test");

module.exports = defineConfig({
  testDir: ".",
  testIgnore: "ios-runtime.spec.ts",
  timeout: 30000,
  use: {
    ...devices["Desktop Safari"],
    ignoreHTTPSErrors: true,
  },
});
