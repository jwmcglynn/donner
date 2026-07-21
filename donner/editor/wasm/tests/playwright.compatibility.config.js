const { defineConfig, devices } = require("@playwright/test");

module.exports = defineConfig({
  testDir: ".",
  testMatch: ["smoke.spec.ts", "browser-presentation-regression.spec.ts"],
  timeout: 30000,
  workers: 1,
  projects: [
    {
      name: "firefox-geode-resize",
      grep: [
        /Geode Wasm View overlays render tile metadata and sparse Slug triangle edges/,
        /Firefox keeps Basic Shapes resize pixels and outline synchronized/,
        /Firefox presents every accepted drag epoch on one stable surface/,
        /Firefox never exposes the checkerboard while dragging a Splash letter/,
        /Firefox bakes the Splash letter and overlay into one accepted surface epoch/,
      ],
      use: {
        ...devices["Desktop Firefox"],
        browserName: "firefox",
        ignoreHTTPSErrors: true,
        screenshot: "only-on-failure",
        viewport: { width: 1200, height: 750 },
      },
    },
    {
      name: "webkit-tiny-skia-carousel",
      grep: [
        /carousel loads Basic Shapes on the first interactive frame/,
        /WebKit TinySkia survives a burst of drag wakeups without fatal errors/,
      ],
      use: {
        ...devices["Desktop Safari"],
        browserName: "webkit",
        ignoreHTTPSErrors: true,
      },
    },
  ],
});
