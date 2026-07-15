import { expect, type Page, test } from "@playwright/test";
import { type CanvasColorStats, type CssRegion, readCanvasColorStats } from "./canvas-color-stats";

declare global {
  interface Window {
    __donnerBackend?: string;
    __donnerCanStartWasm?: boolean;
    __donnerLastScrollEvent?: {
      zoomModifierHeld?: boolean;
      yoffset?: number;
    };
    __donnerWgpuReadbackStats?: {
      frame: number;
      renderPane: WgpuReadbackColorStats;
      layerPreview: WgpuReadbackColorStats;
    };
  }
}

type WgpuReadbackColorStats = Omit<CanvasColorStats, "region">;

interface OpenEditorOptions {
  wgpuReadbackStats?: boolean;
}

const kFatalRuntimePattern =
  /Aborted|Assertion failed|RuntimeError|Pthread .* sent an error|getJsObject|No available adapters|WebGPU on Linux requires|WebGPU adapter (?:request )?(?:failed|unavailable)/i;
const kSourcePaneWidth = 560;
const kRightPaneWidth = 420;

// Which renderer backend the served package was built with. The Geode/WebGPU
// lane publishes window.__donnerWgpuReadbackStats (its swapchain pixels never
// reach a headless screenshot); the tiny_skia software lane draws into the
// WebGL canvas, so the screenshot fallback carries real pixels. Default to
// "geode" so existing invocations keep their behavior.
const kBackend = (process.env.DONNER_WASM_BACKEND || "geode").toLowerCase();
const kRequireWebGpu = process.env.DONNER_WASM_REQUIRE_WEBGPU === "1";
// Match Chromium's webgpu-swiftshader test configuration. In particular, do
// not force the browser compositor onto Vulkan: Dawn selects SwiftShader for
// WebGPU independently, while Chromium keeps a display path Xvfb can present.
const kLinuxGeodeLaunchArgs = [
  "--enable-unsafe-webgpu",
  "--use-webgpu-adapter=swiftshader",
  "--enable-dawn-features=allow_unsafe_apis",
  "--disable-dawn-features=use_dxc",
  "--enable-webgpu-developer-features",
  "--use-gpu-in-tests",
  "--enable-accelerated-2d-canvas",
];

// Pin the viewport to the native editor calibration size (1600x900). The
// sample regions in readRenderPaneColorStats / readLayerPreviewColorStats use
// the same CSS geometry the native EditorWindow uses to compute readback stats
// (source pane 560px, right pane 420px, layer preview at 0.72h..0.96h), so the
// browser canvas must match that window size for the regions to land on the
// document render and the layer thumbnails.
test.use({
  viewport: { width: 1600, height: 900 },
  ...(process.platform === "linux" && kBackend === "geode"
    ? { launchOptions: { args: kLinuxGeodeLaunchArgs } }
    : {}),
});

async function readWebGpuDiagnostics(page: Page) {
  const browser = await page.evaluate(async () => {
    const gpu = navigator.gpu;
    let fallbackAdapterAvailable = false;
    let adapterRequestError: string | null = null;
    if (gpu) {
      try {
        fallbackAdapterAvailable =
          (await gpu.requestAdapter({ forceFallbackAdapter: true })) !== null;
      } catch (error) {
        adapterRequestError = String(error);
      }
    }

    return {
      isSecureContext,
      crossOriginIsolated,
      hasSharedArrayBuffer: typeof SharedArrayBuffer !== "undefined",
      hasNavigatorGpu: Boolean(gpu),
      fallbackAdapterAvailable,
      adapterRequestError,
      selectedBackend: window.__donnerBackend,
      userAgent: navigator.userAgent,
    };
  });

  return {
    ...browser,
    backend: kBackend,
    platform: process.platform,
    launchArgs: process.platform === "linux" && kBackend === "geode" ? kLinuxGeodeLaunchArgs : [],
  };
}

