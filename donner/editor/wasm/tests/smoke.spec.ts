import { expect, type Page, test } from "@playwright/test";
import { inflateSync } from "node:zlib";

declare global {
  interface Window {
    __donnerBackend?: string;
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

interface CssRegion {
  x: number;
  y: number;
  width: number;
  height: number;
}

interface CanvasColorStats {
  samples: number;
  coloredPixels: number;
  nonBlackPixels: number;
  maxChannel: number;
  region: CssRegion;
}

type WgpuReadbackColorStats = Omit<CanvasColorStats, "region">;

interface PngImage {
  width: number;
  height: number;
  channels: number;
  data: Uint8Array;
}

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

function paethPredictor(left: number, up: number, upLeft: number): number {
  const estimate = left + up - upLeft;
  const leftDistance = Math.abs(estimate - left);
  const upDistance = Math.abs(estimate - up);
  const upLeftDistance = Math.abs(estimate - upLeft);
  if (leftDistance <= upDistance && leftDistance <= upLeftDistance) {
    return left;
  }
  return upDistance <= upLeftDistance ? up : upLeft;
}

function decodePng(buffer: Buffer): PngImage {
  const signature = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
  if (!buffer.subarray(0, signature.length).equals(signature)) {
    throw new Error("screenshot is not a PNG");
  }

  let width = 0;
  let height = 0;
  let channels = 0;
  const idatChunks: Buffer[] = [];
  for (let offset = signature.length; offset < buffer.length;) {
    const chunkLength = buffer.readUInt32BE(offset);
    const type = buffer.toString("ascii", offset + 4, offset + 8);
    const dataOffset = offset + 8;
    const dataEnd = dataOffset + chunkLength;
    const data = buffer.subarray(dataOffset, dataEnd);
    offset = dataEnd + 4;

    if (type === "IHDR") {
      width = data.readUInt32BE(0);
      height = data.readUInt32BE(4);
      const bitDepth = data[8];
      const colorType = data[9];
      const compression = data[10];
      const filter = data[11];
      const interlace = data[12];
      if (
        bitDepth !== 8
        || compression !== 0
        || filter !== 0
        || interlace !== 0
        || (colorType !== 2 && colorType !== 6)
      ) {
        throw new Error(`unsupported PNG format: depth=${bitDepth} color=${colorType}`);
      }
      channels = colorType === 6 ? 4 : 3;
    } else if (type === "IDAT") {
      idatChunks.push(data);
    } else if (type === "IEND") {
      break;
    }
  }

  if (width <= 0 || height <= 0 || channels <= 0 || idatChunks.length === 0) {
    throw new Error("PNG is missing image data");
  }

  const bytesPerRow = width * channels;
  const raw = inflateSync(Buffer.concat(idatChunks));
  const output = new Uint8Array(height * bytesPerRow);
  let sourceOffset = 0;
  for (let y = 0; y < height; ++y) {
    const filter = raw[sourceOffset++];
    const rowOffset = y * bytesPerRow;
    const previousRowOffset = rowOffset - bytesPerRow;
    for (let x = 0; x < bytesPerRow; ++x) {
      const value = raw[sourceOffset++];
      const left = x >= channels ? output[rowOffset + x - channels] : 0;
      const up = y > 0 ? output[previousRowOffset + x] : 0;
      const upLeft = y > 0 && x >= channels ? output[previousRowOffset + x - channels] : 0;
      let reconstructed = value;
      if (filter === 1) {
        reconstructed += left;
      } else if (filter === 2) {
        reconstructed += up;
      } else if (filter === 3) {
        reconstructed += Math.floor((left + up) / 2);
      } else if (filter === 4) {
        reconstructed += paethPredictor(left, up, upLeft);
      } else if (filter !== 0) {
        throw new Error(`unsupported PNG row filter ${filter}`);
      }
      output[rowOffset + x] = reconstructed & 0xff;
    }
  }

  return { width, height, channels, data: output };
}

async function readCanvasColorStats(page: Page, region: CssRegion): Promise<CanvasColorStats> {
  const canvasBox = await page.locator("canvas#canvas").boundingBox();
  if (canvasBox === null) {
    throw new Error("canvas not found");
  }

  const x = Math.max(0, region.x);
  const y = Math.max(0, region.y);
  const width = Math.max(1, Math.min(region.width, canvasBox.width - x));
  const height = Math.max(1, Math.min(region.height, canvasBox.height - y));
  const clip = {
    x: canvasBox.x + x,
    y: canvasBox.y + y,
    width,
    height,
  };
  const image = decodePng(await page.screenshot({ clip }));

  let coloredPixels = 0;
  let nonBlackPixels = 0;
  let maxChannel = 0;
  for (let offset = 0; offset < image.data.length; offset += image.channels) {
    const red = image.data[offset];
    const green = image.data[offset + 1];
    const blue = image.data[offset + 2];
    const alpha = image.channels === 4 ? image.data[offset + 3] : 255;
    const maxRgb = Math.max(red, green, blue);
    const minRgb = Math.min(red, green, blue);
    maxChannel = Math.max(maxChannel, maxRgb);
    if (alpha > 0 && maxRgb > 12) {
      nonBlackPixels += 1;
    }
    if (alpha > 0 && maxRgb > 50 && maxRgb - minRgb > 20) {
      coloredPixels += 1;
    }
  }

  return {
    samples: image.width * image.height,
    coloredPixels,
    nonBlackPixels,
    maxChannel,
    region: { x, y, width: image.width, height: image.height },
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
