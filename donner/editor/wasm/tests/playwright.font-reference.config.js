const path = require("node:path");
const { firefoxLaunchEnvironment } = require("./firefox-launch-env.cjs");
const { stallGuardFor } = require("./test-timing.cjs");

module.exports = {
  testDir: __dirname,
  testMatch: "font-reference.spec.ts",
  timeout: 10000,
  // Web-first assertions wait for a state; bound them by the shared stall guard, not a latency.
  expect: { timeout: stallGuardFor(10000) },
  retries: 0,
  workers: 1,
  reporter: "list",
  outputDir: path.join(process.env.TEST_UNDECLARED_OUTPUTS_DIR, "playwright"),
  use: { headless: true, viewport: { width: 500, height: 500 }, deviceScaleFactor: 1 },
  projects: [
    { name: "chromium", use: { browserName: "chromium", channel: "chromium" } },
    {
      name: "firefox",
      use: {
        browserName: "firefox",
        headless: false,
        launchOptions: { env: firefoxLaunchEnvironment(process.env), timeout: 15000 },
      },
    },
  ],
};
