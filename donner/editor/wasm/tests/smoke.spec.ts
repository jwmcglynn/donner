import { expect, type Page, test } from "@playwright/test";
import { inflateSync } from "node:zlib";

type ImageRect = {
  left: number;
  top: number;
  width: number;
  height: number;
};

function isExpectedStartupStderr(text: string): boolean {
  return text === ""
    || text.startsWith("[startup] ")
    || text === "[Geode/emscripten] WebGPU adapter acquired (browser-managed)."
    || text.startsWith("GLFW error 0: [Warning] ");
}

async function waitForCanvasContent(page: Page): Promise<void> {
  const canvas = page.locator("#canvas");
  await expect.poll(
    async () => {
      const box = await canvas.boundingBox();
      if (!box || box.width <= 0 || box.height <= 0) {
        return false;
      }

      const png = await canvas.screenshot();
      return pngHasVisiblePixel(png);
    },
    { timeout: 45_000 },
  ).toBe(true);
}

async function waitForDocumentImageRect(
  page: Page,
  requireSettledLayout = false,
): Promise<ImageRect> {
  const imageRect = await page.waitForFunction(() => {
    const rect = (window as any).__donner_metrics?.documentImageRect;
    return rect && rect.width > 0 && rect.height > 0 ? rect : null;
  }, null, { timeout: 45_000 });
  if (!requireSettledLayout) {
    return imageRect.jsonValue() as Promise<ImageRect>;
  }

  const settledImageRect = await page.waitForFunction(() => {
    const rect = (window as any).__donner_metrics?.documentImageRect;
    return rect && rect.width > 0 && rect.height > 0 && (rect.left > 0 || rect.top > 0)
      ? rect
      : null;
  }, null, { timeout: 45_000 });
  return settledImageRect.jsonValue() as Promise<ImageRect>;
}

function pngHasVisiblePixel(png: Uint8Array): boolean {
  const signature = [137, 80, 78, 71, 13, 10, 26, 10];
  if (signature.some((byte, index) => png[index] !== byte)) {
    return false;
  }

  let offset = 8;
  let width = 0;
  let height = 0;
  let bitDepth = 0;
  let colorType = 0;
  const idatChunks: Uint8Array[] = [];
  while (offset + 8 <= png.length) {
    const length = readUint32(png, offset);
    const type = String.fromCharCode(
      png[offset + 4],
      png[offset + 5],
      png[offset + 6],
      png[offset + 7],
    );
    const dataStart = offset + 8;
    const dataEnd = dataStart + length;
    if (dataEnd + 4 > png.length) {
      return false;
    }

    if (type === "IHDR") {
      width = readUint32(png, dataStart);
      height = readUint32(png, dataStart + 4);
      bitDepth = png[dataStart + 8];
      colorType = png[dataStart + 9];
    } else if (type === "IDAT") {
      idatChunks.push(png.slice(dataStart, dataEnd));
    } else if (type === "IEND") {
      break;
    }
    offset = dataEnd + 4;
  }

  if (width <= 0 || height <= 0 || bitDepth !== 8 || idatChunks.length === 0) {
    return false;
  }

  const bytesPerPixel = colorType === 6 ? 4 : colorType === 2 ? 3 : 0;
  if (bytesPerPixel === 0) {
    return false;
  }

  const inflated = inflateSync(Buffer.concat(idatChunks.map((chunk) => Buffer.from(chunk))));
  const stride = width * bytesPerPixel;
  if (inflated.length < height * (stride + 1)) {
    return false;
  }

  const previous = new Uint8Array(stride);
  const current = new Uint8Array(stride);
  let sourceOffset = 0;
  for (let y = 0; y < height; ++y) {
    const filter = inflated[sourceOffset++];
    for (let x = 0; x < stride; ++x) {
      const raw = inflated[sourceOffset++];
      const left = x >= bytesPerPixel ? current[x - bytesPerPixel] : 0;
      const up = previous[x];
      const upLeft = x >= bytesPerPixel ? previous[x - bytesPerPixel] : 0;
      current[x] = unfilterPngByte(filter, raw, left, up, upLeft);
    }

    for (let x = 0; x < stride; x += bytesPerPixel) {
      if (current[x] > 35 || current[x + 1] > 35 || current[x + 2] > 35) {
        return true;
      }
    }
    previous.set(current);
  }
  return false;
}

