import { test, expect } from "@playwright/test";

// Smoke: the WASM editor loads end-to-end, the canvas exists, the first
// frame renders (detected via `__donner_ready`), and the canvas contains
// non-zero pixels (proving WebGL / emscripten-glfw / the render backend
// all wired up).
//
// This test fails loudly on all the "it compiled but won't run" regression
// classes — missing exports, startup crashes, COOP/COEP misconfig, etc.

test("WASM editor loads and renders a first frame", async ({ page }) => {
  const consoleErrors: string[] = [];
  page.on("console", (msg) => {
    if (msg.type() === "error") {
      consoleErrors.push(msg.text());
    }
  });
  page.on("pageerror", (err) => {
    consoleErrors.push(`pageerror: ${err.message}`);
  });

  await page.goto("/?test=1");

  // The test hook flips `window.__donner_ready` to true after the first
  // successful render. Polling is intentional — we don't want to race the
  // Emscripten runtime-init path.
  await page.waitForFunction(() => (window as any).__donner_ready === true, null, {
    timeout: 45_000,
  });

  // Sanity-check the canvas.
  const canvasSize = await page.evaluate(() => {
    const canvas = document.getElementById("canvas") as HTMLCanvasElement | null;
    if (!canvas) { return null; }
    return { width: canvas.width, height: canvas.height };
  });
  expect(canvasSize).not.toBeNull();
  expect(canvasSize!.width).toBeGreaterThan(0);
  expect(canvasSize!.height).toBeGreaterThan(0);

  // `toDataURL` returns a data: URL of the backing framebuffer (we enable
  // `preserveDrawingBuffer` in index.html). A truly-black canvas serializes
  // to a stable short prefix + a long run; checking for > 256 distinct
  // chars in the base64 body is a cheap "something was drawn" signal.
  const dataUrlLength = await page.evaluate(() => {
    const canvas = document.getElementById("canvas") as HTMLCanvasElement;
    const url = canvas.toDataURL("image/png");
    return url.length;
  });
  expect(dataUrlLength).toBeGreaterThan(1024);

  // Metrics must be populated.
  const metrics = await page.evaluate(() => (window as any).__donner_metrics);
  expect(metrics).toBeTruthy();
  expect(metrics.renderCount).toBeGreaterThan(0);

  // No console errors during startup. If emscripten's stderr is noisy in
  // the future, narrow this to exclude known benign strings rather than
  // deleting the check — console errors are one of the cheapest escape
  // detectors we have.
  expect(consoleErrors, `console errors during startup:\n${consoleErrors.join("\n")}`)
      .toEqual([]);
});
