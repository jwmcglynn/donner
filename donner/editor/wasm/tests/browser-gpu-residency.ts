import { expect, type Page, type TestInfo, type Worker } from "@playwright/test";
import { createHash } from "node:crypto";
import { readFileSync, writeFileSync } from "node:fs";
import path from "node:path";
import { readEditorPixelBoundsFromPng } from "./canvas-color-stats";
import { dispatchCanvasPointer } from "./canvas-pointer";
import { expectSampleThumbnailsToSettle } from "./sample-thumbnail-settle";

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

type ResizeCallbackHold = {
  held: boolean;
  pending: (() => void)[];
  release: () => void;
};
type ResizeTestWindow = Window & { __donnerResizeCallbackHold?: ResizeCallbackHold };

async function installResizeCallbackHold(page: Page) {
  await page.addInitScript(() => {
    const add = window.addEventListener.bind(window);
    const remove = window.removeEventListener.bind(window);
    const wrappers = [
      new Map<EventListenerOrEventListenerObject, EventListener>(),
      new Map<EventListenerOrEventListenerObject, EventListener>(),
    ];
    const hold: ResizeCallbackHold = {
      held: false,
      pending: [],
      release() {
        hold.held = false;
        for (const invoke of hold.pending.splice(0)) invoke();
      },
    };
    (window as ResizeTestWindow).__donnerResizeCallbackHold = hold;
    window.addEventListener = ((type, listener, options) => {
      // The bridge's passive listeners keep publishing viewport dimensions and the frame request.
      if (type !== "resize" || !listener || (typeof options === "object" && options.passive)) {
        return add(type, listener!, options);
      }
      const capture = typeof options === "boolean" ? options : options?.capture === true;
      const table = wrappers[Number(capture)];
      let wrapper = table.get(listener);
      if (!wrapper) {
        wrapper = (event) => {
          const invoke = () => {
            if (typeof listener === "function") listener.call(window, event);
            else listener.handleEvent(event);
          };
          if (hold.held) hold.pending.push(invoke);
          else invoke();
        };
        table.set(listener, wrapper);
      }
      add(type, wrapper, options);
    }) as typeof window.addEventListener;
    window.removeEventListener = ((type, listener, options) => {
      const capture = typeof options === "boolean" ? options : options?.capture === true;
      const table = wrappers[Number(capture)];
      const wrapper = type === "resize" && listener ? table.get(listener) : undefined;
      remove(type, wrapper ?? listener!, options);
      if (wrapper && listener) table.delete(listener);
    }) as typeof window.removeEventListener;
  });
}

type GpuStats = Record<string, number>;
type PreviewStats = NonNullable<SelectionWindow["__donnerSampleThumbnailStats"]>;
type Viewport = NonNullable<SelectionWindow["__donnerViewportStats"]>;
type Accounting = { raw: GpuStats; preview: PreviewStats };

async function readWorkerStats(page: Page) {
  return page.evaluate(() => (window as SelectionWindow).__donnerWorkerStats);
}

async function readViewport(page: Page) {
  return page.evaluate(() => (window as SelectionWindow).__donnerViewportStats!);
}

function assertNoDocumentReadbacks(stats?: WorkerStats) {
  if (!stats) return; // The welcome picker has no document result.
  for (
    const field of [
      "compositorReadbackTotal",
      "tileHandoffReadbackTotal",
      "finalSnapshotReadbackTotal",
      "bitmapPayloadTileTotal",
    ] as const
  ) {
    expect(stats[field], field).toBe(0);
  }
}

function idlePreviewMatches(first: PreviewStats, last: PreviewStats) {
  return first && last && first.publicationGeneration === last.publicationGeneration
    && !last.active && !last.pending && !last.resultReady
    && last.explicitPreviewReadbackTotal !== undefined;
}

async function openEditor(page: Page, onReady?: () => Promise<void>) {
  await page.setViewportSize({ width: 1600, height: 900 });
  const baseUrl = process.env.DONNER_WASM_BASE_URL || "http://127.0.0.1:8000";
  await page.goto(`${baseUrl}/index.html`, { waitUntil: "domcontentloaded" });
  await expect.poll(
    () => page.evaluate(() => (window as SelectionWindow).__donnerFirstFramePresented),
    { timeout: 30000 },
  ).toBe(true);
  await onReady?.();
  await expectSampleThumbnailsToSettle(page, {
    message: "the sample thumbnail lane must drain before document GPU accounting begins",
    timeout: 20000,
  });
}

