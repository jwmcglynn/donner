const { defineConfig, devices } = require("@playwright/test");

module.exports = defineConfig({
  testDir: ".",
  timeout: 30000,
  use: {
    ...devices["Desktop Safari"],
    ignoreHTTPSErrors: true,
  },
});
