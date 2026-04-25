import { defineConfig, devices } from "@playwright/test";

// The Bazel wrapper (`run_tests.sh`) starts the `serve_http` binary and sets
// `DONNER_WASM_BASE_URL` to the resolved http://127.0.0.1:<port> root. When
// running tests by hand (`npx playwright test`), you can either:
//   1. Start `bazel run //donner/editor/wasm:serve_http` yourself and export
//      `DONNER_WASM_BASE_URL=http://127.0.0.1:8000` (or whatever port it
//      chose), or
//   2. Rely on the `webServer` block below, which launches the Bazel target
//      in the background and parses its "Local URL:" line.
//
// The `?test=1` query flag is appended by the individual specs — the config
// just exposes `DONNER_WASM_BASE_URL` as `baseURL`.

const baseURL = process.env.DONNER_WASM_BASE_URL || "http://127.0.0.1:8000";
const skipWebServer = process.env.DONNER_WASM_SKIP_WEBSERVER === "1";
const deviceScaleFactor = Number(process.env.DONNER_WASM_DEVICE_SCALE_FACTOR || "1");

export default defineConfig({
  testDir: ".",
  timeout: 60_000,
  expect: { timeout: 15_000 },
  fullyParallel: false,
  workers: 1,
  reporter: process.env.CI ? [["list"], ["github"]] : [["list"]],
  use: {
    baseURL,
    // Chromium's headless mouse event path is what we're actually trying
    // to exercise. Don't switch to Firefox / WebKit without validating that
    // synthesized mouse dispatch flows the same way — emscripten-glfw's
    // handlers are bound with specific event-listener options.
    viewport: { width: 1280, height: 800 },
    ignoreHTTPSErrors: true,
    trace: "retain-on-failure",
    video: "retain-on-failure",
  },
  projects: [
    {
      name: "chromium",
      use: {
        ...devices["Desktop Chrome"],
        deviceScaleFactor,
        launchOptions: {
          args: [
            // Required for local/headless WebGPU runs of --config=editor-wasm-geode.
            "--enable-unsafe-webgpu",
          ],
        },
      },
    },
  ],
  webServer: skipWebServer
    ? undefined
    : {
      // If unset, assume the caller started `serve_http` manually.
      // `DONNER_WASM_SERVE_CMD` is how the Bazel wrapper injects its
      // serve command (runfiles-resolved path).
      command: process.env.DONNER_WASM_SERVE_CMD || "true",
      url: baseURL,
      reuseExistingServer: true,
      timeout: 60_000,
      stdout: "pipe",
      stderr: "pipe",
    },
});
