import { expect, test } from "@playwright/test";
import { captureSplashPresentationFrame, type CssRegion } from "./canvas-color-stats";
import {
  findCanvasOwnerWorker,
  installSurfaceFrameProbe,
  readSurfaceFrameProbe,
} from "./surface-frame-probe";

const scaledMs = (ms: number) => ms * (process.env.CI ? 4 : 1);

// Follow the Splash D's solid left stem through the same ten-step drag corridor
// used by the presentation suite. Searching the whole document also finds its
// separate yellow lightning and O artwork, which could hide a missing D.
function splashLetterTrackingWindow(stats: {
  documentX: number;
  documentY: number;
  documentWidth: number;
  documentHeight: number;
}): CssRegion {
  const scale = stats.documentWidth / 892;
  const stemX = stats.documentX + stats.documentWidth * (271.41 / 892);
  const stemY = stats.documentY + stats.documentHeight * (433.5 / 512);
  const margin = 6 * scale;
  const left = stemX - 50 - margin;
  const top = stemY - 25 - 50 * scale;
  return {
    x: left,
    y: top,
    width: stemX + 20 * scale - left,
    height: stemY + margin - top,
  };
}

test("Firefox restores the Splash canvas after transient surface loss", async ({ browserName, page }) => {
  test.skip(browserName !== "firefox", "Firefox canvas recovery regression");
  const failures: string[] = [];
  page.on("pageerror", (error) => failures.push(error.message));
  page.on("console", (message) => {
    if (
      /Failed to wake Wasm renderer pthread|Wasm renderer pthread wake rejected|Aborted|RuntimeError|UTILS_RELEASE_ASSERT/i
        .test(message.text())
    ) {
      failures.push(message.text());
    }
  });

  await page.goto(process.env.DONNER_WASM_BASE_URL || "http://127.0.0.1:8000", {
    waitUntil: "domcontentloaded",
  });
  await expect.poll(() =>
    page.evaluate(() =>
      (window as Window & {
        __donnerCanStartWasm?: boolean;
      }).__donnerCanStartWasm
    )
  ).toBe(true);
  expect(await page.evaluate(() => "gpu" in navigator)).toBe(true);
  const canvas = page.locator("canvas#canvas");
  await expect(canvas).toBeVisible();
  await expect.poll(() =>
    page.evaluate(() =>
      (window as Window & {
        __donnerFirstFramePresented?: boolean;
      }).__donnerFirstFramePresented
    ), { timeout: scaledMs(5_000) }).toBe(true);
  await expect(page.locator("#status")).toBeHidden({ timeout: scaledMs(5_000) });

  const bounds = await canvas.boundingBox();
  expect(bounds).not.toBeNull();
  if (bounds === null) throw new Error("editor canvas is missing");
  const before = await page.evaluate(() => {
    const state = window as Window & {
      __donnerWorkerStats?: { completedResults: number };
      __donnerMainLoopRenderedFrames?: number;
    };
    return {
      results: state.__donnerWorkerStats?.completedResults ?? 0,
      frames: state.__donnerMainLoopRenderedFrames ?? 0,
    };
  });
  await page.mouse.click(bounds.x + bounds.width * 0.24, bounds.y + 282);
  await expect(canvas).toHaveAttribute("data-active-sample-id", "donner-splash");
  await expect.poll(() =>
    page.evaluate((previous) => {
      const state = window as Window & {
        __donnerWorkerStats?: { completedResults: number; presentedAtMs?: number };
        __donnerMainLoopRenderedFrames?: number;
        __donnerActiveSampleStats?: { sampleId: string };
      };
      return state.__donnerActiveSampleStats?.sampleId === "donner-splash"
        && (state.__donnerWorkerStats?.completedResults ?? 0) > previous.results
        && (state.__donnerMainLoopRenderedFrames ?? 0) > previous.frames
        && state.__donnerWorkerStats?.presentedAtMs !== undefined;
    }, before), {
    message: "the Splash sample must reach the presented canvas",
    timeout: scaledMs(5_000),
  }).toBe(true);

  const viewport = await page.evaluate(() =>
    (window as Window & {
      __donnerViewportStats?: {
        paneX: number;
        paneY: number;
        paneWidth: number;
        paneHeight: number;
        documentX: number;
        documentY: number;
        documentWidth: number;
        documentHeight: number;
      };
    }).__donnerViewportStats
  );
  expect(viewport).toBeDefined();
  if (!viewport) throw new Error("editor viewport is missing");
  const left = Math.max(viewport.documentX, viewport.paneX);
  const top = Math.max(viewport.documentY, viewport.paneY);
  const region: CssRegion = {
    x: left,
    y: top,
    width:
      Math.min(viewport.documentX + viewport.documentWidth, viewport.paneX + viewport.paneWidth)
      - left,
    height:
      Math.min(viewport.documentY + viewport.documentHeight, viewport.paneY + viewport.paneHeight)
      - top,
  };
  expect(region.width).toBeGreaterThan(0);
  expect(region.height).toBeGreaterThan(0);
  const letterWindow = splashLetterTrackingWindow(viewport);

  expect(await installSurfaceFrameProbe(page)).toBeGreaterThan(0);
  await page.evaluate(() => {
    (window as Window & { __donnerEditorFrameRequested?: boolean }).__donnerEditorFrameRequested =
      true;
  });
  await expect.poll(async () => (await readSurfaceFrameProbe(page)).canvasWorkers, {
    message: "the probe must identify the editor canvas worker",
    timeout: scaledMs(2_000),
  }).toBe(1);
  const owner = await findCanvasOwnerWorker(page);
  expect(owner).not.toBeNull();
  if (owner === null) return;

  await owner.evaluate(() => {
    const scope = globalThis as typeof globalThis & {
      __donnerInjectedSurfaceFailures?: {
        remaining: number;
        observed: number;
        successfulAfterFault: number;
      };
    };
    const injected = { remaining: 2, observed: 0, successfulAfterFault: 0 };
    scope.__donnerInjectedSurfaceFailures = injected;
    const original = GPUCanvasContext.prototype.getCurrentTexture;
    GPUCanvasContext.prototype.getCurrentTexture = function(this: GPUCanvasContext) {
      const editorCanvas = this.canvas.width >= 500 && this.canvas.height >= 500;
      if (editorCanvas && injected.remaining > 0) {
        --injected.remaining;
        ++injected.observed;
        throw new Error("injected transient canvas surface loss");
      }
      const texture = original.call(this);
      if (editorCanvas && injected.observed === 2) ++injected.successfulAfterFault;
      return texture;
    };
  });
  await page.evaluate(() => {
    (window as Window & { __donnerEditorFrameRequested?: boolean }).__donnerEditorFrameRequested =
      true;
  });
  await expect.poll(() =>
    owner.evaluate(() =>
      (globalThis as typeof globalThis & {
        __donnerInjectedSurfaceFailures?: { observed: number };
      }).__donnerInjectedSurfaceFailures?.observed ?? 0
    ), {
    message: "both injected surface acquisitions must fail",
    timeout: scaledMs(2_000),
  }).toBe(2);

  const framesAfterLoss = (await readSurfaceFrameProbe(page)).frames;
  await page.evaluate(() => {
    (window as Window & { __donnerEditorFrameRequested?: boolean }).__donnerEditorFrameRequested =
      true;
  });
  await expect.poll(async () => (await readSurfaceFrameProbe(page)).frames, {
    message: "the editor canvas must acquire a new frame after surface loss",
    timeout: scaledMs(2_000),
  }).toBeGreaterThan(framesAfterLoss);
  await expect.poll(() =>
    owner.evaluate(() =>
      (globalThis as typeof globalThis & {
        __donnerInjectedSurfaceFailures?: { successfulAfterFault: number };
      }).__donnerInjectedSurfaceFailures?.successfulAfterFault ?? 0
    ), {
    message: "the editor canvas must acquire a texture after both injected losses",
    timeout: scaledMs(2_000),
  }).toBeGreaterThan(0);

  await expect.poll(async () => {
    const frame = await captureSplashPresentationFrame(page, region, letterWindow);
    return frame.letter !== null
      && frame.census.darkBackgroundPixels > frame.census.samples * 0.25;
  }, {
    message: "the recovered canvas must show the Splash artwork and document background",
    timeout: scaledMs(5_000),
  }).toBe(true);
  expect(failures).toEqual([]);
});
