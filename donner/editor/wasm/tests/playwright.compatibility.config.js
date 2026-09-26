const { defineConfig, devices } = require("@playwright/test");

module.exports = defineConfig({
  testDir: ".",
  testMatch: ["smoke.spec.ts", "browser-presentation-regression.spec.ts"],
  timeout: 30000,
  workers: 1,
  projects: [
    {
      name: "firefox-geode-resize",
      // Gecko's first worker-presented WebGPU frame can arrive well after the worker finishes.
      // Keep the visual pixel assertions intact while budgeting boot and first presentation.
      timeout: 90000,
      grep: [
        /Geode Wasm View overlays render tile metadata and sparse Slug triangle edges/,
        /Firefox keeps Basic Shapes resize pixels and outline synchronized/,
        /Firefox keeps the dragged shape and its selection outline in every drag frame/,
        /Firefox never exposes the checkerboard while dragging a Splash letter/,
        /the surface frame probe reports canvas work submitted after its task ended/,
        /Firefox renders every visible Splash layer thumbnail/,
        /Firefox hands a blocked thumbnail renderer to a foreground sample load/,
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