function readUint32(bytes: Uint8Array, offset: number): number {
  return (
    ((bytes[offset] << 24) >>> 0)
    + (bytes[offset + 1] << 16)
    + (bytes[offset + 2] << 8)
    + bytes[offset + 3]
  );
}

function unfilterPngByte(
  filter: number,
  raw: number,
  left: number,
  up: number,
  upLeft: number,
): number {
  if (filter === 0) {
    return raw;
  }
  if (filter === 1) {
    return (raw + left) & 0xff;
  }
  if (filter === 2) {
    return (raw + up) & 0xff;
  }
  if (filter === 3) {
    return (raw + Math.floor((left + up) / 2)) & 0xff;
  }

  const estimate = left + up - upLeft;
  const leftDistance = Math.abs(estimate - left);
  const upDistance = Math.abs(estimate - up);
  const upLeftDistance = Math.abs(estimate - upLeft);
  const predictor = leftDistance <= upDistance && leftDistance <= upLeftDistance
    ? left
    : upDistance <= upLeftDistance
    ? up
    : upLeft;
  return (raw + predictor) & 0xff;
}

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
      const text = msg.text();
      if (!isExpectedStartupStderr(text)) {
        consoleErrors.push(text);
      }
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
    if (!canvas) return null;
    return { width: canvas.width, height: canvas.height };
  });
  expect(canvasSize).not.toBeNull();
  expect(canvasSize!.width).toBeGreaterThan(0);
  expect(canvasSize!.height).toBeGreaterThan(0);

  await waitForCanvasContent(page);

  // Metrics must be populated.
  const metrics = await page.evaluate(() => (window as any).__donner_metrics);
  expect(metrics).toBeTruthy();
  expect(metrics.renderCount).toBeGreaterThan(0);

  // No unexpected console errors during startup. Emscripten routes stderr
  // through console.error, so keep known benign startup diagnostics filtered
  // and fail on everything else.
  expect(consoleErrors, `console errors during startup:\n${consoleErrors.join("\n")}`)
    .toEqual([]);
});

test("plain WASM editor page completes startup without input", async ({ page }) => {
  await page.goto("/", { waitUntil: "load", timeout: 45_000 });

  await expect(page.locator("#status")).toBeHidden({ timeout: 45_000 });
  await waitForCanvasContent(page);
});

test("plain WASM editor page finishes after late render-pane layout", async ({ page }) => {
  await page.setViewportSize({ width: 1, height: 1 });
  await page.goto("/", { waitUntil: "load", timeout: 45_000 });
  await expect(page.locator("#status")).toBeHidden({ timeout: 45_000 });

  await page.setViewportSize({ width: 1280, height: 800 });
  await waitForCanvasContent(page);
});

test("WASM trackpad pinch wheel zooms the render pane", async ({ page }) => {
  await page.goto("/?test=1");
  await page.waitForFunction(() => (window as any).__donner_ready === true, null, {
    timeout: 45_000,
  });
  await waitForCanvasContent(page);

  const before = await waitForDocumentImageRect(page, true);
  await page.evaluate(async (rect) => {
    const canvas = document.getElementById("canvas") as HTMLCanvasElement | null;
    if (!canvas) {
      throw new Error("canvas not found");
    }

    const x = rect.left + rect.width * 0.5;
    const y = rect.top + rect.height * 0.5;
    const eventBase = {
      bubbles: true,
      cancelable: true,
      composed: true,
      view: window,
      clientX: x,
      clientY: y,
      screenX: window.screenX + x,
      screenY: window.screenY + y,
    };

    canvas.dispatchEvent(new MouseEvent("mouseenter", eventBase));
    canvas.dispatchEvent(new MouseEvent("mousemove", eventBase));
    canvas.dispatchEvent(new WheelEvent("wheel", {
      ...eventBase,
      deltaX: 0,
      deltaY: -360,
      deltaMode: WheelEvent.DOM_DELTA_PIXEL,
      ctrlKey: true,
    }));
    await new Promise((resolve) => requestAnimationFrame(() => resolve(null)));
  }, before);

  await expect.poll(
    async () => {
      const rect = await page.evaluate(() => (window as any).__donner_metrics?.documentImageRect);
      return rect?.width ?? 0;
    },
    { timeout: 15_000 },
  ).toBeGreaterThan(before.width * 1.1);
});
