const { defineConfig, devices } = require("@playwright/test");

module.exports = defineConfig({
  testDir: ".",
  // The composited-invariant suite samples GPU-composited output every
  // animation frame; under this config's default headless Chromium the
  // rasterizer is SwiftShader, whose CPU cost for the editor's full-canvas
  // passes starves the sampler and fails the suite's usability guard on
  // machines that run the real thing at 110 fps. Both composited suites run on
  // hardware via playwright.composited-chromium.config.js instead.
  testIgnore: ["composited-invariants.spec.ts", "composited-drag-invariants.spec.ts"],
  timeout: 30000,
  use: {
    ...devices["Desktop Chrome"],
    ignoreHTTPSErrors: true,
    launchOptions: {
      args: [
        "--enable-unsafe-webgpu",
        "--ignore-certificate-errors",
      ],
    },
  },
});
