import { expect, type Page, type TestInfo } from "@playwright/test";
import { readEditorPixelBoundsFromPng } from "./canvas-color-stats";

type WorkerStats = {
  completedResults?: number;
  acceptedForPresentation?: boolean;
  presentedAtMs?: number;
  readbackCount?: number;
  readbackWaitStrategy?: string;
  compositorReadbackCount?: number;
  tileHandoffReadbackCount?: number;
  finalSnapshotReadbackCount?: number;
  compositorReadbackTotal?: number;
  tileHandoffReadbackTotal?: number;
  finalSnapshotReadbackTotal?: number;
  bitmapPayloadTileTotal?: number;
  texturePayloadTileTotal?: number;
};
export type SelectionWindow = Window & {
  __donnerFirstFramePresented?: boolean;
  __donnerViewportStats?: {
    paneX: number;
    paneY: number;
    paneWidth: number;
    paneHeight: number;
    zoom: number;
  };
  __donnerWorkerStats?: WorkerStats;
  __donnerSampleThumbnailStats?: {
    completed?: number;
    ready?: number;
    active?: boolean;
    pending?: boolean;
  };
};

/** Checks the production document path, then captures pixels outside the measured workload. */
export async function checkDocumentGpuResidency(
  page: Page,
  info: TestInfo,
  onReady?: () => Promise<void>,
) {
  await page.setViewportSize({ width: 1600, height: 900 });
  const baseUrl = process.env.DONNER_WASM_BASE_URL || "http://127.0.0.1:8000";
  await page.goto(`${baseUrl}/index.html`, { waitUntil: "domcontentloaded" });
  await expect
    .poll(() => page.evaluate(() => (window as SelectionWindow).__donnerFirstFramePresented), {
      timeout: 30000,
    })
    .toBe(true);

  await onReady?.();
  await expect
    .poll(() =>
      page.evaluate(() => {
        const thumbnails = (window as SelectionWindow).__donnerSampleThumbnailStats;
        return !!thumbnails && (thumbnails.completed ?? 0) > 0
          && (thumbnails.ready ?? 0) > 0 && !thumbnails.active && !thumbnails.pending;
      }), { timeout: 20000 })
    .toBe(true);
  const gpuOwner = await Promise.any(
    page.workers().map(async (worker) => {
      const ownsGpu = await worker.evaluate(() =>
        Boolean(
          (globalThis as typeof globalThis & { __donnerGpuOwner?: boolean })
            .__donnerGpuOwner,
        )
      );
      if (!ownsGpu) throw new Error("not the GPU owner");
      return worker;
    }),
  );
  const rawBefore = await gpuOwner.evaluate(() => {
    const state = globalThis as typeof globalThis & {
      __donnerReadGpuObjectStats: () => {
        readbackCopies: number;
        cpuTextureWrites: number;
        surfacePresents: number;
      };
    };
    return state.__donnerReadGpuObjectStats();
  });
  const before = await page.evaluate(() => (window as SelectionWindow).__donnerWorkerStats);
  const editorCanvas = page.locator("canvas#canvas");
  const bounds = await editorCanvas.boundingBox();
  expect(bounds, "the editor canvas is missing").not.toBeNull();
  await page.mouse.click(bounds!.x + bounds!.width * 0.76, bounds!.y + 282);
  await expect(editorCanvas).toHaveAttribute("data-active-sample-id", "basic-shapes");
  await expect
    .poll(
      () => page.evaluate(() => (window as SelectionWindow).__donnerWorkerStats?.completedResults),
      {
        timeout: 10000,
      },
    )
    .toBeGreaterThan(before?.completedResults ?? 0);

  await expect.poll(
    () =>
      page.evaluate(() => Boolean((window as SelectionWindow).__donnerWorkerStats?.presentedAtMs)),
    { timeout: 10000 },
  ).toBe(true);
  const after = await page.evaluate(() => (window as SelectionWindow).__donnerWorkerStats);
  const rawAfter = await gpuOwner.evaluate(() => {
    const state = globalThis as typeof globalThis & {
      __donnerReadGpuObjectStats: () => {
        readbackCopies: number;
        cpuTextureWrites: number;
        surfacePresents: number;
      };
    };
    return state.__donnerReadGpuObjectStats();
  });
  expect(rawAfter.readbackCopies).toBe(rawBefore.readbackCopies);
  expect(rawAfter.surfacePresents).toBeGreaterThan(rawBefore.surfacePresents);
  expect(after, "the document result did not publish readback accounting").toBeDefined();
  expect(after!.bitmapPayloadTileTotal).toBe(before?.bitmapPayloadTileTotal ?? 0);
  expect(after!.texturePayloadTileTotal).toBeGreaterThan(before?.texturePayloadTileTotal ?? 0);
  expect(after!.compositorReadbackTotal).toBeDefined();
  expect(after!.tileHandoffReadbackTotal).toBeDefined();
  expect(after!.finalSnapshotReadbackTotal).toBeDefined();
  expect(after!.readbackCount).toBe(
    (after!.compositorReadbackCount ?? 0) + (after!.tileHandoffReadbackCount ?? 0)
      + (after!.finalSnapshotReadbackCount ?? 0),
  );
  expect((after!.compositorReadbackTotal ?? 0) - (before?.compositorReadbackTotal ?? 0)).toBe(0);
  expect((after!.tileHandoffReadbackTotal ?? 0) - (before?.tileHandoffReadbackTotal ?? 0)).toBe(0);
  expect((after!.finalSnapshotReadbackTotal ?? 0) - (before?.finalSnapshotReadbackTotal ?? 0)).toBe(
    0,
  );
  // UI assets may upload while opening the document. Redraws must neither upload tile pixels
  // nor read them back; tile publication counts cover the initial opening as well.
  const priorViewport = await page.evaluate(() =>
    (window as SelectionWindow).__donnerViewportStats!
  );
  await page.setViewportSize({ width: 1440, height: 920 });
  await expect.poll(
    () => page.evaluate(() => (window as SelectionWindow).__donnerViewportStats?.paneWidth),
    { timeout: 10000 },
  ).not.toBe(priorViewport.paneWidth);
  const viewport = await page.evaluate(() => (window as SelectionWindow).__donnerViewportStats!);
  await page.mouse.move(
    viewport.paneX + viewport.paneWidth * 0.5,
    viewport.paneY + viewport.paneHeight * 0.5,
  );
  await page.evaluate(({ x, y }) => {
    document.querySelector("canvas#canvas")!.dispatchEvent(
      new WheelEvent("wheel", {
        bubbles: true,
        cancelable: true,
        clientX: x,
        clientY: y,
        ctrlKey: true,
        deltaMode: 0,
        deltaY: -20.5,
      }),
    );
  }, {
    x: viewport.paneX + viewport.paneWidth * 0.5,
    y: viewport.paneY + viewport.paneHeight * 0.5,
  });
  await expect.poll(
    () => page.evaluate(() => (window as SelectionWindow).__donnerViewportStats?.zoom),
    { timeout: 10000 },
  ).not.toBe(viewport.zoom);
  await expect.poll(
    () =>
      gpuOwner.evaluate(() =>
        (globalThis as typeof globalThis & {
          __donnerReadGpuObjectStats: () => { surfacePresents: number };
        })
          .__donnerReadGpuObjectStats().surfacePresents
      ),
    { timeout: 10000 },
  ).toBeGreaterThan(rawAfter.surfacePresents);
  const finalRaw = await gpuOwner.evaluate(() =>
    (globalThis as typeof globalThis & { __donnerReadGpuObjectStats: () => Record<string, number> })
      .__donnerReadGpuObjectStats()
  );
  const finalStats = await page.evaluate(() => (window as SelectionWindow).__donnerWorkerStats!);
  expect(finalRaw.readbackCopies).toBe(rawBefore.readbackCopies);
  expect(finalRaw.cpuTextureWrites).toBe(rawAfter.cpuTextureWrites);
  expect(finalStats.bitmapPayloadTileTotal).toBe(before?.bitmapPayloadTileTotal ?? 0);
  expect(finalStats.compositorReadbackTotal).toBe(before?.compositorReadbackTotal ?? 0);
  expect(finalStats.tileHandoffReadbackTotal).toBe(before?.tileHandoffReadbackTotal ?? 0);
  expect(finalStats.finalSnapshotReadbackTotal).toBe(before?.finalSnapshotReadbackTotal ?? 0);

  // Explicit screenshots are outside the readback/upload workload.
  const pane = await page.evaluate(() => (window as SelectionWindow).__donnerViewportStats!);
  const image = await page.screenshot({
    clip: { x: pane.paneX, y: pane.paneY, width: pane.paneWidth, height: pane.paneHeight },
  });
  const blue = readEditorPixelBoundsFromPng(image, "basic-blue", {
    width: pane.paneWidth,
    height: pane.paneHeight,
  }, { minX: 0, minY: 0, maxX: pane.paneWidth, maxY: pane.paneHeight });
  await info.attach("gpu-resident-document", { body: image, contentType: "image/png" });
  await info.attach("gpu-residency-accounting", {
    contentType: "application/json",
    body: JSON.stringify({ rawBefore, rawAfter, finalRaw, before, after, finalStats, blue }),
  });
  expect(blue?.pixels, "the GPU-resident Basic Shapes document must be visible").toBeGreaterThan(
    100,
  );
}
