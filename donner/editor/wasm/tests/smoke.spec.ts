import { expect, type Page, test } from "@playwright/test";

declare global {
  interface Window {
    __donnerLastScrollEvent?: {
      zoomModifierHeld?: boolean;
      yoffset?: number;
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

const kFatalRuntimePattern =
  /Aborted|Assertion failed|RuntimeError|Pthread .* sent an error|getJsObject/i;
const kSourcePaneWidth = 560;
const kRightPaneWidth = 420;

async function readCanvasColorStats(page: Page, region: CssRegion): Promise<CanvasColorStats> {
  return page.evaluate((cssRegion) => {
    const canvas = document.getElementById("canvas") as HTMLCanvasElement | null;
    if (!canvas) {
      throw new Error("canvas not found");
    }

    const gl = canvas.getContext("webgl2") ?? canvas.getContext("webgl");
    if (!gl) {
      throw new Error("WebGL context not available");
    }

    const cssWidth = canvas.clientWidth;
    const cssHeight = canvas.clientHeight;
    const scaleX = gl.drawingBufferWidth / cssWidth;
    const scaleY = gl.drawingBufferHeight / cssHeight;
    const x = Math.max(0, Math.floor(cssRegion.x * scaleX));
    const y = Math.max(0, Math.floor((cssHeight - cssRegion.y - cssRegion.height) * scaleY));
    const width = Math.max(
      1,
      Math.min(gl.drawingBufferWidth - x, Math.floor(cssRegion.width * scaleX)),
    );
    const height = Math.max(
      1,
      Math.min(gl.drawingBufferHeight - y, Math.floor(cssRegion.height * scaleY)),
    );
    const pixels = new Uint8Array(width * height * 4);
    const framebufferBinding = gl.getParameter(gl.FRAMEBUFFER_BINDING) as WebGLFramebuffer | null;
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.readPixels(x, y, width, height, gl.RGBA, gl.UNSIGNED_BYTE, pixels);
    gl.bindFramebuffer(gl.FRAMEBUFFER, framebufferBinding);

    let coloredPixels = 0;
    let nonBlackPixels = 0;
    let maxChannel = 0;
    for (let offset = 0; offset < pixels.length; offset += 4) {
      const red = pixels[offset];
      const green = pixels[offset + 1];
      const blue = pixels[offset + 2];
      const alpha = pixels[offset + 3];
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
      samples: width * height,
      coloredPixels,
      nonBlackPixels,
      maxChannel,
      region: { x, y, width, height },
    };
  }, region);
}

async function readRenderPaneColorStats(page: Page): Promise<CanvasColorStats> {
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
  const region = await page.evaluate(({ rightPaneWidth }) => {
    const canvas = document.getElementById("canvas") as HTMLCanvasElement | null;
    if (!canvas) {
      throw new Error("canvas not found");
    }

    return {
      x: canvas.clientWidth - rightPaneWidth + 8,
      y: canvas.clientHeight * 0.72,
      width: 90,
      height: canvas.clientHeight * 0.24,
    };
  }, { rightPaneWidth: kRightPaneWidth });

  return readCanvasColorStats(page, region);
}

async function openEditor(page: Page): Promise<string[]> {
  const baseUrl = process.env.DONNER_WASM_BASE_URL || "http://127.0.0.1:8000";
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

  await page.goto(baseUrl, { waitUntil: "domcontentloaded" });
  const hasWebGpu = await page.evaluate(() => "gpu" in navigator);
  test.skip(!hasWebGpu, "Browser does not expose navigator.gpu");

  await expect(page.locator("canvas#canvas")).toBeVisible();
  await expect(page.locator("#status")).toBeHidden({ timeout: 20000 });
  await page.waitForTimeout(2000);

  return fatalMessages;
}

test("wasm editor starts without runtime abort", async ({ page }) => {
  const fatalMessages = await openEditor(page);

  expect(fatalMessages).toEqual([]);
});

test("wasm editor renders colored document pixels", async ({ page }) => {
  const fatalMessages = await openEditor(page);
  await expect
    .poll(async () => {
      const stats = await readRenderPaneColorStats(page);
      return stats.nonBlackPixels > 1000 && stats.coloredPixels > 500 && stats.maxChannel > 80;
    }, {
      message: "expected non-black, colored pixels in the render pane",
      timeout: 20000,
    })
    .toBe(true);

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
