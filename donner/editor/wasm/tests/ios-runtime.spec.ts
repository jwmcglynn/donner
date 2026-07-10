import { expect, type Page, test } from "@playwright/test";
import { readCanvasColorStats } from "./canvas-color-stats";

declare global {
  interface Window {
    __donnerBackend?: string;
    __donnerCanStartWasm?: boolean;
  }
}

interface RuntimeOptions {
  disableWebGpu?: boolean;
  renderer?: "tiny_skia";
}

const kFatalRuntimePattern =
  /Aborted|Assertion failed|RuntimeError|Pthread .* sent an error|WebAssembly runtime unavailable|Unhandled promise rejection/i;

async function startRuntime(page: Page, options: RuntimeOptions = {}) {
  if (options.disableWebGpu) {
    await page.addInitScript(() => {
      Object.defineProperty(Navigator.prototype, "gpu", {
        configurable: true,
        value: undefined,
      });
    });
  }

  const baseUrl = process.env.DONNER_WASM_BASE_URL || "http://127.0.0.1:8000";
  const url = new URL(baseUrl);
  if (options.renderer) {
    url.searchParams.set("renderer", options.renderer);
  }
  const fatalMessages: string[] = [];

  page.on("console", (message) => {
    if (kFatalRuntimePattern.test(message.text())) {
      fatalMessages.push(`[console:${message.type()}] ${message.text()}`);
    }
  });
  page.on("pageerror", (error) => {
    fatalMessages.push(`[pageerror] ${error.message}`);
  });

  await page.goto(url.toString(), { waitUntil: "domcontentloaded" });
  await expect(page.locator("canvas#canvas")).toBeVisible();
  await expect(page.locator("#status")).toBeHidden({ timeout: 30000 });
  await page.waitForTimeout(2000);

  const capabilities = await page.evaluate(() => {
    const probe = document.createElement("canvas");
    const webGl2 = probe.getContext("webgl2");
    webGl2?.getExtension("WEBGL_lose_context")?.loseContext();
    const editorCanvas = document.getElementById("canvas");
    return {
      backend: window.__donnerBackend,
      canStartWasm: window.__donnerCanStartWasm,
      crossOriginIsolated,
      hasNavigatorGpu: Boolean(navigator.gpu),
      hasSharedArrayBuffer: typeof SharedArrayBuffer !== "undefined",
      hasWebGl2: Boolean(webGl2),
      isSecureContext,
      search: window.location.search,
      userAgent: navigator.userAgent,
      viewport: {
        width: editorCanvas?.clientWidth || 0,
        height: editorCanvas?.clientHeight || 0,
      },
    };
  });

  expect(capabilities.userAgent).toContain("iPhone");
  expect(capabilities.isSecureContext).toBe(true);
  expect(capabilities.crossOriginIsolated).toBe(true);
  expect(capabilities.hasSharedArrayBuffer).toBe(true);
  expect(capabilities.canStartWasm).toBe(true);
  expect(capabilities.hasWebGl2).toBe(true);
  expect(capabilities.viewport.width).toBeGreaterThan(0);
  expect(capabilities.viewport.height).toBeGreaterThan(0);
  expect(fatalMessages).toEqual([]);

  return { capabilities, fatalMessages };
}

test("iPhone-profile WebKit automatically falls back without WebGPU", async ({ page }) => {
  const { capabilities, fatalMessages } = await startRuntime(page, { disableWebGpu: true });

  expect(capabilities.search).not.toContain("renderer=");
  expect(capabilities.hasNavigatorGpu).toBe(false);
  expect(capabilities.backend).toBe("tiny_skia");
  await page.waitForTimeout(2000);
  expect(fatalMessages).toEqual([]);
});

test.describe("WebKit TinySkia presentation", () => {
  // Mobile layout is intentionally separate work. Use the desktop calibration
  // at DPR 1 to prove the WebKit renderer path presents document pixels, while
  // the companion test retains the real iPhone viewport and DPR for startup.
  test.use({ deviceScaleFactor: 1, viewport: { width: 1600, height: 900 } });

  test("presents document pixels", async ({ page }) => {
    const { capabilities, fatalMessages } = await startRuntime(page, { renderer: "tiny_skia" });
    const stats = await readCanvasColorStats(page, {
      x: 580,
      y: 80,
      width: 580,
      height: 680,
    });

    expect(capabilities.backend).toBe("tiny_skia");
    expect(stats.nonBlackPixels, JSON.stringify(stats)).toBeGreaterThan(5000);
    expect(stats.coloredPixels, JSON.stringify(stats)).toBeGreaterThan(1000);
    expect(stats.maxChannel, JSON.stringify(stats)).toBeGreaterThan(100);
    await page.waitForTimeout(1000);
    expect(fatalMessages).toEqual([]);
  });
});