async function findGpuOwner(page: Page): Promise<Worker> {
  return Promise.any(
    page.workers().map(async (worker) => {
      const ownsGpu = await worker.evaluate(() =>
        Boolean(
          (globalThis as typeof globalThis & { __donnerGpuOwner?: boolean }).__donnerGpuOwner,
        )
      );
      if (!ownsGpu) throw new Error("not the GPU owner");
      return worker;
    }),
  );
}

/** Observes bounded transfer metadata without copying or mapping any image pixels. */
async function installTransferProbe(owner: Worker) {
  await owner.evaluate(() => {
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
}

class GpuAccountingProbe {
  private lastObservation: unknown;

  constructor(private page: Page, readonly owner: Worker, private info: TestInfo) {}

  async saveFailureEvidence() {
    const output = this.info.outputPath("gpu-residency-failure.json");
    writeFileSync(
      output,
      JSON.stringify(this.lastObservation ?? { observationsUnavailable: true }),
    );
    await this.info.attach("gpu-residency-failure", {
      path: output,
      contentType: "application/json",
    });
  }

  readGpu(): Promise<GpuStats> {
    return this.owner.evaluate(() =>
      (globalThis as typeof globalThis & { __donnerReadGpuObjectStats: () => GpuStats })
        .__donnerReadGpuObjectStats()
    );
  }

  readPreview(): Promise<PreviewStats> {
    return this.page.evaluate(() => (window as SelectionWindow).__donnerSampleThumbnailStats!);
  }

  async readIdle(readbackOffset?: number): Promise<Accounting> {
    let result: Accounting | undefined;
    try {
      await expect.poll(async () => {
        const first = await this.readPreview();
        const raw = await this.readGpu();
        const preview = await this.readPreview();
        this.lastObservation = { first, raw, preview, readbackOffset };
        if (!idlePreviewMatches(first, preview) || raw.pendingSubmissions !== 0) return false;
        if (
          readbackOffset !== undefined
          && raw.readbackCopies - preview.explicitPreviewReadbackTotal! !== readbackOffset
        ) return false;
        result = { raw, preview };
        return true;
      }, {
        timeout: 15000,
        message: "GPU readbacks must reconcile with a fresh idle auxiliary snapshot",
      }).toBe(true);
    } catch (error) {
      await this.saveFailureEvidence();
      throw error;
    }
    return result!;
  }

  async waitForPresentAfter(count: number) {
    await expect.poll(async () => (await this.readGpu()).surfacePresents, { timeout: 10000 })
      .toBeGreaterThan(count);
  }
}

async function openBasicShapes(page: Page, priorResults: number) {
  const canvas = page.locator("canvas#canvas");
  const bounds = await canvas.boundingBox();
  expect(bounds, "the editor canvas is missing").not.toBeNull();
  await dispatchCanvasPointer(
    page,
    { x: bounds!.x + bounds!.width * 0.76, y: bounds!.y + 282 },
    true,
  );
  await expect(canvas).toHaveAttribute("data-active-sample-id", "basic-shapes");
  await expect.poll(async () => (await readWorkerStats(page))?.completedResults, { timeout: 10000 })
    .toBeGreaterThan(priorResults);
  await expect.poll(async () => Boolean((await readWorkerStats(page))?.presentedAtMs), {
    timeout: 10000,
  }).toBe(true);
}

function assertGpuDocumentResult(before: WorkerStats | undefined, after: WorkerStats | undefined) {
  expect(after, "the document result must publish readback accounting").toBeDefined();
  assertNoDocumentReadbacks(after);
  expect(after!.texturePayloadTileTotal).toBeGreaterThan(before?.texturePayloadTileTotal ?? 0);
  expect(after!.readbackCount).toBe(
    after!.compositorReadbackCount!
      + after!.tileHandoffReadbackCount! + after!.finalSnapshotReadbackCount!,
  );
}

async function pinchAtViewportCenter(page: Page, viewport: Viewport) {
  const point = {
    x: viewport.paneX + viewport.paneWidth * 0.5,
    y: viewport.paneY + viewport.paneHeight * 0.5,
  };
  await page.mouse.move(point.x, point.y);
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
  }, point);
  await expect.poll(async () => (await readViewport(page)).zoom, { timeout: 10000 }).not.toBe(
    viewport.zoom,
  );
}

async function waitForCompletedCamera(page: Page) {
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
}

