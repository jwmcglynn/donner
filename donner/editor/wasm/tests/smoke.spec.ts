import { expect, type Page, test } from "@playwright/test";
import {
  type CanvasColorStats,
  findElementColoredPixel,
  readCanvasColorStats,
  readEditorPixelBounds,
  readEditorResizePixelBounds,
  readElementColorStats,
  readTextStyleGlyphStats,
} from "./canvas-color-stats";

declare global {
  interface Window {
    __donnerBackend?: string;
    __donnerWholeAppWorker?: boolean;
    __donnerCanStartWasm?: boolean;
    __donnerFirstFramePresented?: boolean;
    __donnerLastScrollEvent?: {
      zoomModifierHeld?: boolean;
      yoffset?: number;
      count?: number;
    };
    __donnerPinchWheelDeltaPerLnScale?: number;
    __donnerWorkerStats?: {
      completedResults: number;
      publishedAtMs: number;
      workerMs: number;
      queueWaitMs: number;
      dequeueToStartMs: number;
      setupMs: number;
      renderFrameMs: number;
      buildPreviewMs: number;
      finalSnapshotMs: number;
      diagnosticsMs: number;
      pollDelayMs: number;
      wakeToPollMs: number;
      firstFrameDrawMs: number;
      firstFramePlanningMs: number;
      firstFrameWarmupMs: number;
      immediateRasterizeMs: number;
      cachedRasterizeMs: number;
      immediateTileCount: number;
      cachedTileCount: number;
      readbackCount: number;
      readbackPollIterations: number;
      readbackWaitStrategy: string;
    };
    __donnerActiveSampleStats?: {
      sampleId: string;
      activatedAtMs: number;
    };
    __donnerWgpuReadbackStats?: {
      frame: number;
      request: number;
      renderPane: WgpuReadbackColorStats;
      layerPreview: WgpuReadbackColorStats;
      selectionChromePixels: number;
      carouselThumbnails?: WgpuCarouselThumbnailStats[];
    };
    __donnerRequestWgpuReadback?: () => number;
    __donnerWgpuReadbackCaptureStarts?: number;
    __donnerWgpuReadbackCaptureCompletions?: number;
    __donnerWgpuReadbackCaptureFailures?: number;
    __donnerMainLoopRenderedFrames?: number;
    __donnerInteractionStats?: {
      selectedCount: number;
      pendingClick: boolean;
      workerBusy: boolean;
      dragging: boolean;
      publishedAtFrame: number;
    };
    __donnerViewportStats?: {
      paneX: number;
      paneY: number;
      paneWidth: number;
      paneHeight: number;
      documentX: number;
      documentY: number;
      documentWidth: number;
      documentHeight: number;
      zoom: number;
      // Renders the editor owes itself rather than ones an interaction asked
      // for: a debounced canvas-size commit invalidates the render tree, and an
      // overview-infill request restores whole-document coverage the presenter
      // is missing. Both are bounded, both are invisible to
      // `waitForPresentationQuiescence` while still pending, and both are
      // therefore candidates for a render that lands on the frame an
      // interaction wakes. Published so a render-count failure can say whether
      // either one is responsible.
      documentCanvasCommits: number;
      overviewInfillRenders: number;
    };
    __donnerFirstFramePresentedAtMs?: number;
    __donnerEditorRevealedAtMs?: number;
    __donnerLoadingScreenHiddenAtMs?: number;
    __donnerBootstrapStartedAtMs?: number;
    __donnerRuntimeInitializedAtMs?: number;
    __donnerHeadlessDeviceCreations?: number;
    __donnerSampleThumbnailStats?: {
      carouselFrame: number;
      firstRequestFrame: number;
      requested: number;
      started: number;
      completed: number;
      rendered: number;
      ready: number;
      publicationFrames: number[];
      pending: boolean;
      active: boolean;
      resultReady: boolean;
    };
    __donnerLayerThumbnailStats?: {
      rowCount: number;
      renderedCount: number;
      reusedCount: number;
      deferredCount: number;
      skippedForCanvasInvalidationCount: number;
      snapshotRebuildCount: number;
      bitmapCount: number;
      bitmapBytes: number;
      textureSnapshotCount: number;
      textureCount: number;
    };
    __donnerPresentationResourceStats?: {
      totalTrackedBytes: number;
      peakTrackedBytes: number;
      pendingRetiredBytes: number;
      agedRetiredBytes: number;
      activeTileTextures: number;
      overviewTileTextures: number;
      pendingRetiredTextures: number;
      agedRetiredTextures: number;
      retiredFrameCount: number;
      lifetimeTextureCreates: number;
      lifetimeBufferCreates: number;
    };
  }
}

type WgpuReadbackColorStats = Omit<CanvasColorStats, "region">;

interface WgpuCarouselThumbnailStats extends WgpuReadbackColorStats {
  fingerprint: number;
  backgroundPixels: number;
  glyphPixels: number;
}

interface OpenEditorOptions {
  wgpuReadbackStats?: boolean;
  postInitializationDwellMs?: number;
}

const kFatalRuntimePattern =
  /Aborted|Assertion failed|RuntimeError|Pthread .* sent an error|getJsObject|No available adapters|WebGPU on Linux requires|WebGPU adapter (?:request )?(?:failed|unavailable)|Wasm renderer pthread wake rejected/i;
const kSourcePaneWidth = 560;
const kRightPaneWidth = 420;
const kWelcomeContentMaxWidth = 920;

const kBackend = "geode";

// Shared CI runners execute this suite 2-4x slower than local development
// hardware. Scale wall-clock budgets and tight acceptance polls so the timing
// assertions verify the same invariants without flaking on runner speed;
// local bounds are unchanged (scale 1).
const kCiTimeScale = process.env.CI ? 4 : 1;
const scaledMs = (ms: number) => ms * kCiTimeScale;
const kRequireWebGpu = process.env.DONNER_WASM_REQUIRE_WEBGPU === "1";
// Match Chromium's webgpu-swiftshader test configuration. Dawn selects
// SwiftShader for WebGPU while the pixel-validation lane uses explicit texture
// readback, so the test does not depend on the browser compositor backend.
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

// Document geometry inside `#canvas` is calibrated against the native editor's
// 1600x900 layout (source pane 560px, right pane 420px), so pointer targets
// below are fixed offsets from the canvas box. Since the single-canvas architecture there is no
// separate document element to measure: Geode places the document inside the
// single canvas's WebGPU frame.
const kBlueRectOffset = { x: 408, y: 353 };

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
  if (kBackend === "geode") {
    throw new Error(
      "Geode layer pixels require an explicit WGPU readback; browser screenshots omit WebGPU swapchains",
    );
  }
  throw new Error("The editor Wasm package must use Geode");
}