async function readRenderPaneColorStats(page: Page): Promise<CanvasColorStats> {
  const wgpuStats = await page.evaluate(() => window.__donnerWgpuReadbackStats?.renderPane);
  if (wgpuStats) {
    return { ...wgpuStats, region: { x: 0, y: 0, width: 0, height: 0 } };
  }

  const region = await page.evaluate(({ sourcePaneWidth, rightPaneWidth }) => {
    const canvas = document.getElementById("canvas") as HTMLCanvasElement | null;
    if (!canvas) {
      throw new Error("canvas not found");
    }

    let x = sourcePaneWidth + 20;
    let width = canvas.clientWidth - sourcePaneWidth - rightPaneWidth - 40;
    if (width <= 0) {
      x = canvas.clientWidth * 0.35;
      width = canvas.clientWidth * 0.3;
    }

    return {
      x,
      y: 80,
      width,
      height: Math.max(1, canvas.clientHeight - 220),
    };
  }, { sourcePaneWidth: kSourcePaneWidth, rightPaneWidth: kRightPaneWidth });

  return readCanvasColorStats(page, region);
}

async function readLayerPreviewColorStats(page: Page): Promise<CanvasColorStats> {
  const wgpuStats = await page.evaluate(() => window.__donnerWgpuReadbackStats?.layerPreview);
  if (wgpuStats) {
    return { ...wgpuStats, region: { x: 0, y: 0, width: 0, height: 0 } };
  }

  const region = await page.evaluate(({ rightPaneWidth }) => {
    const canvas = document.getElementById("canvas") as HTMLCanvasElement | null;
    if (!canvas) {
      throw new Error("canvas not found");
    }

    // The Layers panel thumbnail swatches sit in a narrow column at the left
    // edge of the right sidebar, just inside its border. In the software
    // (tiny_skia) screenshot lane the layers list renders at the TOP of the
    // sidebar (below the menu bar), so scan the upper portion of that column.
    // The Geode lane never reaches this branch: it returns the natively
    // published __donnerWgpuReadbackStats above.
    return {
      x: canvas.clientWidth - rightPaneWidth + 8,
      y: Math.max(1, canvas.clientHeight * 0.05),
      width: 90,
      height: Math.max(1, canvas.clientHeight * 0.42),
    };
  }, { rightPaneWidth: kRightPaneWidth });

  return readCanvasColorStats(page, region);
}

async function openEditor(page: Page, options: OpenEditorOptions = {}): Promise<string[]> {
  const baseUrl = process.env.DONNER_WASM_BASE_URL || "http://127.0.0.1:8000";
  const url = new URL(baseUrl);
  if (options.wgpuReadbackStats !== false) {
    url.searchParams.set("wgpuReadbackStats", "1");
  }
  const fatalMessages: string[] = [];

  page.on("console", (message) => {
    const text = message.text();
    if (kFatalRuntimePattern.test(text)) {
      fatalMessages.push(`[console:${message.type()}] ${text}`);
    }
  });
  page.on("pageerror", (error) => {
    fatalMessages.push(`[pageerror] ${error.message}`);
  });

  await page.goto(url.toString(), { waitUntil: "domcontentloaded" });
  await expect
    .poll(async () => {
      try {
        return await page.evaluate(() => window.__donnerCanStartWasm === true);
      } catch {
        return false;
      }
    }, {
      message: "expected browser capabilities to permit Wasm startup",
      timeout: 20000,
    })
    .toBe(true);
  // The tiny_skia software lane renders through WebGL and does not require
  // WebGPU, so only gate the Geode lane on navigator.gpu.
  if (kBackend !== "tiny_skia") {
    const hasWebGpu = await page.evaluate(() => "gpu" in navigator);
    if (!hasWebGpu && kRequireWebGpu) {
      throw new Error("Geode smoke suite requires WebGPU, but navigator.gpu is unavailable");
    }
    test.skip(!hasWebGpu, "Browser does not expose navigator.gpu");
  }

  await expect(page.locator("canvas#canvas")).toBeVisible();
  await expect(page.locator("#status")).toBeHidden({ timeout: 20000 });
  const selectedBackend = await page.evaluate(() => window.__donnerBackend);
  if (selectedBackend !== "packaged") {
    expect(selectedBackend).toBe(kBackend);
  }
  await page.waitForTimeout(2000);

  return fatalMessages;
}

