const { defineConfig, devices } = require("@playwright/test");

module.exports = defineConfig({
  testDir: ".",
  testMatch: ["smoke.spec.ts", "browser-presentation-regression.spec.ts"],
  timeout: 30000,
  workers: 1,
  projects: [
    {
      name: "firefox-geode-resize",
      repeatEach: 100,
      grep: [/Firefox never exposes the checkerboard while dragging a Splash letter/],
      use: {
        ...devices["Desktop Firefox"],
        browserName: "firefox",
        ignoreHTTPSErrors: true,
        screenshot: "only-on-failure",
        viewport: { width: 1200, height: 750 },
      },
    },
    {
      name: "webkit-geode-carousel",
      grep: [
        /carousel loads Basic Shapes on the first interactive frame/,
        /WebKit Geode survives a burst of drag wakeups without fatal errors/,
      ],
      use: {
        ...devices["Desktop Safari"],
        browserName: "webkit",
        ignoreHTTPSErrors: true,
      },
    },
  ],
});