async function openEditor(page: Page, options: OpenEditorOptions = {}): Promise<string[]> {
  const baseUrl = process.env.DONNER_WASM_BASE_URL || "http://127.0.0.1:8000";
  const url = new URL(baseUrl);
  // Readback diagnostics are intentionally opt-in: even asynchronous readback
  // copies and scans the framebuffer, so production interaction tests should
  // not include that debug-only work unless they are explicitly guarding it.
  if (options.wgpuReadbackStats === true) {
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
    fatalMessages.push(`[pageerror] ${error.stack || error.message}`);
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
  const hasWebGpu = await page.evaluate(() => "gpu" in navigator);
  const browserName = page.context().browser()?.browserType().name();
  if (!hasWebGpu && kRequireWebGpu && browserName !== "webkit") {
    throw new Error("Geode smoke suite requires WebGPU, but navigator.gpu is unavailable");
  }
  // Playwright's bundled WebKit ships no WebGPU; the Geode-only package needs
  // real Safari for that engine, which runs in the headed validation lane.
  test.skip(!hasWebGpu, "Browser does not expose navigator.gpu");

  await expect(page.locator("canvas#canvas")).toBeVisible();
  // The single canvas is the whole application, so "the editor is up" is one
  // signal: Geode presented its first frame into it.
  await expect
    .poll(async () => page.evaluate(() => window.__donnerFirstFramePresented === true), {
      message: "expected Geode to present the editor's first frame",
      timeout: 20000,
      intervals: [16, 25, 50, 100],
    })
    .toBe(true);
  await expect(page.locator("#status")).toBeHidden({ timeout: 20000 });
  const selectedBackend = await page.evaluate(() => window.__donnerBackend);
  expect(selectedBackend).toBe(kBackend);
  expect(await page.evaluate(() => window.__donnerWholeAppWorker)).toBe(true);
  await page.waitForTimeout(options.postInitializationDwellMs ?? 2000);

  return fatalMessages;
}

async function requestWgpuDiagnostic(page: Page): Promise<number> {
  return page.evaluate(() => {
    if (!window.__donnerRequestWgpuReadback) {
      throw new Error("WGPU diagnostic request hook is unavailable");
    }
    return window.__donnerRequestWgpuReadback();
  });
}

// A landed worker result is not a settled presentation. Loading a sample can
// queue follow-up renders (the debounced canvas-size commit, the post-drag
// settled-selection refresh), and a drag release always queues one settle
// render behind the frame the pointer already produced. Tests that assert "no
// further renders happened" must start from quiescence, not from "one result
// landed".
//
// Quiescence here means: the worker is idle and `completedResults` has not
// moved for a continuous settle window. Returns the settled result count.
async function waitForPresentationQuiescence(
  page: Page,
  options: { message: string; settleMs?: number; timeout?: number },
): Promise<number> {
  // The settle window has to outlast the longest bounded follow-up the renderer
  // can schedule without the worker looking busy, which is the 120 ms
  // canvas-size commit debounce.
  const settleMs = options.settleMs ?? scaledMs(400);
  const deadline = Date.now() + (options.timeout ?? scaledMs(6000));
  let lastCount = -1;
  let stableSince = Date.now();
  for (;;) {
    const snapshot = await page.evaluate(() => ({
      completed: window.__donnerWorkerStats?.completedResults || 0,
      busy: window.__donnerInteractionStats?.workerBusy ?? false,
    }));
    const now = Date.now();
    if (snapshot.busy || snapshot.completed !== lastCount) {
      lastCount = snapshot.completed;
      stableSince = now;
    } else if (now - stableSince >= settleMs) {
      return lastCount;
    }
    if (now >= deadline) {
      throw new Error(
        `${options.message}: presentation never quiesced (completedResults=${snapshot.completed}, `
          + `workerBusy=${snapshot.busy})`,
      );
    }
    await page.waitForTimeout(25);
  }
}

// One snapshot of everything that can explain a document render count: the
// count itself, the two renders the editor owes itself (see
// `__donnerViewportStats`), and the interaction and frame state around them.
async function readRenderAccounting(page: Page): Promise<Record<string, unknown>> {
  return page.evaluate(() => ({
    completedResults: window.__donnerWorkerStats?.completedResults || 0,
    documentCanvasCommits: window.__donnerViewportStats?.documentCanvasCommits ?? -1,
    overviewInfillRenders: window.__donnerViewportStats?.overviewInfillRenders ?? -1,
    interaction: window.__donnerInteractionStats,
    renderedFrames: window.__donnerMainLoopRenderedFrames || 0,
  }));
}

test("wasm editor starts without runtime abort", async ({ page }) => {
  const fatalMessages = await openEditor(page);

  expect(fatalMessages).toEqual([]);
});

test("loading handoff publishes ordered startup timings", async ({ page }) => {
  const startupDeviceLogs: string[] = [];
  page.on("console", (message) => {
    const text = message.text();
    if (/\b(?:adapter|device|renderer|webgpu|wgpu)\b/i.test(text)) {
      startupDeviceLogs.push(`[${message.type()}] ${text}`);
    }
  });
  const fatalMessages = await openEditor(page, { postInitializationDwellMs: 0 });
  const startup = await page.evaluate(() => ({
    bootstrapStartedAtMs: window.__donnerBootstrapStartedAtMs || 0,
    runtimeInitializedAtMs: window.__donnerRuntimeInitializedAtMs || 0,
    firstFramePresentedAtMs: window.__donnerFirstFramePresentedAtMs || 0,
    firstFramePresented: window.__donnerFirstFramePresented === true,
    editorRevealedAtMs: window.__donnerEditorRevealedAtMs || 0,
    loadingScreenHiddenAtMs: window.__donnerLoadingScreenHiddenAtMs || 0,
    wgpuReadbackStats: window.__donnerWgpuReadbackStats || null,
    readbackCaptureStarts: window.__donnerWgpuReadbackCaptureStarts || 0,
    readbackCaptureCompletions: window.__donnerWgpuReadbackCaptureCompletions || 0,
    readbackCaptureFailures: window.__donnerWgpuReadbackCaptureFailures || 0,
    headlessDeviceCreations: window.__donnerHeadlessDeviceCreations ?? -1,
  }));
  expect(startup.firstFramePresented).toBe(true);
  expect(startup.bootstrapStartedAtMs).toBeGreaterThan(0);
  expect(startup.runtimeInitializedAtMs).toBeGreaterThanOrEqual(startup.bootstrapStartedAtMs);
  expect(startup.firstFramePresentedAtMs).toBeGreaterThanOrEqual(startup.runtimeInitializedAtMs);
  // The loading screen may only retire once Geode has drawn into the canvas the
  // page handed it, so the reveal is ordered strictly after the first frame.
  expect(startup.editorRevealedAtMs).toBeGreaterThanOrEqual(startup.firstFramePresentedAtMs);
  expect(startup.loadingScreenHiddenAtMs).toBeGreaterThanOrEqual(startup.editorRevealedAtMs);
  if (kBackend === "geode") {
    expect(startup.headlessDeviceCreations).toBeGreaterThanOrEqual(1);
  }
  console.log(`wasm-startup=${JSON.stringify(startup)}`);
  console.log(`wasm-startup-device-logs=${JSON.stringify(startupDeviceLogs)}`);
  expect(fatalMessages).toEqual([]);
});

test("welcome picker does not render a hidden document", async ({ page }) => {
  const fatalMessages = await openEditor(page, { postInitializationDwellMs: 0 });

  await page.waitForTimeout(750);
  expect(await page.evaluate(() => window.__donnerWorkerStats?.completedResults || 0)).toBe(0);
  expect(fatalMessages).toEqual([]);
});

test("welcome picker paints before asynchronously rendering real SVG thumbnails", async ({ page }) => {
  const fatalMessages = await openEditor(page, {
    postInitializationDwellMs: 0,
    wgpuReadbackStats: kBackend === "geode",
  });
  const initialDeviceState = await page.evaluate(() => ({
    headlessDeviceCreations: window.__donnerHeadlessDeviceCreations || 0,
  }));

  await expect
    .poll(async () => page.evaluate(() => window.__donnerSampleThumbnailStats?.ready || 0), {
      message: "expected all four catalog SVGs to publish real Donner-rendered thumbnails",
      timeout: 15000,
      intervals: [0, 25, 50, 100],
    })
    .toBe(4);

  let geodeThumbnailStats: WgpuCarouselThumbnailStats[] | undefined;
  if (kBackend === "geode") {
    const requestId = await requestWgpuDiagnostic(page);
    await expect
      .poll(async () => page.evaluate(() => window.__donnerWgpuReadbackStats?.request || 0), {
        message: "expected one explicit final-frame readback after all thumbnails were uploaded",
        timeout: scaledMs(2000),
        intervals: [16, 25, 50, 100],
      })
      .toBeGreaterThanOrEqual(requestId);
    geodeThumbnailStats = await page.evaluate(
      () => window.__donnerWgpuReadbackStats?.carouselThumbnails,
    );
  }

  const settled = await page.evaluate(() => ({
    deviceState: {
      headlessDeviceCreations: window.__donnerHeadlessDeviceCreations || 0,
    },
    thumbnails: window.__donnerSampleThumbnailStats,
  }));
  expect(settled.thumbnails).toBeDefined();
  expect(settled.thumbnails?.carouselFrame).toBeGreaterThan(0);
  expect(settled.thumbnails?.firstRequestFrame).toBeGreaterThan(
    settled.thumbnails?.carouselFrame || 0,
  );
  expect(settled.thumbnails).toMatchObject({
    requested: 4,
    started: 4,
    completed: 4,
    rendered: 4,
    ready: 4,
    pending: false,
    active: false,
    resultReady: false,
  });
  expect(settled.thumbnails?.publicationFrames).toHaveLength(4);
  const publicationFrames = settled.thumbnails?.publicationFrames || [];
  for (let index = 1; index < publicationFrames.length; ++index) {
    expect(publicationFrames[index]).toBeGreaterThan(publicationFrames[index - 1]);
  }
  expect(settled.deviceState).toEqual(initialDeviceState);

  const thumbnailSamples = [
    { id: "donner-splash", column: 0, row: 0 },
    { id: "basic-shapes", column: 1, row: 0 },
    { id: "text-style", column: 2, row: 0 },
    { id: "gradients-clip", column: 0, row: 1 },
  ] as const;
  if (kBackend === "geode") {
    expect(geodeThumbnailStats).toHaveLength(thumbnailSamples.length);
    const fingerprints = new Set<number>();
    for (let index = 0; index < thumbnailSamples.length; ++index) {
      const sample = thumbnailSamples[index];
      const stats = geodeThumbnailStats?.[index];
      expect(stats, `${sample.id} should have final-frame WGPU readback stats`).toBeDefined();
      if (!stats) {
        continue;
      }
      expect(stats.maxChannel, `${sample.id} thumbnail should contain visible source pixels`)
        .toBeGreaterThan(80);
      expect(stats.coloredPixels, `${sample.id} thumbnail should not be a flat placeholder`)
        .toBeGreaterThan(32);
      expect(stats.coloredPixels, `${sample.id} thumbnail should contain nonuniform source art`)
        .toBeLessThan(stats.samples - 32);
      fingerprints.add(stats.fingerprint);
    }
    expect(fingerprints.size, "all four card interiors should contain distinct source art").toBe(4);
    expect(geodeThumbnailStats?.[2].backgroundPixels).toBeGreaterThan(1000);
    expect(geodeThumbnailStats?.[2].glyphPixels).toBeGreaterThan(20);
  } else {
    const canvasBounds = await page.locator("canvas#canvas").boundingBox();
    expect(canvasBounds).not.toBeNull();
    if (canvasBounds === null) {
      return;
    }

    // Mirror the current 1600px-wide welcome layout. Probe inside the 104x64 thumbnail slots so
    // card borders, labels, and hover states cannot make a missing/placeholder thumbnail pass.
    const pickerContentWidth = Math.min(920, canvasBounds.width - 32);
    const pickerContentLeft = Math.max(32, (canvasBounds.width - pickerContentWidth) * 0.5);
    const gridGap = 12;
    const cardWidth = (pickerContentWidth - gridGap * 2) / 3;
    const thumbnailSlotInset = 8;
    const thumbnailWidth = 104;
    const thumbnailHeight = 64;
    const probeInset = 6;
    const rowCenters = [282, 390] as const;
    const thumbnailCaptures: Array<{
      id: string;
      png: Buffer;
      region: { x: number; y: number; width: number; height: number };
    }> = [];
    for (const sample of thumbnailSamples) {
      const region = {
        x: pickerContentLeft + sample.column * (cardWidth + gridGap) + thumbnailSlotInset
          + probeInset,
        y: rowCenters[sample.row] - thumbnailHeight * 0.5 + probeInset,
        width: thumbnailWidth - probeInset * 2,
        height: thumbnailHeight - probeInset * 2,
      };
      const stats = await readCanvasColorStats(page, region);
      const png = await page.screenshot({
        clip: {
          x: canvasBounds.x + region.x,
          y: canvasBounds.y + region.y,
          width: region.width,
          height: region.height,
        },
      });
      await test.info().attach(`carousel-thumbnail-${sample.id}`, {
        body: png,
        contentType: "image/png",
      });

      expect(stats.maxChannel, `${sample.id} thumbnail should contain visible source pixels`)
        .toBeGreaterThan(80);
      expect(stats.coloredPixels, `${sample.id} thumbnail should not be a flat placeholder`)
        .toBeGreaterThan(32);
      expect(stats.coloredPixels, `${sample.id} thumbnail should contain nonuniform source art`)
        .toBeLessThan(stats.samples - 32);
      expect(png.byteLength, `${sample.id} thumbnail should not compress like a uniform well`)
        .toBeGreaterThan(400);
      thumbnailCaptures.push({ id: sample.id, png, region });
    }

    for (let left = 0; left < thumbnailCaptures.length; ++left) {
      for (let right = left + 1; right < thumbnailCaptures.length; ++right) {
        expect(
          thumbnailCaptures[left].png.equals(thumbnailCaptures[right].png),
          `${thumbnailCaptures[left].id} and ${
            thumbnailCaptures[right].id
          } need distinct source art`,
        ).toBe(false);
      }
    }

    const textStyleCapture = thumbnailCaptures.find((capture) => capture.id === "text-style");
    expect(textStyleCapture).toBeDefined();
    if (textStyleCapture === undefined) {
      return;
    }
    const textStyle = await readTextStyleGlyphStats(page, textStyleCapture.region);
    expect(textStyle.backgroundPixels).toBeGreaterThan(1000);
    expect(textStyle.glyphPixels).toBeGreaterThan(20);
  }

  // Once the last result is uploaded, the worker and event-driven main loop must park. The
  // quiet window crosses the former recurring 400-500 ms update interval, so a loop that kept
  // re-rendering could never satisfy it; polling for the window instead of comparing against an
  // earlier snapshot tolerates the bounded tail of frames the capture readbacks above run.
  await expect
    .poll(
      async () => {
        const first = await page.evaluate(() => window.__donnerMainLoopRenderedFrames || 0);
        await page.waitForTimeout(650);
        const second = await page.evaluate(() => window.__donnerMainLoopRenderedFrames || 0);
        return second - first;
      },
      {
        message: "expected the main loop to park once thumbnails settled",
        timeout: scaledMs(8000),
        intervals: [50],
      },
    )
    .toBe(0);
  expect(fatalMessages).toEqual([]);
});

test("WGPU diagnostics do not block the first carousel interaction", async ({ page }) => {
  test.skip(kBackend !== "geode", "WGPU readback diagnostics are Geode-specific");
  const fatalMessages = await openEditor(page, {
    wgpuReadbackStats: true,
    postInitializationDwellMs: 0,
  });
  const canvas = page.locator("canvas#canvas");
  const bounds = await canvas.boundingBox();
  expect(bounds).not.toBeNull();
  if (bounds === null) {
    return;
  }

  await expect
    .poll(async () => page.evaluate(() => window.__donnerWgpuReadbackStats?.frame || 0), {
      message: "expected the first asynchronous WGPU diagnostic frame",
      timeout: scaledMs(2000),
    })
    .toBeGreaterThan(0);
  await expect
    .poll(async () =>
      page.evaluate(() => {
        const thumbnails = window.__donnerSampleThumbnailStats;
        return {
          active: thumbnails?.active ?? true,
          pending: thumbnails?.pending ?? true,
          ready: thumbnails?.ready ?? 0,
          resultReady: thumbnails?.resultReady ?? true,
        };
      }), {
      message: "expected all asynchronous carousel thumbnails to settle before the idle probe",
      timeout: scaledMs(5000),
      intervals: [0, 50, 100],
    })
    .toEqual({
      active: false,
      pending: false,
      ready: 4,
      resultReady: false,
    });
  await expect
    .poll(async () => {
      const before = await page.evaluate(() => window.__donnerMainLoopRenderedFrames || 0);
      // This exceeds the old 400-500 ms recurring cadence, so continuous rendering never reaches
      // the quiescent state. The state barrier above prevents a quiet gap between thumbnail
      // completions from being mistaken for final startup quiescence.
      await page.waitForTimeout(550);
      const after = await page.evaluate(() => window.__donnerMainLoopRenderedFrames || 0);
      return after - before;
    }, {
      message: "expected the Wasm main loop to become event-driven after startup",
      timeout: scaledMs(5000),
      intervals: [0, 50, 100],
    })
    .toBe(0);
  const initialActivity = await page.evaluate(() => ({
    completions: window.__donnerWgpuReadbackCaptureCompletions || 0,
    frame: window.__donnerWgpuReadbackStats?.frame || 0,
    mainFrames: window.__donnerMainLoopRenderedFrames || 0,
    starts: window.__donnerWgpuReadbackCaptureStarts || 0,
  }));
  expect(initialActivity.starts).toBeGreaterThan(0);
  expect(initialActivity.completions).toBeGreaterThan(0);
  expect(initialActivity.mainFrames).toBeGreaterThan(0);
  // Dwell beyond several old periods and observe capture starts directly. A publication-only
  // check could miss an idle copy/map that failed.
  await page.waitForTimeout(1500);
  expect(
    await page.evaluate(() => ({
      completions: window.__donnerWgpuReadbackCaptureCompletions || 0,
      frame: window.__donnerWgpuReadbackStats?.frame || 0,
      mainFrames: window.__donnerMainLoopRenderedFrames || 0,
      starts: window.__donnerWgpuReadbackCaptureStarts || 0,
    })),
  ).toEqual(initialActivity);
  await page.evaluate(() => {
    const heartbeat = {
      lastTickMs: performance.now(),
      maxGapMs: 0,
      ticks: 0,
      intervalId: 0,
    };
    heartbeat.intervalId = window.setInterval(() => {
      const now = performance.now();
      heartbeat.maxGapMs = Math.max(heartbeat.maxGapMs, now - heartbeat.lastTickMs);
      heartbeat.lastTickMs = now;
      heartbeat.ticks += 1;
    }, 10);
    (window as Window & { __donnerDiagnosticHeartbeat?: typeof heartbeat })
      .__donnerDiagnosticHeartbeat = heartbeat;
  });

  await page.mouse.click(bounds.x + bounds.width * 0.5, bounds.y + 282);
  await expect(canvas).toHaveAttribute("data-active-sample-id", "basic-shapes", {
    timeout: scaledMs(1000),
  });
  const carouselHeartbeatGapMs = await page.evaluate(() => {
    const state = (window as Window & {
      __donnerDiagnosticHeartbeat?: { lastTickMs: number; maxGapMs: number };
    }).__donnerDiagnosticHeartbeat;
    if (!state) {
      return 0;
    }
    const maxGapMs = state.maxGapMs;
    state.lastTickMs = performance.now();
    state.maxGapMs = 0;
    return maxGapMs;
  });
  const diagnosticCompleted = async (request: number) => {
    return (await page.evaluate(() => window.__donnerWgpuReadbackStats?.request || 0)) >= request;
  };
  const postClickRequest = await requestWgpuDiagnostic(page);
  await expect
    .poll(() => diagnosticCompleted(postClickRequest), {
      message: "expected a post-click WGPU diagnostic frame",
      timeout: scaledMs(5000),
      intervals: [16, 25, 50, 100],
    })
    .toBe(true);
  await expect
    .poll(async () => {
      const before = await page.evaluate(() => window.__donnerMainLoopRenderedFrames || 0);
      await page.waitForTimeout(550);
      const after = await page.evaluate(() => window.__donnerMainLoopRenderedFrames || 0);
      return after - before;
    }, {
      message: "expected the Wasm main loop to become idle after loading the sample",
      timeout: scaledMs(5000),
      intervals: [0, 50, 100],
    })
    .toBe(0);
  const secondRequest = await requestWgpuDiagnostic(page);
  await expect
    .poll(() => diagnosticCompleted(secondRequest), {
      message: "expected an idle diagnostic request to wake the Wasm main loop",
      timeout: scaledMs(5000),
      intervals: [16, 25, 50, 100],
    })
    .toBe(true);
  const heartbeat = await page.evaluate(() => {
    const state = (window as Window & {
      __donnerDiagnosticHeartbeat?: {
        maxGapMs: number;
        ticks: number;
        intervalId: number;
      };
    }).__donnerDiagnosticHeartbeat;
    if (state) {
      window.clearInterval(state.intervalId);
    }
    return state;
  });
  expect(heartbeat?.ticks).toBeGreaterThan(0);
  console.log(
    `diagnostic-heartbeat carousel=${carouselHeartbeatGapMs.toFixed(1)}ms readback=${
      (heartbeat?.maxGapMs || 0).toFixed(1)
    }ms`,
  );
  expect(carouselHeartbeatGapMs).toBeLessThan(100);
  expect(heartbeat?.maxGapMs).toBeLessThan(100);
  expect(fatalMessages).toEqual([]);
});

for (
  const sample of [
    { id: "donner-splash", name: "Donner Splash", xFraction: 0.24, y: 282 },
    { id: "basic-shapes", name: "Basic Shapes", xFraction: 0.5, y: 282 },
    { id: "text-style", name: "Text and Style", xFraction: 0.76, y: 282 },
    { id: "gradients-clip", name: "Gradients and Clip", xFraction: 0.24, y: 390 },
  ] as const
) {
  test(`carousel loads ${sample.name} on the first interactive frame`, async ({ page }) => {
    const fatalMessages = await openEditor(page, { postInitializationDwellMs: 0 });
    const canvas = page.locator("canvas#canvas");
    const bounds = await canvas.boundingBox();
    expect(bounds).not.toBeNull();
    if (bounds === null) {
      return;
    }

    const deviceCreationsBeforeClick = await page.evaluate(
      () => window.__donnerHeadlessDeviceCreations ?? -1,
    );
    const beforeSample = await page.evaluate(
      () => window.__donnerWorkerStats?.completedResults || 0,
    );
    const beforeFrames = await page.evaluate(() => window.__donnerMainLoopRenderedFrames || 0);
    const clickStartedAt = await page.evaluate(() => performance.now());
    await page.mouse.click(bounds.x + bounds.width * sample.xFraction, bounds.y + sample.y);
    const carouselDeadlineMs = scaledMs(300);
    const phaseTimings: {
      activatedMs?: number;
      submittedMs?: number;
      dequeuedMs?: number;
      startedMs?: number;
      completedMs?: number;
      polledMs?: number;
      presentedMs?: number;
    } = {};
    let completedWorkerStats: Window["__donnerWorkerStats"] | undefined;
    // Presentation is now one canvas frame: the sample is on screen once the
    // document render lands AND the app thread has drawn a frame carrying it.
    // `presentedMs` is read on the page clock at that observation.
    const presentationReady = async () => {
      const state = await page.evaluate(() => ({
        activeSample: window.__donnerActiveSampleStats,
        frames: window.__donnerMainLoopRenderedFrames || 0,
        nowMs: performance.now(),
        worker: window.__donnerWorkerStats,
      }));
      if (state.activeSample?.sampleId === sample.id) {
        phaseTimings.activatedMs = state.activeSample.activatedAtMs - clickStartedAt;
      }
      const completedResults = state.worker?.completedResults || 0;
      if (state.worker && completedResults > beforeSample) {
        completedWorkerStats = state.worker;
        phaseTimings.polledMs = state.worker.publishedAtMs - clickStartedAt;
        phaseTimings.completedMs = phaseTimings.polledMs - state.worker.pollDelayMs;
        phaseTimings.startedMs = phaseTimings.completedMs - state.worker.workerMs;
        phaseTimings.dequeuedMs = phaseTimings.startedMs - state.worker.dequeueToStartMs;
        phaseTimings.submittedMs = phaseTimings.dequeuedMs - state.worker.queueWaitMs;
      }
      const pixelsPresented = state.activeSample?.sampleId === sample.id
        && completedResults > beforeSample
        && state.frames > beforeFrames;
      if (pixelsPresented) {
        phaseTimings.presentedMs ??= state.nowMs - clickStartedAt;
      }
      return pixelsPresented;
    };
    try {
      await expect.poll(presentationReady, {
        message: `expected ${sample.name} to present a document frame in the editor canvas`,
        timeout: scaledMs(3000),
        intervals: [8, 16, 25, 50],
      }).toBe(true);
    } finally {
      console.log(
        `carousel-presentation sample=${sample.id} timings=${
          JSON.stringify(phaseTimings)
        } worker=${JSON.stringify(completedWorkerStats)}`,
      );
    }
    expect(phaseTimings.presentedMs).toBeLessThan(carouselDeadlineMs);
    if (kBackend === "geode") {
      expect(await page.evaluate(() => window.__donnerHeadlessDeviceCreations)).toBe(
        deviceCreationsBeforeClick,
      );
    }
    expect(fatalMessages).toEqual([]);
  });
}

test("Text and Style renders embedded glyphs without font requests", async ({ page }) => {
  const fontNetworkRequests: string[] = [];
  page.on("request", (request) => {
    const pathname = new URL(request.url()).pathname.toLowerCase();
    if (
      request.resourceType() === "font"
      || /\.(?:ttf|otf|woff2?)(?:$|\/)/i.test(pathname)
    ) {
      fontNetworkRequests.push(request.url());
    }
  });
  const fatalMessages = await openEditor(page, { postInitializationDwellMs: 0 });
  const fontRequests = () =>
    page.evaluate(() =>
      performance
        .getEntriesByType("resource")
        .map((entry) => entry.name)
        .filter((name) => {
          const pathname = new URL(name).pathname.toLowerCase();
          return pathname.includes("/fonts/") || /\.(?:ttf|otf|woff2?)$/.test(pathname);
        })
    );
  expect(await fontRequests()).toEqual([]);
  expect(fontNetworkRequests).toEqual([]);

  const canvas = page.locator("canvas#canvas");
  const bounds = await canvas.boundingBox();
  expect(bounds).not.toBeNull();
  if (bounds === null) {
    return;
  }
  const beforeSample = await page.evaluate(
    () => window.__donnerWorkerStats?.completedResults || 0,
  );
  await page.mouse.click(bounds.x + bounds.width * 0.76, bounds.y + 282);
  await expect(canvas).toHaveAttribute("data-active-sample-id", "text-style", { timeout: 1000 });
  await expect
    .poll(async () => page.evaluate(() => window.__donnerWorkerStats?.completedResults || 0), {
      message: "expected the Text and Style sample to finish presenting",
      timeout: scaledMs(2000),
      intervals: [16, 25, 50, 100],
    })
    .toBeGreaterThan(beforeSample);
  // Glyph evidence comes from the render pane's own pixels in the single
  // canvas: the document background and the antialiased glyph coverage on top
  // of it must both be present, which is what a missing embedded font breaks.
  const textStats = await readTextStyleGlyphStats(page, {
    x: kSourcePaneWidth + 20,
    y: 80,
    width: 1600 - kSourcePaneWidth - kRightPaneWidth - 40,
    height: 680,
  });
  expect(textStats.backgroundPixels).toBeGreaterThan(10_000);
  expect(textStats.glyphPixels).toBeGreaterThan(200);
  expect(await fontRequests()).toEqual([]);
  expect(fontNetworkRequests).toEqual([]);
  expect(fatalMessages).toEqual([]);
});

test("browser presents the first Basic Shapes drag frame within the interaction budget", async ({ page }) => {
  const fatalMessages = await openEditor(page, {
    postInitializationDwellMs: 0,
    wgpuReadbackStats: kBackend === "geode",
  });
  const canvas = page.locator("canvas#canvas");
  const bounds = await canvas.boundingBox();
  expect(bounds).not.toBeNull();
  if (bounds === null) {
    return;
  }

  const beforeSample = await page.evaluate(
    () => window.__donnerWorkerStats?.completedResults || 0,
  );
  await page.mouse.click(bounds.x + bounds.width * 0.5, bounds.y + 282);
  await expect(canvas).toHaveAttribute("data-active-sample-id", "basic-shapes", { timeout: 1000 });
  await expect
    .poll(async () => page.evaluate(() => window.__donnerWorkerStats?.completedResults || 0), {
      message: "expected the Basic Shapes document to finish its first presentation",
      timeout: scaledMs(1000),
      intervals: [16, 25, 50, 100],
    })
    .toBeGreaterThan(beforeSample);

  const blueRectCenter = {
    x: bounds.x + kBlueRectOffset.x,
    y: bounds.y + kBlueRectOffset.y,
  };
  if (kBackend === "geode") {
    // Selecting a shape must draw chrome without re-rasterizing the document.
    // Start the click from a quiesced presentation so no leftover load render
    // can move the chrome out from under the probe.
    const beforeSelectionResult = await waitForPresentationQuiescence(page, {
      message: "expected the Basic Shapes presentation to settle before selecting a shape",
    });
    const baselineRequest = await requestWgpuDiagnostic(page);
    await expect
      .poll(async () => page.evaluate(() => window.__donnerWgpuReadbackStats?.request || 0), {
        message: "expected a diagnostic capture before shape selection",
        timeout: scaledMs(2000),
        intervals: [16, 25, 50, 100],
      })
      .toBeGreaterThanOrEqual(baselineRequest);
    const beforeSelectionChrome = await page.evaluate(
      () => window.__donnerWgpuReadbackStats?.selectionChromePixels || 0,
    );
    await page.mouse.click(blueRectCenter.x, blueRectCenter.y);
    await expect
      .poll(async () => page.evaluate(() => window.__donnerInteractionStats?.selectedCount || 0), {
        message: "expected shape selection before capturing its overlay",
        timeout: scaledMs(2000),
        intervals: [16, 25, 50, 100],
      })
      .toBeGreaterThan(0);
    const selectionRequest = await requestWgpuDiagnostic(page);
    await expect
      .poll(async () => page.evaluate(() => window.__donnerWgpuReadbackStats?.request || 0), {
        message: "expected a diagnostic capture after shape selection",
        timeout: scaledMs(2000),
        intervals: [16, 25, 50, 100],
      })
      .toBeGreaterThanOrEqual(selectionRequest);
    let selectionChromeDelta = 0;
    await expect
      .poll(
        async () => {
          const snapshot = await page.evaluate(() => ({
            chrome: window.__donnerWgpuReadbackStats?.selectionChromePixels || 0,
            completed: window.__donnerWorkerStats?.completedResults || 0,
          }));
          selectionChromeDelta = snapshot.chrome - beforeSelectionChrome;
          if (selectionChromeDelta > 50) {
            return "selection-chrome";
          }
          // The completed capture may predate the first frame that drew the
          // chrome on slow hosts; request a fresh diagnostic for the next read
          // instead of polling a stale snapshot forever.
          await requestWgpuDiagnostic(page);
          return "waiting";
        },
        {
          message: "expected selection chrome pixels before starting the drag",
          timeout: scaledMs(2000),
          intervals: [100, 250, 500],
        },
      )
      .not.toBe("waiting");
    expect(selectionChromeDelta).toBeGreaterThan(50);
    // A selection click intentionally schedules exactly one document render to prewarm the
    // selected layer's textures so the first drag frame presents immediately. Polling for the
    // exact +1 keeps the contract tight: a second render never matches and times out here.
    await expect
      .poll(async () => page.evaluate(() => window.__donnerWorkerStats?.completedResults || 0), {
        message: "expected the selection click's single prewarm render and nothing further",
        timeout: scaledMs(2000),
        intervals: [16, 25, 50, 100],
      })
      .toBe(beforeSelectionResult + 1);
  }
  await page.mouse.move(blueRectCenter.x, blueRectCenter.y);
  await page.mouse.down();

  // The 125 ms interaction budget is calibrated for local development
  // hardware. Shared CI runners are slower and the poll below adds up to one
  // interval of measurement quantization, so CI enforces a looser bound that
  // still fails on genuine stalls.
  const firstDragFrameBudgetMs = scaledMs(125);
  // The first visible drag frame is a UI frame presenting the prewarmed selected-layer
  // texture at its moved position; the worker does not re-rasterize during the drag, so the
  // budget measures the frame counter, not the document counter.
  const beforeMoveFrames = await page.evaluate(
    () => window.__donnerMainLoopRenderedFrames || 0,
  );
  const dragMoveStartedAt = Date.now();
  await page.mouse.move(blueRectCenter.x + 120, blueRectCenter.y + 70);
  await expect
    .poll(async () => {
      const frames = await page.evaluate(
        () => window.__donnerMainLoopRenderedFrames || 0,
      );
      return frames > beforeMoveFrames;
    }, {
      message: "expected the shape's first visible drag frame while the pointer remained down",
      timeout: firstDragFrameBudgetMs,
      intervals: [8, 16, 25, 33, 50],
    })
    .toBe(true);
  const firstDragFrameMs = Date.now() - dragMoveStartedAt;
  console.log(`first-drag-frame-ms=${firstDragFrameMs}`);
  expect(firstDragFrameMs).toBeLessThan(firstDragFrameBudgetMs);
  await page.mouse.up();
  expect(fatalMessages).toEqual([]);
});

test("Firefox keeps Basic Shapes resize pixels and outline synchronized", async ({ browserName, page }) => {
  test.skip(browserName !== "firefox" || kBackend !== "geode", "Firefox Geode regression");
  const fatalMessages = await openEditor(page, { postInitializationDwellMs: 0 });
  const canvas = page.locator("canvas#canvas");
  const bounds = await canvas.boundingBox();
  expect(bounds).not.toBeNull();
  if (bounds === null) {
    return;
  }

  const beforeSample = await page.evaluate(
    () => window.__donnerWorkerStats?.completedResults || 0,
  );
  await page.mouse.click(bounds.x + bounds.width * 0.5, bounds.y + 282);
  await expect(canvas).toHaveAttribute("data-active-sample-id", "basic-shapes", { timeout: 1000 });
  await expect
    .poll(async () => page.evaluate(() => window.__donnerWorkerStats?.completedResults || 0), {
      message: "expected Basic Shapes to finish presenting before resize",
      timeout: scaledMs(1000),
      intervals: [16, 25, 50, 100],
    })
    .toBeGreaterThan(beforeSample);

  await expect
    .poll(async () => (await readElementColorStats(canvas)).coloredPixels, {
      message: "expected visible Basic Shapes pixels before selecting the resize target",
      timeout: scaledMs(1000),
      intervals: [16, 25, 50, 100],
    })
    .toBeGreaterThan(500);

  const editorBounds = await canvas.boundingBox();
  expect(editorBounds).not.toBeNull();
  if (editorBounds === null) {
    return;
  }

  const blueRectCenter = {
    x: editorBounds.x + kBlueRectOffset.x,
    y: editorBounds.y + kBlueRectOffset.y,
  };
  const resizeHandle = { x: editorBounds.x + 498, y: editorBounds.y + 413 };
  const probeRegion = {
    x: editorBounds.x + 300,
    y: editorBounds.y + 270,
    width: 360,
    height: 240,
  };
  await page.mouse.click(blueRectCenter.x, blueRectCenter.y);
  await expect
    .poll(async () =>
      (await readEditorPixelBounds(page, probeRegion, "selection-teal"))?.pixels || 0
    )
    .toBeGreaterThan(50);

  const initialPixels = await readEditorResizePixelBounds(page, probeRegion);
  expect(initialPixels.blue).not.toBeNull();
  if (initialPixels.blue === null) {
    return;
  }
  // A live resize presents like a live drag: the press prewarms the selected
  // layer, and each pointer move presents by transforming that texture inside a
  // UI frame instead of re-rasterizing the document. So "the gesture is not
  // starved" is a claim about presented frames - every move produces one while
  // the pointer is still down - and the document commit is asserted after the
  // release below. Start from an idle worker anyway: the selection click above
  // schedules the prewarm render, and a burst that begins while it is in flight
  // is measuring the test's own timing.
  const initialResults = await waitForPresentationQuiescence(page, {
    message: "resize burst",
  });
  const initialFrames = await page.evaluate(() => window.__donnerMainLoopRenderedFrames || 0);
  await page.mouse.move(resizeHandle.x, resizeHandle.y);
  await page.mouse.down();

  // Emit a short 60 Hz-style burst. A starved gesture presents nothing until the
  // pointer stops, so the frame counter is what separates the two behaviors.
  for (let step = 1; step <= 4; ++step) {
    await page.mouse.move(resizeHandle.x + step * 28, resizeHandle.y + step * 18);
    await page.waitForTimeout(16);
  }
  await expect
    .poll(async () => page.evaluate(() => window.__donnerMainLoopRenderedFrames || 0), {
      message: "expected multiple resize frames to land during one continuous pointer burst",
      timeout: scaledMs(500),
      intervals: [16, 25, 50],
    })
    .toBeGreaterThanOrEqual(initialFrames + 2);

  await expect
    .poll(async () => {
      const { blue, teal } = await readEditorResizePixelBounds(page, probeRegion);
      if (blue === null || teal === null) {
        return false;
      }
      const reachedFinalPointer = blue.maxX >= initialPixels.blue.maxX + 100
        && blue.maxY >= initialPixels.blue.maxY + 60;
      const horizontalGap = Math.abs(teal.maxX - blue.maxX);
      const verticalGap = Math.abs(teal.maxY - blue.maxY);
      return reachedFinalPointer && horizontalGap <= 8 && verticalGap <= 8;
    }, {
      message:
        "expected the final resized pixels and selection outline from one document render",
      // The pointer is resting at its final position while this polls, so the
      // window only bounds how long presentation plus the Gecko readback
      // cadence may take to converge; the pixel claim itself is unchanged.
      // Gecko resolves readback maps in 2-19 waitAny slices where Chromium
      // takes one, which made the previous window marginal on shared runners.
      timeout: scaledMs(4000),
      intervals: [50, 100, 250],
    })
    .toBe(true);
  await page.mouse.up();
  // The release commits the resized geometry with a document render. The gesture
  // may already have committed a debounced canvas-size render of its own while
  // the pointer was down, so this is a lower bound on the settled count rather
  // than an exact one.
  await expect
    .poll(async () => page.evaluate(() => window.__donnerWorkerStats?.completedResults || 0), {
      message: "expected the pointer release to commit the resized document",
      timeout: scaledMs(2000),
      intervals: [16, 25, 50, 100],
    })
    .toBeGreaterThan(initialResults);
  expect(fatalMessages).toEqual([]);
});

test("Geode WASM selects through the overlay with one prewarm render and no recurring updates", async ({ page }) => {
  test.skip(kBackend !== "geode", "Geode worker scheduling regression is backend-specific");
  const fatalMessages = await openEditor(page, { wgpuReadbackStats: false });
  const canvas = page.locator("canvas#canvas");
  const bounds = await canvas.boundingBox();
  expect(bounds).not.toBeNull();
  if (bounds === null) {
    return;
  }

  const beforeSample = await page.evaluate(
    () => window.__donnerWorkerStats?.completedResults || 0,
  );
  await page.mouse.click(bounds.x + bounds.width * 0.5, bounds.y + 282);
  await expect
    .poll(async () => page.evaluate(() => window.__donnerWorkerStats?.completedResults || 0), {
      message: "expected the Basic Shapes sample render to finish",
      timeout: 20000,
    })
    .toBeGreaterThan(beforeSample);

  const dragStart = {
    x: bounds.x + kBlueRectOffset.x,
    y: bounds.y + kBlueRectOffset.y,
  };
  // "One result landed" is not "the load settled" - the debounced canvas-size
  // commit can still be queued. Baseline the no-rerender assertion below
  // against a quiesced presentation instead.
  const beforeSelection = await waitForPresentationQuiescence(page, {
    message: "expected the Basic Shapes presentation to settle before selecting a shape",
  });
  const beforeSelectionUiFrame = await page.evaluate(
    () => window.__donnerMainLoopRenderedFrames || 0,
  );
  const beforeSelectionAccounting = await readRenderAccounting(page);
  await page.mouse.click(dragStart.x, dragStart.y);
  await expect
    .poll(async () => page.evaluate(() => window.__donnerMainLoopRenderedFrames || 0), {
      message: "expected the selection event to produce an overlay frame",
      timeout: scaledMs(1000),
      intervals: [16, 25, 50, 100],
    })
    .toBeGreaterThan(beforeSelectionUiFrame);
  await page.waitForTimeout(1000);
  const interactionAfterClick = await page.evaluate(() => window.__donnerInteractionStats);
  console.log(`firefox-interaction-after-click=${JSON.stringify(interactionAfterClick)}`);
  console.log(`firefox-fatal-after-click=${JSON.stringify(fatalMessages)}`);
  expect(
    interactionAfterClick?.selectedCount || 0,
    "expected the canvas click to commit a semantic selection within 1 second",
  ).toBeGreaterThan(0);
  await expect
    .poll(async () => page.evaluate(() => window.__donnerInteractionStats?.pendingClick ?? true), {
      message: "expected the committed selection to consume its buffered click",
      timeout: scaledMs(1000),
      intervals: [16, 25, 50, 100],
    })
    .toBe(false);
  // The click's one intentional render is the selected-layer prewarm; anything
  // past +1 is the recurring-update regression this test exists to catch.
  //
  // A shared runner occasionally reports +2 here. The failure carries the
  // editor's own render accounting so the next occurrence is attributable
  // instead of being another timing guess: `documentCanvasCommits` and
  // `overviewInfillRenders` are the two renders the editor can owe itself and
  // land on the frame the click wakes, and quiescence above cannot exclude
  // either, because one still pending moves neither the result count nor the
  // busy flag. Measured on this build they both stay at zero through sample
  // load, selection, and pan, so a +2 that reports zero for both is neither of
  // them and the reason is still open.
  const selectionDeadline = Date.now() + scaledMs(2000);
  let selectionAccounting = await readRenderAccounting(page);
  while (
    selectionAccounting.completedResults !== beforeSelection + 1
    && Date.now() < selectionDeadline
  ) {
    await page.waitForTimeout(25);
    selectionAccounting = await readRenderAccounting(page);
  }
  expect(
    selectionAccounting.completedResults,
    `expected the selection click's single prewarm render and nothing further: `
      + `beforeSelection=${beforeSelection} `
      + `before=${JSON.stringify(beforeSelectionAccounting)} `
      + `after=${JSON.stringify(selectionAccounting)}`,
  ).toBe(beforeSelection + 1);
  await page.mouse.move(dragStart.x, dragStart.y);
  await page.mouse.down();
  // An active drag presents by transforming the prewarmed selected-layer texture inside UI
  // frames; the worker does not re-rasterize until the pointer releases. The live signal for
  // "the drag is visibly presenting" is therefore the UI frame counter, and the document
  // counter staying flat through the drag is the architecture's own contract.
  const beforeDragFrames = await page.evaluate(
    () => window.__donnerMainLoopRenderedFrames || 0,
  );
  await page.mouse.move(dragStart.x + 18, dragStart.y + 12);
  await page.mouse.move(dragStart.x + 32, dragStart.y + 20);
  await expect
    .poll(async () => page.evaluate(() => window.__donnerMainLoopRenderedFrames || 0), {
      message: "expected drag-preview frames while the shape drag is active",
      timeout: scaledMs(1000),
      intervals: [16, 25, 50, 100],
    })
    .toBeGreaterThan(beforeDragFrames);
  expect(
    await page.evaluate(() => window.__donnerWorkerStats?.completedResults || 0),
    "an active drag must not re-rasterize the document before the pointer releases",
  ).toBe(beforeSelection + 1);
  const beforeMouseUpResults = await page.evaluate(
    () => window.__donnerWorkerStats?.completedResults || 0,
  );
  await page.mouse.up();
  await expect
    .poll(async () => page.evaluate(() => window.__donnerWorkerStats?.completedResults || 0), {
      message: "expected the drag-release settlement frame",
      timeout: scaledMs(1000),
      intervals: [16, 25, 50, 100],
    })
    .toBeGreaterThan(beforeMouseUpResults);

  // Releasing a drag queues exactly one settled-selection refresh behind the
  // frame the pointer already produced, so the first post-mouse-up result is
  // not the last one. Let that bounded settle finish (it not finishing is
  // itself a failure), then assert nothing keeps re-rendering.
  const settledCount = await waitForPresentationQuiescence(page, {
    message: "expected the drag-release settle to finish",
  });
  await page.waitForTimeout(1500);
  expect(await page.evaluate(() => window.__donnerWorkerStats?.completedResults || 0)).toBe(
    settledCount,
  );
  const workerStats = await page.evaluate(() => window.__donnerWorkerStats);
  expect(workerStats).toBeDefined();
  // Presentation costs no CPU readback: Geode draws the document straight into
  // the canvas frame, so the readback counters stay at zero and the wait
  // strategy is only ever one of the two GPU-fence strategies.
  expect(workerStats?.readbackCount).toBe(0);
  expect(workerStats?.readbackPollIterations).toBe(0);
  expect(["timed-wait-any", "device-poll"]).toContain(workerStats?.readbackWaitStrategy);
  console.log(`wasm-worker-stats=${JSON.stringify(workerStats)}`);
  expect(fatalMessages).toEqual([]);
});

test("production Geode wasm presents visible editor pixels", async ({ page }) => {
  test.skip(kBackend !== "geode", "production WebGPU presentation is Geode-specific");
  // Browser screenshots omit a transferred WebGPU swapchain, and since Design
  // the single-canvas architecture the whole application lives in that one transferred canvas. Prove the
  // presented pixels with the editor's own explicit GPU readback instead.
  const fatalMessages = await openEditor(page, {
    postInitializationDwellMs: 0,
    wgpuReadbackStats: true,
  });
  const gpuDiagnostics = await readWebGpuDiagnostics(page);
  console.log(`browser-gpu-diagnostics=${JSON.stringify(gpuDiagnostics)}`);
  await test.info().attach("browser-gpu-diagnostics", {
    body: JSON.stringify(gpuDiagnostics, null, 2),
    contentType: "application/json",
  });
  const canvas = page.locator("canvas#canvas");
  await expect(canvas).toBeVisible();
  const canvasBox = await canvas.boundingBox();
  expect(canvasBox).not.toBeNull();
  if (canvasBox === null) {
    return;
  }

  await page.mouse.click(canvasBox.x + canvasBox.width * 0.5, canvasBox.y + 282);
  await expect(canvas).toHaveAttribute("data-active-sample-id", "basic-shapes", {
    timeout: 20000,
  });
  await expect
    .poll(async () => {
      await requestWgpuDiagnostic(page);
      const renderPane = await page.evaluate(
        () => window.__donnerWgpuReadbackStats?.renderPane,
      );
      return Boolean(
        renderPane
          && renderPane.nonBlackPixels > 1000
          && renderPane.coloredPixels > 500
          && renderPane.maxChannel > 80,
      );
    }, {
      message: "expected the render pane to hold Basic Shapes GPU product pixels",
      timeout: 20000,
      intervals: [16, 25, 50, 100],
    })
    .toBe(true);
  const renderPaneStats = await readRenderPaneColorStats(page);
  console.log(`render-pane-pixels=${JSON.stringify(renderPaneStats)}`);
  expect(fatalMessages).toEqual([]);
});

test("wasm editor renders layer panel previews after loading a document", async ({ page }) => {
  const fatalMessages = await openEditor(page, { wgpuReadbackStats: kBackend === "geode" });
  const canvas = page.locator("canvas#canvas");
  const bounds = await canvas.boundingBox();
  expect(bounds).not.toBeNull();
  if (bounds === null) {
    return;
  }

  const beforeSample = await page.evaluate(() => window.__donnerWorkerStats?.completedResults || 0);
  await page.mouse.click(bounds.x + bounds.width * 0.5, bounds.y + 282);
  await expect(canvas).toHaveAttribute("data-active-sample-id", "basic-shapes");
  await expect
    .poll(async () => page.evaluate(() => window.__donnerWorkerStats?.completedResults || 0), {
      message: "expected Basic Shapes to present before sampling the layer panel",
      timeout: 20000,
    })
    .toBeGreaterThan(beforeSample);
  if (kBackend === "geode") {
    const request = await requestWgpuDiagnostic(page);
    await expect
      .poll(async () => page.evaluate(() => window.__donnerWgpuReadbackStats?.request || 0), {
        message: "expected a post-load WGPU diagnostic capture for the Layers panel",
        timeout: scaledMs(5000),
        intervals: [16, 25, 50, 100],
      })
      .toBeGreaterThanOrEqual(request);
  }
  // Geode-only Wasm renders row thumbnails as GPU texture snapshots; a CPU
  // bitmap fallback would mean the texture path silently degraded.
  await expect
    .poll(
      async () =>
        page.evaluate(() => window.__donnerLayerThumbnailStats?.textureSnapshotCount || 0),
      {
        message:
          "expected the Layers panel to render SVG row thumbnails instead of fallback swatches",
        timeout: 20000,
        intervals: [16, 25, 50, 100],
      },
    )
    .toBeGreaterThan(0);
  const thumbnailStats = await page.evaluate(() => window.__donnerLayerThumbnailStats);
  expect(thumbnailStats?.bitmapCount).toBe(0);
  expect(thumbnailStats?.textureCount).toBeGreaterThan(0);
  await expect
    .poll(async () => {
      const stats = await readLayerPreviewColorStats(page);
      if (stats.coloredPixels > 100 && stats.maxChannel > 80) {
        return true;
      }
      // The stored capture may predate the thumbnail batch (thumbnails settle
      // later on slow hosts); request a fresh diagnostic for the next read
      // instead of polling a stale snapshot forever.
      await requestWgpuDiagnostic(page);
      return false;
    }, {
      message: "expected colored thumbnail pixels in the Layers panel preview column",
      timeout: 20000,
      intervals: [100, 250, 500],
    })
    .toBe(true);

  expect(fatalMessages).toEqual([]);
});

test("wasm editor drains thumbnails for expanded sublayers", async ({ page }) => {
  const fatalMessages = await openEditor(page, { wgpuReadbackStats: kBackend === "geode" });
  const canvas = page.locator("canvas#canvas");
  const bounds = await canvas.boundingBox();
  expect(bounds).not.toBeNull();
  if (bounds === null) {
    return;
  }

  await page.mouse.click(bounds.x + bounds.width * 0.25, bounds.y + 282);
  await expect(canvas).toHaveAttribute("data-active-sample-id", "donner-splash");
  await expect
    .poll(async () => {
      const stats = await page.evaluate(() => window.__donnerLayerThumbnailStats);
      if (
        !stats || stats.rowCount <= 1 || stats.deferredCount !== 0
        || stats.textureSnapshotCount !== stats.rowCount
        || stats.textureCount < stats.textureSnapshotCount
      ) {
        return 0;
      }
      return stats.rowCount;
    }, {
      message: "expected the initially visible Splash layers to finish their thumbnail batch",
      timeout: 20000,
      intervals: [16, 25, 50, 100],
    })
    .toBeGreaterThan(1);
  const initialRowCount = await page.evaluate(
    () => window.__donnerLayerThumbnailStats?.rowCount || 0,
  );

  // The top-level Splash group is expanded by default. Its Sunburst child is the second visible
  // sublayer and contains two nested children, which makes one thumbnail remain deferred after the
  // first incremental pass.
  await page.mouse.click(
    bounds.x + bounds.width - kRightPaneWidth + 35,
    bounds.y + 125,
  );
  await expect
    .poll(async () => {
      const stats = await page.evaluate(() => window.__donnerLayerThumbnailStats);
      return Boolean(
        stats
          && stats.rowCount > initialRowCount
          && stats.deferredCount === 0
          && stats.textureSnapshotCount === stats.rowCount
          && stats.textureCount >= stats.textureSnapshotCount,
      );
    }, {
      message: "expected every expanded Sunburst sublayer to publish a bitmap and Wasm texture",
      timeout: 20000,
      intervals: [16, 25, 50, 100],
    })
    .toBe(true);

  if (kBackend === "geode") {
    const request = await requestWgpuDiagnostic(page);
    await expect
      .poll(async () => page.evaluate(() => window.__donnerWgpuReadbackStats?.request || 0), {
        message: "expected a WGPU capture after the expanded sublayer thumbnails settled",
        timeout: scaledMs(5000),
        intervals: [16, 25, 50, 100],
      })
      .toBeGreaterThanOrEqual(request);
  }
  const previewStats = await readLayerPreviewColorStats(page);
  expect(previewStats.coloredPixels).toBeGreaterThan(100);
  expect(previewStats.maxChannel).toBeGreaterThan(80);
  expect(fatalMessages).toEqual([]);
});

test("Firefox renders every visible Splash layer thumbnail", async ({ browserName, page }) => {
  test.skip(browserName !== "firefox", "Firefox Geode thumbnail presentation regression");
  const fatalMessages = await openEditor(page);
  const canvas = page.locator("canvas#canvas");
  const bounds = await canvas.boundingBox();
  expect(bounds).not.toBeNull();
  if (bounds === null) {
    return;
  }

  await page.mouse.click(bounds.x + bounds.width * 0.25, bounds.y + 282);
  await expect(canvas).toHaveAttribute("data-active-sample-id", "donner-splash");
  await expect
    .poll(async () => {
      return page.evaluate(() => {
        const stats = window.__donnerLayerThumbnailStats;
        return Boolean(
          stats
            && stats.rowCount > 1
            && stats.deferredCount === 0
            && stats.renderedCount + stats.reusedCount === stats.rowCount,
        );
      });
    }, {
      message: "expected Firefox to publish every initially visible Splash thumbnail",
      timeout: 20000,
      intervals: [16, 25, 50, 100],
    })
    .toBe(true);
  const settledStats = await page.evaluate(() => window.__donnerLayerThumbnailStats);
  expect(settledStats?.bitmapCount).toBe(0);
  expect(settledStats?.textureSnapshotCount).toBe(settledStats?.rowCount);
  expect(settledStats?.textureCount).toBeGreaterThanOrEqual(
    settledStats?.textureSnapshotCount || 0,
  );

  const thumbnailRegions = [
    {
      name: "Sunburst",
      x: bounds.width - kRightPaneWidth + 60,
      y: 112,
      width: 30,
      height: 26,
    },
    {
      name: "Blue_center_burst",
      x: bounds.width - kRightPaneWidth + 60,
      y: 172,
      width: 30,
      height: 26,
    },
  ];
  for (const region of thumbnailRegions) {
    const stats = await readCanvasColorStats(page, region);
    expect(stats.coloredPixels, `${region.name} must show its rendered SVG thumbnail`)
      .toBeGreaterThan(
        20,
      );
  }
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

  const browserDefaultAllowed = await page.evaluate(({ x, y }) => {
    const target = document.getElementById("canvas");
    if (!target) {
      throw new Error("canvas not found");
    }

    return target.dispatchEvent(
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

  expect(browserDefaultAllowed).toBe(false);

  await expect
    .poll(async () => page.evaluate(() => window.__donnerLastScrollEvent?.zoomModifierHeld))
    .toBe(true);
  await expect
    .poll(async () => page.evaluate(() => window.__donnerLastScrollEvent?.yoffset))
    .toBeGreaterThan(0);
  expect(fatalMessages).toEqual([]);
});

// The render-pane classifier applies `zoomFactor = pow(kWheelZoomStep, yoffset)`,
// and the Wasm GLFW port converts a DOM_DELTA_PIXEL wheel event into
// `yoffset = -deltaY / kWasmWheelPixelsPerScrollUnit`. Both constants are owned
// by donner/editor/PinchZoomPolicy.h; these mirrors let the browser tests assert
// magnitude instead of sign.
const kWheelZoomStep = 1.1;
const kWasmWheelPixelsPerScrollUnit = 100;

async function readScrollEventYOffset(page: Page, previousCount: number): Promise<number> {
  await expect
    .poll(async () => page.evaluate(() => window.__donnerLastScrollEvent?.count ?? 0), {
      message: "expected the synthesized wheel event to reach the editor scroll callback",
      timeout: 10000,
    })
    .toBeGreaterThan(previousCount);
  const scrollEvent = await page.evaluate(() => window.__donnerLastScrollEvent);
  expect(scrollEvent?.zoomModifierHeld).toBe(true);
  expect(typeof scrollEvent?.yoffset).toBe("number");
  return scrollEvent?.yoffset as number;
}

test("WASM trackpad pinch gesture zooms by the gesture scale", async ({ page }) => {
  const fatalMessages = await openEditor(page);
  const canvas = page.locator("canvas#canvas");
  const bounds = await canvas.boundingBox();
  expect(bounds).not.toBeNull();
  if (bounds === null) {
    return;
  }

  // The editor publishes the C++-owned pinch policy at Wasm startup; the page's
  // WebKit gesture bridge must be driving off that value, not a stale literal.
  // It is the UNGAINED wire shape (100 CSS px per unit of ln(scale)), identical
  // to what Chromium and Gecko synthesize natively: the single pinch gain lives
  // in the editor's input-bridge discriminator, and pre-gaining here too is the
  // Safari "zoom far too fast" regression. See donner/editor/PinchZoomPolicy.h.
  const publishedDeltaPerLnScale = await page.evaluate(() =>
    window.__donnerPinchWheelDeltaPerLnScale
  );
  expect(publishedDeltaPerLnScale).toBeCloseTo(kWasmWheelPixelsPerScrollUnit, 6);

  const center = {
    x: bounds.x + bounds.width * 0.5,
    y: bounds.y + bounds.height * 0.5,
  };
  await page.mouse.move(center.x, center.y);
  const beforeCount = await page.evaluate(() => window.__donnerLastScrollEvent?.count ?? 0);

  // WebKit's non-standard GestureEvent has no cross-browser constructor, so
  // synthesize the shape the bridge reads: a cancelable event carrying `scale`.
  //
  // Keep this strictly under the discriminator's 1.5x per-event clamp. A
  // gesture of exactly 1.5 is indistinguishable from a clamped runaway: the
  // double-gain regression that made Safari pinch unusable pinned EVERY
  // gesture to the clamp, so a 1.5 probe passed while real pinches were
  // roughly 10.5x too fast in log space.
  const gestureScale = 1.2;
  expect(gestureScale).toBeLessThan(1.5);
  await page.evaluate(({ x, y, scale }) => {
    const target = document.getElementById("canvas");
    if (!target) {
      throw new Error("canvas not found");
    }

    const dispatchGesture = (type: string, gestureScale: number) => {
      const event = new Event(type, { bubbles: true, cancelable: true });
      Object.assign(event, { scale: gestureScale, rotation: 0, clientX: x, clientY: y });
      target.dispatchEvent(event);
    };

    dispatchGesture("gesturestart", 1);
    dispatchGesture("gesturechange", scale);
    dispatchGesture("gestureend", scale);
  }, { ...center, scale: gestureScale });

  const yoffset = await readScrollEventYOffset(page, beforeCount);

  // A pinch to `scale` must zoom the document by exactly `scale`, matching the
  // native macOS pinch path. In scroll units that is ln(scale) / ln(1.1).
  const expectedYOffset = Math.log(gestureScale) / Math.log(kWheelZoomStep);
  expect(
    Math.abs(yoffset - expectedYOffset) / expectedYOffset,
    `pinch to ${gestureScale} must yield yoffset ${expectedYOffset}, got ${yoffset}`,
  ).toBeLessThan(0.02);
  expect(Math.pow(kWheelZoomStep, yoffset)).toBeCloseTo(gestureScale, 2);
  expect(fatalMessages).toEqual([]);
});

test("WASM ctrl+wheel pinch zooms by the shared wheel-zoom policy", async ({ page }) => {
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
  const beforeCount = await page.evaluate(() => window.__donnerLastScrollEvent?.count ?? 0);

  // Chromium and Firefox deliver trackpad pinch on this raw ctrl+wheel channel
  // rather than through gesture events, so pin the pixels-to-scroll-unit half
  // of the policy directly.
  const deltaY = -250;
  const browserDefaultAllowed = await page.evaluate(({ x, y, deltaY }) => {
    const target = document.getElementById("canvas");
    if (!target) {
      throw new Error("canvas not found");
    }

    return target.dispatchEvent(
      new WheelEvent("wheel", {
        bubbles: true,
        cancelable: true,
        clientX: x,
        clientY: y,
        ctrlKey: true,
        deltaMode: WheelEvent.DOM_DELTA_PIXEL,
        deltaY,
      }),
    );
  }, { ...center, deltaY });
  expect(browserDefaultAllowed).toBe(false);

  const yoffset = await readScrollEventYOffset(page, beforeCount);

  // A ctrl-flagged wheel with no physically held modifier is discriminated as
  // a browser-synthesized trackpad pinch and receives the desktop pinch
  // calibration: units scale by 1/ln(kWheelZoomStep) so the applied zoom is
  // exp(-deltaY/100), then clamp to one 1.5x step per event. deltaY=-250
  // exceeds the clamp, pinning both the gain and the safety bound.
  const rawUnits = -deltaY / kWasmWheelPixelsPerScrollUnit;
  const gained = rawUnits / Math.log(kWheelZoomStep);
  const maxUnits = Math.log(1.5) / Math.log(kWheelZoomStep);
  const expectedYOffset = Math.min(gained, maxUnits);
  expect(
    Math.abs(yoffset - expectedYOffset) / expectedYOffset,
    `discriminated pinch deltaY ${deltaY} must yield clamped yoffset ${expectedYOffset}, got ${yoffset}`,
  ).toBeLessThan(0.02);
  expect(fatalMessages).toEqual([]);
});

test("Geode WASM presents selection path overlay pixels", async ({ page }) => {
  test.skip(kBackend !== "geode", "direct selection overlay is Geode-specific");
  const fatalMessages = await openEditor(page, { wgpuReadbackStats: true });
  const canvas = page.locator("canvas#canvas");
  const bounds = await canvas.boundingBox();
  expect(bounds).not.toBeNull();
  if (bounds === null) {
    return;
  }

  const beforeSampleResult = await page.evaluate(
    () => window.__donnerWorkerStats?.completedResults || 0,
  );
  await page.mouse.click(bounds.x + bounds.width * 0.5, bounds.y + 282);
  await expect(canvas).toHaveAttribute("data-active-sample-id", "basic-shapes");
  await expect
    .poll(async () => page.evaluate(() => window.__donnerWorkerStats?.completedResults || 0), {
      message: "expected the Basic Shapes worker result before reading diagnostics",
      timeout: 20000,
    })
    .toBeGreaterThan(beforeSampleResult);
  const coloredPixel = {
    x: bounds.x + kBlueRectOffset.x,
    y: bounds.y + kBlueRectOffset.y,
  };
  const sampleRequest = await requestWgpuDiagnostic(page);
  await page.mouse.move(coloredPixel.x - 4, coloredPixel.y - 4);
  await expect
    .poll(async () => page.evaluate(() => window.__donnerWgpuReadbackStats?.request || 0), {
      message: "expected a diagnostic capture for the settled Basic Shapes render",
      timeout: 20000,
    })
    .toBeGreaterThanOrEqual(sampleRequest);
  const beforeSelectionChrome = await page.evaluate(
    () => window.__donnerWgpuReadbackStats?.selectionChromePixels || 0,
  );
  await page.mouse.click(coloredPixel.x, coloredPixel.y);
  await expect
    .poll(async () => page.evaluate(() => window.__donnerInteractionStats?.selectedCount || 0), {
      message: "expected shape selection before capturing its overlay",
      timeout: scaledMs(2000),
      intervals: [16, 25, 50, 100],
    })
    .toBeGreaterThan(0);
  const selectionRequest = await requestWgpuDiagnostic(page);
  await page.mouse.move(coloredPixel.x + 4, coloredPixel.y + 4);
  await expect
    .poll(async () => page.evaluate(() => window.__donnerWgpuReadbackStats?.request || 0), {
      message: "expected a diagnostic capture requested after selection",
      timeout: 20000,
    })
    .toBeGreaterThanOrEqual(selectionRequest);
  try {
    await expect
      .poll(
        async () =>
          (await page.evaluate(
            () => window.__donnerWgpuReadbackStats?.selectionChromePixels || 0,
          )) - beforeSelectionChrome,
        {
          message: "expected selection outline, bounds, and handle pixels in the document pane",
          timeout: 20000,
        },
      )
      .toBeGreaterThan(50);
  } catch (error) {
    console.log(`selection-after=${
      JSON.stringify(
        await page.evaluate(() => ({
          readback: window.__donnerWgpuReadbackStats,
          worker: window.__donnerWorkerStats,
        })),
      )
    }`);
    throw error;
  }

  expect(fatalMessages).toEqual([]);
});