async function redrawDocument(page: Page, probe: GpuAccountingProbe, priorPresents: number) {
  const priorViewport = await readViewport(page);
  let resizeObservation: unknown;
  await page.evaluate(() => {
    (window as ResizeTestWindow).__donnerResizeCallbackHold!.held = true;
  });
  try {
    await page.setViewportSize({ width: 1440, height: 920 });
    await expect.poll(
      () =>
        page.evaluate(() =>
          (window as ResizeTestWindow).__donnerResizeCallbackHold!.pending.length
        ),
      { message: "the GLFW resize callbacks must actually be withheld" },
    ).toBeGreaterThan(0);
    await expect.poll(async () => {
      resizeObservation = await page.evaluate((priorWidth) => {
        const state = window as SelectionWindow & {
          __donnerFrameLoopStats?: { renderedFrames: number; lastFrameAtMs: number };
          __donnerFrameTickStats?: unknown;
          __donnerFrameAdmissionStats?: unknown;
        };
        const frames = state.__donnerFrameLoopStats;
        return {
          resized: !!state.__donnerViewportStats
            && state.__donnerViewportStats.paneWidth !== priorWidth,
          viewport: state.__donnerViewportStats,
          browser: { width: innerWidth, height: innerHeight, dpr: devicePixelRatio },
          frames: { rendered: frames?.renderedFrames, lastAtMs: frames?.lastFrameAtMs },
          ticks: state.__donnerFrameTickStats,
          admission: state.__donnerFrameAdmissionStats,
          interaction: state.__donnerInteractionStats,
          worker: state.__donnerWorkerStats,
          queue: state.__donnerPresentationQueueStats,
        };
      }, priorViewport.paneWidth);
      return resizeObservation;
    }, {
      message: "pane layout must follow the published viewport before delayed GLFW callbacks",
      timeout: 10000,
    }).toMatchObject({ resized: true });
  } catch (error) {
    console.error(`resize-failure ${JSON.stringify(resizeObservation)}`);
    throw error;
  } finally {
    await page.evaluate(() => (window as ResizeTestWindow).__donnerResizeCallbackHold!.release());
  }
  await probe.waitForPresentAfter(priorPresents);
  const afterResizePresents = (await probe.readGpu()).surfacePresents;
  await pinchAtViewportCenter(page, await readViewport(page));
  await probe.waitForPresentAfter(afterResizePresents);
  await waitForCompletedCamera(page);
}

function packageHashes() {
  return Object.fromEntries(
    ["wasm", "js"].map((
      extension,
    ) => [
      extension,
      createHash("sha256").update(
        readFileSync(path.join(process.env.DONNER_WASM_PACKAGE_DIR!, `editor.${extension}`)),
      ).digest("hex"),
    ]),
  );
}

async function saveAccounting(page: Page, info: TestInfo, owner: Worker, observations: object) {
  const transfers = await owner.evaluate(() =>
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
      packageHashes: packageHashes(),
      ...observations,
      transfers,
    }),
  );
  await info.attach("gpu-residency-accounting", {
    path: accountingPath,
    contentType: "application/json",
  });
}

/** Explicit pixel capture happens after the measured readback/upload interval. */
async function assertVisibleDocument(page: Page, info: TestInfo) {
  const pane = await readViewport(page);
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

/** Checks the production document path after startup and sample-preview settling. */
export async function checkDocumentGpuResidency(
  page: Page,
  info: TestInfo,
  onReady?: () => Promise<void>,
) {
  await installResizeCallbackHold(page);
  await openEditor(page, onReady);
  const owner = await findGpuOwner(page);
  await installTransferProbe(owner);
  const probe = new GpuAccountingProbe(page, owner, info);
  const { raw: rawBefore, preview: beforePreview } = await probe.readIdle();
  const readbackOffset = rawBefore.readbackCopies - beforePreview.explicitPreviewReadbackTotal!;
  const before = await readWorkerStats(page);
  assertNoDocumentReadbacks(before);
  await openBasicShapes(page, before?.completedResults ?? 0);
  const { raw: rawAfter, preview: afterPreview } = await probe.readIdle(readbackOffset);
  const after = await readWorkerStats(page);
  assertGpuDocumentResult(before, after);
  expect(rawAfter.surfacePresents).toBeGreaterThan(rawBefore.surfacePresents);
  await redrawDocument(page, probe, rawAfter.surfacePresents);
  const { raw: finalRaw, preview: finalPreview } = await probe.readIdle(readbackOffset);
  const finalStats = await readWorkerStats(page);
  await saveAccounting(page, info, owner, {
    rawBefore,
    rawAfter,
    finalRaw,
    before,
    after,
    finalStats,
    beforePreview,
    afterPreview,
    finalPreview,
  });
  expect(finalRaw.cpuTextureWrites).toBe(rawAfter.cpuTextureWrites);
  assertGpuDocumentResult(before, finalStats);
  await assertVisibleDocument(page, info);
}
