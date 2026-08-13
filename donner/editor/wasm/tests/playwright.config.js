const { defineConfig, devices } = require("@playwright/test");

// The editor is one canvas transferred to a worker that presents into it with
// WebGPU. Chromium's default headless GL path never composites that
// OffscreenCanvas swapchain into the surface `page.screenshot()` captures, so
// every pixel probe in this suite reads the bare page background: the document
// claims cannot fail for the right reason, and the ones that measure the cost
// of presenting are measuring a presentation the compositor never did.
// Routing GL through ANGLE puts the presented canvas into the captured
// surface.
//
// Linux keeps the arguments it already had. Its lane pins an explicit
// GPU/WebGPU configuration of its own (see kLinuxGeodeLaunchArgs in
// smoke.spec.ts), and this file cannot be validated against it from a macOS
// host, so the change stays where it was verified.
const kAngleCompositorArgs = process.platform === "linux" ? [] : ["--use-gl=angle"];

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
        ...kAngleCompositorArgs,
      ],
    },
  },
});
