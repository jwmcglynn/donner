import { expect, type Page, type TestInfo } from "@playwright/test";
import { createHash } from "node:crypto";
import { readFileSync, writeFileSync } from "node:fs";
import path from "node:path";
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
  __donnerPresentationQueueStats?: { inputRepresented: boolean; viewportZoom: number };
  __donnerInteractionStats?: { pendingClick: boolean; workerBusy: boolean };
  __donnerSampleThumbnailStats?: {
    explicitPreviewReadbackTotal?: number;
    publicationGeneration?: number;
    resultReady?: boolean;
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
  await gpuOwner.evaluate(() => {
    const scope = globalThis as typeof globalThis & { __donnerResidencyTransfers?: unknown[] };
    const transfers: unknown[] = [];
    scope.__donnerResidencyTransfers = transfers;
    const copy = GPUCommandEncoder.prototype.copyTextureToBuffer;
    GPUCommandEncoder.prototype.copyTextureToBuffer = function(source, destination, size) {
      if (transfers.length < 32) {
        transfers.push({
          kind: "readback",
          width: source.texture.width,
          height: source.texture.height,
          size,
        });
      }
      return copy.call(this, source, destination, size);
    };
    const write = GPUQueue.prototype.writeTexture;
    GPUQueue.prototype.writeTexture = function(destination, bytes, layout, size) {
      if (transfers.length < 32) {
        transfers.push({
          kind: "upload",
          width: destination.texture.width,
          height: destination.texture.height,
          size,
        });
      }
      return write.call(this, destination, bytes, layout, size);
    };
  });
  const readIdleAccounting = async (readbackOffset?: number) => {
    let result: {
      raw: Record<string, number>;
      preview: NonNullable<SelectionWindow["__donnerSampleThumbnailStats"]>;
    } | undefined;
    await expect.poll(async () => {
      const first = await page.evaluate(() =>
        (window as SelectionWindow).__donnerSampleThumbnailStats!
      );
      const raw = await gpuOwner.evaluate(() =>
        (globalThis as typeof globalThis & {
          __donnerReadGpuObjectStats: () => Record<string, number>;
        })
          .__donnerReadGpuObjectStats()
      );
      const preview = await page.evaluate(() =>
        (window as SelectionWindow).__donnerSampleThumbnailStats!
      );
      if (
        !first || !preview || first.publicationGeneration !== preview.publicationGeneration
        || preview.active || preview.pending || preview.resultReady || raw.pendingSubmissions !== 0
        || preview.explicitPreviewReadbackTotal === undefined
      ) return false;
      if (
        readbackOffset !== undefined
        && raw.readbackCopies - preview.explicitPreviewReadbackTotal !== readbackOffset
      ) return false;
      result = { raw, preview };
      return true;
    }, {
      timeout: 15000,
      message: "GPU readbacks must reconcile with a fresh idle auxiliary snapshot",
    }).toBe(true);
    return result!;
  };
  const { raw: rawBefore, preview: beforePreview } = await readIdleAccounting();
  const readbackOffset = rawBefore.readbackCopies - beforePreview.explicitPreviewReadbackTotal!;
  const before = await page.evaluate(() => (window as SelectionWindow).__donnerWorkerStats);
  expect(before?.compositorReadbackTotal ?? 0).toBe(0);
  expect(before?.tileHandoffReadbackTotal ?? 0).toBe(0);
  expect(before?.finalSnapshotReadbackTotal ?? 0).toBe(0);
  expect(before?.bitmapPayloadTileTotal ?? 0).toBe(0);

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
  const { raw: rawAfter, preview: afterPreview } = await readIdleAccounting(readbackOffset);
  const after = await page.evaluate(() => (window as SelectionWindow).__donnerWorkerStats);
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
  await expect.poll(
    () =>
      gpuOwner.evaluate(() =>
        (globalThis as typeof globalThis & {
          __donnerReadGpuObjectStats: () => Record<string, number>;
        })
          .__donnerReadGpuObjectStats().surfacePresents
      ),
    { timeout: 10000 },
  ).toBeGreaterThan(rawAfter.surfacePresents);
  const afterResizePresents = await gpuOwner.evaluate(() =>
    (globalThis as typeof globalThis & { __donnerReadGpuObjectStats: () => Record<string, number> })
      .__donnerReadGpuObjectStats().surfacePresents
  );
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
  ).toBeGreaterThan(afterResizePresents);
  await expect.poll(() =>
    page.evaluate(() => {
      const state = window as SelectionWindow;
      const completed = state.__donnerPresentationQueueStats;
      const viewport = state.__donnerViewportStats;
      return completed?.inputRepresented && viewport
        && Math.abs(completed.viewportZoom - viewport.zoom) < 0.001;
    }), { timeout: 15000 }).toBe(true);
  await expect.poll(
    () => page.evaluate(() => (window as SelectionWindow).__donnerInteractionStats),
    { timeout: 15000 },
  ).toEqual(expect.objectContaining({ pendingClick: false, workerBusy: false }));
  await expect.poll(
    () =>
      gpuOwner.evaluate(() =>
        (globalThis as typeof globalThis & {
          __donnerReadGpuObjectStats: () => Record<string, number>;
        })
          .__donnerReadGpuObjectStats().pendingSubmissions
      ),
    { timeout: 15000 },
  ).toBe(0);
  await expect.poll(
    () => page.evaluate(() => (window as SelectionWindow).__donnerSampleThumbnailStats),
    { timeout: 15000 },
  ).toEqual(expect.objectContaining({ active: false, pending: false, resultReady: false }));
  const { raw: finalRaw, preview: finalPreview } = await readIdleAccounting(readbackOffset);
  const finalStats = await page.evaluate(() => (window as SelectionWindow).__donnerWorkerStats!);
  const transfers = await gpuOwner.evaluate(() =>
    (globalThis as typeof globalThis & { __donnerResidencyTransfers?: unknown[] })
      .__donnerResidencyTransfers
  );
  const accountingPath = info.outputPath("gpu-residency-accounting.json");
  writeFileSync(
    accountingPath,
    JSON.stringify({
      browser: page.context().browser()?.version(),
      project: info.project.name,
      devicePixelRatio: await page.evaluate(() => devicePixelRatio),
      packageHashes: Object.fromEntries(
        ["wasm", "js"].map((
          extension,
        ) => [
          extension,
          createHash("sha256").update(
            readFileSync(path.join(process.env.DONNER_WASM_PACKAGE_DIR!, `editor.${extension}`)),
          ).digest("hex"),
        ]),
      ),
      rawBefore,
      rawAfter,
      finalRaw,
      before,
      after,
      finalStats,
      beforePreview,
      afterPreview,
      finalPreview,
      transfers,
    }),
  );
  await info.attach("gpu-residency-accounting", {
    path: accountingPath,
    contentType: "application/json",
  });
  expect(finalRaw.readbackCopies - rawBefore.readbackCopies, JSON.stringify(transfers)).toBe(
    finalPreview.explicitPreviewReadbackTotal! - beforePreview.explicitPreviewReadbackTotal!,
  );
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
  const imagePath = info.outputPath("gpu-resident-document.png");
  writeFileSync(imagePath, image);
  await info.attach("gpu-resident-document", { path: imagePath, contentType: "image/png" });
  expect(blue?.pixels, "the GPU-resident Basic Shapes document must be visible").toBeGreaterThan(
    100,
  );
}
