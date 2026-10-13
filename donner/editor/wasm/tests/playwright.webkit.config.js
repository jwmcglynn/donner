const { defineConfig, devices } = require("@playwright/test");
const { stallGuardFor } = require("./test-timing.cjs");

module.exports = defineConfig({
  testDir: ".",
  timeout: 30000,
  // Web-first assertions wait for a state; bound them by the shared stall guard, not a latency.
  expect: { timeout: stallGuardFor(30000) },
  use: {
    ...devices["Desktop Safari"],
    ignoreHTTPSErrors: true,
  },
});
