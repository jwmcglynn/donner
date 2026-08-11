const { defineConfig, devices } = require("@playwright/test");

// Hardware-Chromium lane for the composited-output invariant suite.
//
// The default config's headless Chromium rasterizes with SwiftShader, which
// cannot sustain the editor's full-canvas surface passes at frame rate; the
// per-rAF sampler then starves and the suite's usability guard fires on
// machines where real Chromium runs the same gestures at 110 fps. This config
// runs the full Chromium build on the platform GPU (Metal via ANGLE on
// macOS), which is also the only configuration whose numbers mean anything
// for composited-output behavior.
module.exports = defineConfig({
  testDir: ".",
  testMatch: ["composited-invariants.spec.ts", "composited-drag-invariants.spec.ts"],
  timeout: 120000,
  use: {
    ...devices["Desktop Chrome"],
    channel: "chromium",
    // Headed, and not negotiable: this package's renderer runs WebGPU inside a
    // worker, and the headless shell does not create a worker GPU device here
    // (`__donnerWorkerRuntimeStats.ready` stays false and the editor never
    // finishes booting, so every test in the lane fails in `openEditor`).
    // Headless also defeats the point of the lane, which is to measure
    // composited output on the platform GPU.
    headless: false,
    ignoreHTTPSErrors: true,
    launchOptions: {
      args: [
        "--enable-unsafe-webgpu",
        "--ignore-certificate-errors",
        "--use-angle=metal",
      ],
    },
  },
});