test("wasm editor starts without runtime abort", async ({ page }) => {
  const fatalMessages = await openEditor(page);

  expect(fatalMessages).toEqual([]);
});

test("production Geode wasm presents visible editor pixels", async ({ page }) => {
  test.skip(kBackend !== "geode", "production WebGPU presentation is Geode-specific");
  const fatalMessages = await openEditor(page, { wgpuReadbackStats: false });
  const gpuDiagnostics = await readWebGpuDiagnostics(page);
  console.log(`browser-gpu-diagnostics=${JSON.stringify(gpuDiagnostics)}`);
  await test.info().attach("browser-gpu-diagnostics", {
    body: JSON.stringify(gpuDiagnostics, null, 2),
    contentType: "application/json",
  });
  const canvasBox = await page.locator("canvas#canvas").boundingBox();
  expect(canvasBox).not.toBeNull();
  if (canvasBox === null) {
    return;
  }

  const fullCanvasRegion = { x: 0, y: 0, width: canvasBox.width, height: canvasBox.height };
  try {
    await expect
      .poll(async () => (await readCanvasColorStats(page, fullCanvasRegion)).coloredPixels, {
        message: "expected the production WebGPU canvas to present colored document pixels",
        timeout: 20000,
      })
      .toBeGreaterThan(1000);
  } catch (error) {
    console.log(
      `full-canvas-stats=${JSON.stringify(await readCanvasColorStats(page, fullCanvasRegion))}`,
    );
    throw error;
  }
  expect(fatalMessages).toEqual([]);
});

test("wasm editor renders colored document pixels", async ({ page }) => {
  const fatalMessages = await openEditor(page);
  try {
    await expect
      .poll(async () => {
        const stats = await readRenderPaneColorStats(page);
        return stats.nonBlackPixels > 1000 && stats.coloredPixels > 500 && stats.maxChannel > 80;
      }, {
        message: "expected non-black, colored pixels in the render pane",
        timeout: 20000,
      })
      .toBe(true);
  } catch (error) {
    console.log(`render-pane-stats=${JSON.stringify(await readRenderPaneColorStats(page))}`);
    throw error;
  }

  expect(fatalMessages).toEqual([]);
});

test("wasm editor renders layer panel thumbnails", async ({ page }) => {
  const fatalMessages = await openEditor(page);
  await expect
    .poll(async () => {
      const stats = await readLayerPreviewColorStats(page);
      return stats.coloredPixels > 100 && stats.maxChannel > 80;
    }, {
      message: "expected colored thumbnail pixels in the Layers panel preview column",
      timeout: 20000,
    })
    .toBe(true);

  expect(fatalMessages).toEqual([]);
});

test("WASM trackpad pinch wheel reaches editor as zoom gesture", async ({ page }) => {
  const fatalMessages = await openEditor(page);
  const canvas = page.locator("canvas#canvas");
  const bounds = await canvas.boundingBox();
  expect(bounds).not.toBeNull();
  if (bounds === null) {
    return;
  }

  const center = {
    x: bounds.x + bounds.width * 0.5,
    y: bounds.y + bounds.height * 0.5,
  };
  await page.mouse.move(center.x, center.y);
  await page.evaluate(() => {
    window.__donnerLastScrollEvent = undefined;
  });

  await page.evaluate(({ x, y }) => {
    const target = document.getElementById("canvas");
    if (!target) {
      throw new Error("canvas not found");
    }

    target.dispatchEvent(
      new WheelEvent("wheel", {
        bubbles: true,
        cancelable: true,
        clientX: x,
        clientY: y,
        ctrlKey: true,
        deltaMode: WheelEvent.DOM_DELTA_PIXEL,
        deltaY: -100,
      }),
    );
  }, center);

  await expect
    .poll(async () => page.evaluate(() => window.__donnerLastScrollEvent?.zoomModifierHeld))
    .toBe(true);
  await expect
    .poll(async () => page.evaluate(() => window.__donnerLastScrollEvent?.yoffset))
    .toBeGreaterThan(0);
  expect(fatalMessages).toEqual([]);
});
