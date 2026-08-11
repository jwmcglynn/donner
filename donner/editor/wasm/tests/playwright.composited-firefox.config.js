const { defineConfig, devices } = require("@playwright/test");

// Gecko lane for the composited-output invariant suite.
//
// Chromium coverage for both composited specs comes from
// `playwright.composited-chromium.config.js`. The default config
// (`playwright.config.js`) deliberately ignores them - its headless Chromium
// rasterizes with SwiftShader - so this config is not a duplicate of anything
// the default glob picks up; it exists to run the same specs on a second
// engine.
//
// It is a separate config rather than another project in
// `playwright.compatibility.config.js` because that config pins an explicit
// `testMatch` list and per-project `grep` filters for the resize and carousel
// regressions; this suite wants the whole spec on one engine, with a longer
// per-test timeout. Playwright's Firefox runs the Wasm editor roughly an order
// of magnitude slower than Chromium, and these tests deliberately sample every
// animation frame across a multi-second gesture.
//
// The correctness invariants (epoch atomicity, backing stability, pan motion,
// pinch parity) hold on Gecko unchanged. The one timing-sensitive bound, the
// black-frame fraction, is widened inside the spec for this engine.
module.exports = defineConfig({
  testDir: ".",
  testMatch: ["composited-invariants.spec.ts", "composited-drag-invariants.spec.ts"],
  timeout: 180000,
  workers: 1,
  projects: [
    {
      name: "firefox-composited-invariants",
      use: {
        ...devices["Desktop Firefox"],
        browserName: "firefox",
        ignoreHTTPSErrors: true,
        screenshot: "only-on-failure",
        viewport: { width: 1600, height: 900 },
      },
    },
  ],
});
