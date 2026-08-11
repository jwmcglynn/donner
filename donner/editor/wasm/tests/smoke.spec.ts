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
    __donnerCanStartWasm?: boolean;
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
      presentMs: number;
      diagnosticsMs: number;
      pollDelayMs: number;
      taskBoundaryMs: number;
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
      directSurfaceFrames: number;
    };
    __donnerWorkerRuntimeStats?: {
      ready: boolean;
      initializationMs: number;
      maskPipelineMs: number;
      initializationCount: number;
      workerDeviceCreations: number;
      readyAtMs: number;
    };
    __donnerActiveSampleStats?: {
      sampleId: string;
      activatedAtMs: number;
    };
    __donnerAcceptedPresentation?: {
      kind: "geode";
      token: number;
      presentedAtMs: number;
      selectionChromeBaked?: boolean;
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
    __donnerWorkerSurfaceDiagnostic?: {
      frameToken: number;
      samples: number;
      coloredPixels: number;
      nonBlackPixels: number;
      maxChannel: number;
      textStyleBackgroundPixels: number;
      textStyleGlyphPixels: number;
      acceptedAtMs: number;
      publishedAtMs: number;
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
  workerSurfaceDiagnostic?: boolean;
  workerSurfaceMode?: "direct-surface" | "bitmap-bridge";
}

const kFatalRuntimePattern =
  /Aborted|Assertion failed|RuntimeError|Pthread .* sent an error|getJsObject|No available adapters|WebGPU on Linux requires|WebGPU adapter (?:request )?(?:failed|unavailable)|Donner (?:worker surface|bitmap bridge|Wasm renderer pthread wake rejected)/i;
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

// The presented surface element spans its cap-sized canvas backing store, so
// its bounding box includes a clipped, transparent surplus band on the right
// and bottom. Document-space geometry must map through the VISIBLE box: the
// element box minus the clip-path insets.
async function visibleSurfaceBounds(
  locator: import("@playwright/test").Locator,
): Promise<{ x: number; y: number; width: number; height: number } | null> {
  return locator.evaluate((el) => {
    const rect = el.getBoundingClientRect();
    const clip = getComputedStyle(el).clipPath || "";
    const inset = clip.match(/inset\(([^)]*)\)/);
    if (!inset) {
      return { x: rect.left, y: rect.top, width: rect.width, height: rect.height };
    }
    // Engines serialize the inset() shorthand with 1-4 values; expand per the
    // CSS shorthand rules (top, right, bottom, left).
    const v = inset[1].trim().split(/\s+/).map((part) => parseFloat(part));
    if (v.length === 0 || v.some((n) => !Number.isFinite(n))) {
      return { x: rect.left, y: rect.top, width: rect.width, height: rect.height };
    }
    const top = v[0];
    const right = v.length >= 2 ? v[1] : v[0];
    const bottom = v.length >= 3 ? v[2] : v[0];
    const left = v.length >= 4 ? v[3] : right;
    return {
      x: rect.left + left,
      y: rect.top + top,
      width: rect.width - left - right,
      height: rect.height - top - bottom,
    };
  });
}

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
  const { wgpuStats, directSurface } = await page.evaluate(() => ({
    wgpuStats: window.__donnerWgpuReadbackStats?.renderPane,
    directSurface: window.__donnerWorkerStats?.readbackWaitStrategy === "direct-surface",
  }));
  if (wgpuStats && !directSurface) {
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
  // Surface diagnostics are intentionally opt-in: even asynchronous readback
  // copies and scans the framebuffer, so production interaction tests should
  // not include that debug-only work unless they are explicitly guarding it.
  if (options.wgpuReadbackStats === true) {
    url.searchParams.set("wgpuReadbackStats", "1");
  }
  if (options.workerSurfaceDiagnostic === true) {
    url.searchParams.set("workerSurfaceDiagnostic", "1");
  }
  if (options.workerSurfaceMode) {
    url.searchParams.set("workerSurface", options.workerSurfaceMode);
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
  await expect(page.locator("#status")).toBeHidden({ timeout: 20000 });
  const selectedBackend = await page.evaluate(() => window.__donnerBackend);
  expect(selectedBackend).toBe(kBackend);
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

// Restore the page scroll origin and report where it ended up.
//
// The worker surface element spans its cap-sized backing store, which is larger
// than the editor viewport, so the page has a scrollable overflow area it never
// has in normal use (the editor sets `overflow: hidden` and never scrolls). Any
// Playwright *element* screenshot - which is what `readElementColorStats` takes
// - first scrolls that element into view, leaving the page scrolled by the
// overflow amount. Every viewport-relative coordinate measured before such a
// screenshot (pointer targets, `page.screenshot` clips) is stale afterwards, so
// tests must return to the origin and re-measure rather than mix pre-scroll and
// post-scroll geometry.
async function restoreViewportScrollOrigin(page: Page): Promise<{ x: number; y: number }> {
  return page.evaluate(() => {
    window.scrollTo(0, 0);
    document.documentElement.scrollLeft = 0;
    document.documentElement.scrollTop = 0;
    return { x: window.scrollX, y: window.scrollY };
  });
}

// A landed worker result is not a settled presentation. Loading a sample can
// queue follow-up renders (the debounced canvas-size commit, an epoch retry,
// the post-drag settled-selection refresh), and a drag release always queues
// one settle render behind the frame the pointer already produced. Tests that
// assert "no further renders happened" - or that probe a diagnostic which only
// a *specific* accepted epoch can answer - must start from quiescence, not
// from "one result landed".
//
// Quiescence here means: the worker is idle and `completedResults` has not
// moved for a continuous settle window. Returns the settled result count.
async function waitForPresentationQuiescence(
  page: Page,
  options: { message: string; settleMs?: number; timeout?: number },
): Promise<number> {
  // The settle window has to outlast the longest bounded follow-up the renderer
  // can schedule without the worker looking busy: the 120 ms canvas-size commit
  // debounce and the direct-surface retry backoff, which tops out at 1 s.
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
    workerRuntime: window.__donnerWorkerRuntimeStats,
    editorRevealedAtMs: window.__donnerEditorRevealedAtMs || 0,
    loadingScreenHiddenAtMs: window.__donnerLoadingScreenHiddenAtMs || 0,
    wgpuReadbackStats: window.__donnerWgpuReadbackStats || null,
    readbackCaptureStarts: window.__donnerWgpuReadbackCaptureStarts || 0,
    readbackCaptureCompletions: window.__donnerWgpuReadbackCaptureCompletions || 0,
    readbackCaptureFailures: window.__donnerWgpuReadbackCaptureFailures || 0,
    headlessDeviceCreations: window.__donnerHeadlessDeviceCreations ?? -1,
  }));
  expect(startup.bootstrapStartedAtMs).toBeGreaterThan(0);
  expect(startup.runtimeInitializedAtMs).toBeGreaterThanOrEqual(startup.bootstrapStartedAtMs);
  expect(startup.firstFramePresentedAtMs).toBeGreaterThanOrEqual(startup.runtimeInitializedAtMs);
  if (kBackend === "geode") {
    expect(startup.workerRuntime).toMatchObject({ ready: true, initializationCount: 1 });
    expect(startup.workerRuntime?.workerDeviceCreations).toBeGreaterThanOrEqual(1);
    expect(startup.headlessDeviceCreations).toBe(startup.workerRuntime?.workerDeviceCreations);
    expect(startup.workerRuntime?.readyAtMs).toBeGreaterThanOrEqual(startup.bootstrapStartedAtMs);
    expect(startup.editorRevealedAtMs).toBeGreaterThanOrEqual(
      Math.max(startup.firstFramePresentedAtMs, startup.workerRuntime?.readyAtMs || 0),
    );
  } else {
    expect(startup.editorRevealedAtMs).toBeGreaterThanOrEqual(startup.firstFramePresentedAtMs);
  }
  expect(startup.loadingScreenHiddenAtMs).toBeGreaterThanOrEqual(startup.editorRevealedAtMs);
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
    workerDeviceCreations: window.__donnerWorkerRuntimeStats?.workerDeviceCreations || 0,
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
      workerDeviceCreations: window.__donnerWorkerRuntimeStats?.workerDeviceCreations || 0,
    },
    frames: window.__donnerMainLoopRenderedFrames || 0,
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

  // Once the last result is uploaded, the worker and event-driven main loop must park. This dwell
  // crosses the former recurring 400-500 ms update interval.
  await page.waitForTimeout(650);
  expect(await page.evaluate(() => window.__donnerMainLoopRenderedFrames || 0)).toBe(
    settled.frames,
  );
  expect(fatalMessages).toEqual([]);
});

test("WGPU diagnostics do not block the first carousel interaction", async ({ page }) => {
  test.skip(kBackend !== "geode", "WGPU surface readback diagnostics are Geode-specific");
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

    const workerRuntimeBeforeClick = await page.evaluate(
      () => window.__donnerWorkerRuntimeStats,
    );
    if (kBackend === "geode") {
      expect(workerRuntimeBeforeClick).toMatchObject({
        ready: true,
        initializationCount: 1,
      });
    }

    const beforeSample = await page.evaluate(
      () => window.__donnerWorkerStats?.completedResults || 0,
    );
    const beforeSurfaceFrame = kBackend === "geode"
      ? await page.evaluate(() =>
        Math.max(
          ...["donner-document-canvas", "donner-document-canvas-back"].map((id) =>
            Number(document.getElementById(id)?.getAttribute("data-direct-surface-frame") || 0)
          ),
        )
      )
      : 0;
    const beforeAcceptedPresentation = await page.evaluate(
      () => window.__donnerAcceptedPresentation,
    );
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
    const presentationReady = async () => {
      const state = await page.evaluate(() => ({
        acceptedPresentation: window.__donnerAcceptedPresentation,
        activeSample: window.__donnerActiveSampleStats,
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
      if (state.activeSample?.sampleId !== sample.id || completedResults <= beforeSample) {
        return false;
      }
      const acceptedPresentation = state.acceptedPresentation;
      const acceptedTokenAdvanced = acceptedPresentation
        && acceptedPresentation.token > (beforeAcceptedPresentation?.token || 0);
      const visibleSurface = page.locator("canvas[data-direct-surface-visible=\"true\"]");
      const pixelsPresented = acceptedPresentation?.kind === "geode"
        && acceptedTokenAdvanced
        && await visibleSurface.count() === 1
        && Number(await visibleSurface.getAttribute("data-direct-surface-frame"))
          > beforeSurfaceFrame;
      if (pixelsPresented && acceptedPresentation) {
        phaseTimings.presentedMs = acceptedPresentation.presentedAtMs - clickStartedAt;
      }
      return pixelsPresented;
    };
    try {
      await expect.poll(presentationReady, {
        message: `expected ${sample.name} to publish a matching accepted presentation epoch`,
        timeout: scaledMs(3000),
        intervals: [8, 16, 25, 50],
      }).toBe(true);
    } finally {
      console.log(
        `carousel-presentation sample=${sample.id} timings=${
          JSON.stringify(phaseTimings)
        } runtime=${JSON.stringify(workerRuntimeBeforeClick)} worker=${
          JSON.stringify(completedWorkerStats)
        }`,
      );
    }
    expect(phaseTimings.presentedMs).toBeLessThan(carouselDeadlineMs);
    if (kBackend === "geode") {
      expect(await page.evaluate(() => window.__donnerHeadlessDeviceCreations)).toBe(
        workerRuntimeBeforeClick?.workerDeviceCreations,
      );
    }
    expect(fatalMessages).toEqual([]);
  });
}

test("carousel never exposes the placeholder viewport for Donner Splash", async ({ page }) => {
  const fatalMessages = await openEditor(page, { postInitializationDwellMs: 0 });
  const canvas = page.locator("canvas#canvas");
  const bounds = await canvas.boundingBox();
  expect(bounds).not.toBeNull();
  if (bounds === null) {
    return;
  }

  await page.evaluate(() => {
    type VisibleSurfaceSample = {
      frame: number;
      height: number;
      rectHeight: number;
      rectWidth: number;
      slot: string;
      width: number;
    };
    const samples: VisibleSurfaceSample[] = [];
    const capture = () => {
      for (const id of ["donner-document-canvas", "donner-document-canvas-back"]) {
        const surface = document.getElementById(id) as HTMLCanvasElement | null;
        if (surface?.dataset.directSurfaceVisible !== "true") {
          continue;
        }
        const rect = surface.getBoundingClientRect();
        // The surface is configured once at a size cap, so the backing store
        // and the element box both span that cap and carry no document aspect
        // ratio at all. The presented extent is the cap minus the clip-path
        // insets that hide the surplus band, so both the backing-store sample
        // and the layout-rect sample are measured through those insets. The
        // backing store is in device pixels, so the CSS insets are scaled by
        // the element's device-pixel ratio before being subtracted from it.
        const clip = getComputedStyle(surface).clipPath || "";
        const inset = clip.match(/inset\(([^)]*)\)/);
        const parts = inset
          ? inset[1].trim().split(/\s+/).map((part) => parseFloat(part))
          : [];
        const usable = parts.length > 0 && parts.every((n) => Number.isFinite(n));
        const clipTop = usable ? parts[0] : 0;
        const clipRight = usable ? (parts.length >= 2 ? parts[1] : parts[0]) : 0;
        const clipBottom = usable ? (parts.length >= 3 ? parts[2] : parts[0]) : 0;
        const clipLeft = usable ? (parts.length >= 4 ? parts[3] : clipRight) : 0;
        const backingScaleX = rect.width > 0 ? surface.width / rect.width : 1;
        const backingScaleY = rect.height > 0 ? surface.height / rect.height : 1;
        const sample = {
          frame: Number(surface.dataset.directSurfaceFrame || 0),
          height: surface.height - (clipTop + clipBottom) * backingScaleY,
          rectHeight: rect.height - clipTop - clipBottom,
          rectWidth: rect.width - clipLeft - clipRight,
          slot: id,
          width: surface.width - (clipLeft + clipRight) * backingScaleX,
        };
        const previous = samples.at(-1);
        if (JSON.stringify(previous) !== JSON.stringify(sample)) {
          samples.push(sample);
        }
      }
      (window as Window & { __donnerVisibleSurfaceSamples?: VisibleSurfaceSample[] })
        .__donnerVisibleSurfaceSamples = samples;
    };
    const observer = new MutationObserver(capture);
    for (const id of ["donner-document-canvas", "donner-document-canvas-back"]) {
      const surface = document.getElementById(id);
      if (surface) {
        observer.observe(surface, { attributes: true });
      }
    }
    (window as Window & { __donnerVisibleSurfaceObserver?: MutationObserver })
      .__donnerVisibleSurfaceObserver = observer;
    capture();
  });

  const beforeFrame = await page.evaluate(() =>
    Math.max(
      ...["donner-document-canvas", "donner-document-canvas-back"].map((id) =>
        Number(document.getElementById(id)?.getAttribute("data-direct-surface-frame") || 0)
      ),
    )
  );
  await page.mouse.click(bounds.x + bounds.width * 0.24, bounds.y + 282);
  await expect(canvas).toHaveAttribute("data-active-sample-id", "donner-splash");
  await expect
    .poll(async () =>
      Number(
        await page.locator("canvas[data-direct-surface-visible=\"true\"]")
          .getAttribute("data-direct-surface-frame") || 0,
      ), {
      message: "expected Donner Splash to present a document surface",
      timeout: scaledMs(5000),
      intervals: [8, 16, 25, 50],
    })
    .toBeGreaterThan(beforeFrame);

  const visibleSamples = await page.evaluate(() => {
    const state = window as Window & {
      __donnerVisibleSurfaceObserver?: MutationObserver;
      __donnerVisibleSurfaceSamples?: Array<{
        frame: number;
        height: number;
        rectHeight: number;
        rectWidth: number;
        slot: string;
        width: number;
      }>;
    };
    state.__donnerVisibleSurfaceObserver?.disconnect();
    return state.__donnerVisibleSurfaceSamples || [];
  });

  expect(visibleSamples.length).toBeGreaterThan(0);
  // Both samples are presented (clip-inset) extents, one derived from the
  // backing store and one from the layout rect: the very first surface the
  // carousel makes visible must already carry the Splash artboard aspect, never
  // a placeholder viewport.
  const firstVisible = visibleSamples[0];
  expect(firstVisible.width / firstVisible.height).toBeCloseTo(892 / 512, 2);
  expect(firstVisible.rectWidth / firstVisible.rectHeight).toBeCloseTo(892 / 512, 2);
  expect(fatalMessages).toEqual([]);
});

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
  const fatalMessages = await openEditor(page, {
    postInitializationDwellMs: 0,
    workerSurfaceDiagnostic: kBackend === "geode",
  });
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
  const acceptedBefore = await page.evaluate(() => window.__donnerAcceptedPresentation?.token || 0);
  await page.mouse.click(bounds.x + bounds.width * 0.76, bounds.y + 282);
  await expect(canvas).toHaveAttribute("data-active-sample-id", "text-style", { timeout: 1000 });
  await expect
    .poll(async () => page.evaluate(() => window.__donnerWorkerStats?.completedResults || 0), {
      message: "expected the Text and Style sample to finish presenting",
      timeout: scaledMs(2000),
      intervals: [16, 25, 50, 100],
    })
    .toBeGreaterThan(beforeSample);
  if (kBackend === "geode") {
    await expect
      .poll(async () =>
        page.evaluate((beforeToken) => {
          const diagnostic = window.__donnerWorkerSurfaceDiagnostic;
          return Boolean(
            diagnostic
              && diagnostic.frameToken > beforeToken
              && diagnostic.acceptedAtMs > 0
              && diagnostic.textStyleBackgroundPixels > 10_000
              && diagnostic.textStyleGlyphPixels > 200,
          );
        }, acceptedBefore), {
        message: "expected Text and Style diagnostics from the accepted direct-surface epoch",
        timeout: scaledMs(5000),
        intervals: [16, 25, 50, 100],
      })
      .toBe(true);
    const textStats = await page.evaluate(() => window.__donnerWorkerSurfaceDiagnostic);
    expect(textStats?.textStyleBackgroundPixels).toBeGreaterThan(10_000);
    expect(textStats?.textStyleGlyphPixels).toBeGreaterThan(200);
  } else {
    const textStats = await readTextStyleGlyphStats(page, {
      x: kSourcePaneWidth + 20,
      y: 80,
      width: 1600 - kSourcePaneWidth - kRightPaneWidth - 40,
      height: 680,
    });
    expect(textStats.backgroundPixels).toBeGreaterThan(10_000);
    expect(textStats.glyphPixels).toBeGreaterThan(200);
  }
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

  const documentCanvas = page.locator("canvas[data-direct-surface-visible=\"true\"]");
  const documentBounds = kBackend === "geode" ? await visibleSurfaceBounds(documentCanvas) : null;
  if (kBackend === "geode") {
    expect(documentBounds).not.toBeNull();
    expect(await page.evaluate(() => window.__donnerWorkerSurfaceDiagnostic)).toBeUndefined();
  }
  const blueRectCenter = documentBounds
    ? {
      x: documentBounds.x + documentBounds.width * (122 / 640),
      y: documentBounds.y + documentBounds.height * (92 / 400),
    }
    : { x: bounds.x + 408, y: bounds.y + 353 };
  if (kBackend === "geode") {
    // `selectionChromePixels` reads the ImGui canvas, so it can only see chrome
    // the overlay drew. Once any render lands while a selection is live, the
    // worker bakes the chrome into its own surface, `selectionChromeBaked` goes
    // true, and EditorShell stops drawing it into the overlay - the probe then
    // reads zero forever. Start the click from a quiesced presentation so no
    // leftover load render can move the chrome out from under the probe.
    const beforeSelectionResult = await waitForPresentationQuiescence(page, {
      message: "expected the Basic Shapes presentation to settle before selecting a shape",
    });
    const beforeSelectionFrame = Number(
      await documentCanvas.getAttribute("data-direct-surface-frame"),
    );
    expect(
      await page.evaluate(() => window.__donnerAcceptedPresentation?.selectionChromeBaked ?? false),
      "expected an unbaked accepted epoch so overlay-only chrome is observable",
    ).toBe(false);
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
    let selectionFeedbackDiagnostic = "";
    await expect
      .poll(
        async () => {
          const snapshot = await page.evaluate(() => ({
            chrome: window.__donnerWgpuReadbackStats?.selectionChromePixels || 0,
            completed: window.__donnerWorkerStats?.completedResults || 0,
            baked: window.__donnerAcceptedPresentation?.selectionChromeBaked ?? false,
          }));
          selectionChromeDelta = snapshot.chrome - beforeSelectionChrome;
          if (selectionChromeDelta > 50) {
            return "overlay-chrome";
          }
          if (snapshot.baked || snapshot.completed !== beforeSelectionResult) {
            // A render landed while the selection was live, so the worker baked
            // the chrome into its own surface and this probe (which reads the
            // ImGui canvas) can never see it. Stop polling and report the real
            // failure instead of starving until the timeout.
            selectionFeedbackDiagnostic = JSON.stringify(snapshot);
            return "rerendered-after-selection";
          }
          // The completed capture may predate the overlay's first rendered
          // frame on slow hosts; request a fresh diagnostic for the next read
          // instead of polling a stale snapshot forever.
          await requestWgpuDiagnostic(page);
          return "waiting";
        },
        {
          message: "expected overlay-only selection feedback before starting the drag",
          timeout: scaledMs(2000),
          intervals: [100, 250, 500],
        },
      )
      .not.toBe("waiting");
    expect(
      selectionFeedbackDiagnostic,
      "a selection click must not trigger a render; the baked epoch hides overlay chrome",
    ).toBe("");
    expect(selectionChromeDelta).toBeGreaterThan(50);
    expect(await page.evaluate(() => window.__donnerWorkerStats?.completedResults || 0)).toBe(
      beforeSelectionResult,
    );
    expect(Number(await documentCanvas.getAttribute("data-direct-surface-frame"))).toBe(
      beforeSelectionFrame,
    );
  }
  const targetClip = {
    x: bounds.x + 280,
    y: bounds.y + 270,
    width: 400,
    height: 240,
  };
  const beforeMouseDown = kBackend === "geode"
    ? null
    : await page.screenshot({ clip: targetClip });
  await page.mouse.move(blueRectCenter.x, blueRectCenter.y);
  const mouseDownStartedAt = Date.now();
  await page.mouse.down();
  if (beforeMouseDown !== null) {
    await expect
      .poll(async () => !(await page.screenshot({ clip: targetClip })).equals(beforeMouseDown), {
        message: "expected selection feedback after mouse-down",
        timeout: scaledMs(1000),
        intervals: [16, 25, 50, 100],
      })
      .toBe(true);
  }
  console.log(`selection-feedback-ms=${Date.now() - mouseDownStartedAt}`);

  const draggedShapeInterior = {
    x: blueRectCenter.x - 12,
    y: blueRectCenter.y - 12,
    width: 24,
    height: 24,
  };
  const beforeMove = kBackend === "geode"
    ? null
    : await page.screenshot({ clip: draggedShapeInterior });
  const beforeMoveDirectSurfaceFrame = kBackend === "geode"
    ? await documentCanvas.getAttribute("data-direct-surface-frame").then(Number)
    : 0;
  // The 125 ms interaction budget is calibrated for local development
  // hardware. Shared CI runners are slower and the poll below adds up to one
  // interval of measurement quantization, so CI enforces a looser bound that
  // still fails on genuine stalls.
  const firstDragFrameBudgetMs = scaledMs(125);
  const dragMoveStartedAt = Date.now();
  await page.mouse.move(blueRectCenter.x + 120, blueRectCenter.y + 70);
  await expect
    .poll(async () => {
      if (kBackend === "geode") {
        const directSurfaceFrame = Number(
          await documentCanvas.getAttribute("data-direct-surface-frame"),
        );
        return directSurfaceFrame > beforeMoveDirectSurfaceFrame;
      }
      return beforeMove !== null
        && !(await page.screenshot({ clip: draggedShapeInterior })).equals(beforeMove);
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

  const documentCanvas = page.locator("canvas[data-direct-surface-visible=\"true\"]");
  await expect(documentCanvas).toBeVisible({ timeout: 1000 });
  await expect
    .poll(async () => (await readElementColorStats(documentCanvas)).coloredPixels, {
      message: "expected visible Basic Shapes pixels before selecting the resize target",
      timeout: scaledMs(1000),
      intervals: [16, 25, 50, 100],
    })
    .toBeGreaterThan(500);

  // `readElementColorStats` takes an element screenshot, which Playwright
  // scrolls into view first; the cap-sized surface overflows the viewport, so
  // that leaves the page scrolled. Return to the origin and re-measure before
  // deriving any pointer target or screenshot clip from the editor canvas.
  expect(
    await restoreViewportScrollOrigin(page),
    "element screenshots scrolled the editor page and it did not return to the origin",
  ).toEqual({ x: 0, y: 0 });
  const editorBounds = await canvas.boundingBox();
  expect(editorBounds).not.toBeNull();
  if (editorBounds === null) {
    return;
  }

  const blueRectCenter = { x: editorBounds.x + 408, y: editorBounds.y + 353 };
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
  const initialResults = await page.evaluate(
    () => window.__donnerWorkerStats?.completedResults || 0,
  );
  await page.mouse.move(resizeHandle.x, resizeHandle.y);
  await page.mouse.down();

  // Emit a short 60 Hz-style burst. Before the no-starvation policy, each
  // move canceled the preceding render and only the final request could land.
  for (let step = 1; step <= 4; ++step) {
    await page.mouse.move(resizeHandle.x + step * 28, resizeHandle.y + step * 18);
    await page.waitForTimeout(16);
  }
  await expect
    .poll(async () => page.evaluate(() => window.__donnerWorkerStats?.completedResults || 0), {
      message: "expected multiple resize frames to land during one continuous pointer burst",
      timeout: scaledMs(500),
      intervals: [16, 25, 50],
    })
    .toBeGreaterThanOrEqual(initialResults + 2);

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
        "expected the final resized pixels and selection outline from one presentation epoch",
      timeout: scaledMs(1500),
      intervals: [50, 100],
    })
    .toBe(true);
  await page.mouse.up();
  expect(fatalMessages).toEqual([]);
});

test("Geode WASM selects through the overlay without document rerender or recurring updates", async ({ page }) => {
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

  const documentCanvas = page.locator("canvas[data-direct-surface-visible=\"true\"]");
  await expect(documentCanvas).toBeVisible();
  await expect
    .poll(async () => Number(await documentCanvas.getAttribute("data-direct-surface-frame")), {
      message: "expected the worker-owned document surface to present Basic Shapes",
      timeout: scaledMs(1000),
      intervals: [16, 25, 50, 100],
    })
    .toBeGreaterThan(0);
  const documentBounds = await visibleSurfaceBounds(documentCanvas);
  expect(documentBounds).not.toBeNull();
  if (documentBounds === null) {
    return;
  }
  const dragStart = {
    x: documentBounds.x + documentBounds.width * (122 / 640),
    y: documentBounds.y + documentBounds.height * (92 / 400),
  };
  // "One result landed" is not "the load settled" - the debounced canvas-size
  // commit and epoch retries can still be queued. Baseline the no-rerender
  // assertion below against a quiesced presentation instead.
  const beforeSelection = await waitForPresentationQuiescence(page, {
    message: "expected the Basic Shapes presentation to settle before selecting a shape",
  });
  const beforeSelectionFrame = Number(
    await documentCanvas.getAttribute("data-direct-surface-frame"),
  );
  const beforeSelectionUiFrame = await page.evaluate(
    () => window.__donnerMainLoopRenderedFrames || 0,
  );
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
  expect(await page.evaluate(() => window.__donnerWorkerStats?.completedResults || 0)).toBe(
    beforeSelection,
  );
  expect(Number(await documentCanvas.getAttribute("data-direct-surface-frame"))).toBe(
    beforeSelectionFrame,
  );
  await page.mouse.move(dragStart.x, dragStart.y);
  await page.mouse.down();
  const beforeDrag = await page.evaluate(
    () => window.__donnerWorkerStats?.completedResults || 0,
  );
  const beforeDragFrame = Number(await documentCanvas.getAttribute("data-direct-surface-frame"));
  await page.mouse.move(dragStart.x + 18, dragStart.y + 12);
  await page.mouse.move(dragStart.x + 32, dragStart.y + 20);
  await expect
    .poll(async () => page.evaluate(() => window.__donnerWorkerStats?.completedResults || 0), {
      message: "expected a direct document frame while the shape drag is still active",
      timeout: scaledMs(1000),
      intervals: [16, 25, 50, 100],
    })
    .toBeGreaterThan(beforeDrag);
  await expect
    .poll(async () => Number(await documentCanvas.getAttribute("data-direct-surface-frame")), {
      message: "expected the visible document surface to advance before mouse-up",
      timeout: scaledMs(1000),
      intervals: [16, 25, 50, 100],
    })
    .toBeGreaterThan(beforeDragFrame);
  const beforeMouseUpFrame = Number(
    await documentCanvas.getAttribute("data-direct-surface-frame"),
  );
  await page.mouse.up();
  await expect
    .poll(async () => Number(await documentCanvas.getAttribute("data-direct-surface-frame")), {
      message: "expected the drag-release settlement frame",
      timeout: scaledMs(1000),
      intervals: [16, 25, 50, 100],
    })
    .toBeGreaterThan(beforeMouseUpFrame);

  // Releasing a drag queues exactly one settled-selection refresh behind the
  // frame the pointer already produced, so the first post-mouse-up surface
  // frame is not the last result. Let that bounded settle finish (it not
  // finishing is itself a failure), then assert nothing keeps re-rendering.
  const settledCount = await waitForPresentationQuiescence(page, {
    message: "expected the drag-release settle to finish",
  });
  await page.waitForTimeout(1500);
  expect(await page.evaluate(() => window.__donnerWorkerStats?.completedResults || 0)).toBe(
    settledCount,
  );
  const workerStats = await page.evaluate(() => window.__donnerWorkerStats);
  expect(workerStats).toBeDefined();
  expect(workerStats?.directSurfaceFrames).toBeGreaterThan(0);
  expect(workerStats?.readbackCount).toBe(0);
  expect(workerStats?.readbackWaitStrategy).toBe("direct-surface");
  expect(workerStats?.readbackPollIterations).toBe(0);
  console.log(`wasm-worker-stats=${JSON.stringify(workerStats)}`);
  expect(fatalMessages).toEqual([]);
});

test("production Geode wasm presents visible editor pixels", async ({ page }) => {
  test.skip(kBackend !== "geode", "production WebGPU presentation is Geode-specific");
  // Chromium screenshots omit a transferred WebGPU swapchain. Prove direct-surface pixels with a
  // worker diagnostic tied to the exact epoch the browser accepted instead.
  const fatalMessages = await openEditor(page, {
    postInitializationDwellMs: 0,
    workerSurfaceDiagnostic: true,
  });
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

  const acceptedBefore = await page.evaluate(() => window.__donnerAcceptedPresentation?.token || 0);
  await page.mouse.click(canvasBox.x + canvasBox.width * 0.5, canvasBox.y + 282);
  const documentCanvas = page.locator("canvas[data-direct-surface-visible=\"true\"]");
  await expect(documentCanvas).toBeVisible({ timeout: 20000 });
  await expect
    .poll(async () =>
      page.evaluate((beforeToken) => {
        const visible = document.querySelector(
          "canvas[data-direct-surface-visible=\"true\"]",
        );
        const surfaceFrame = Number(visible?.getAttribute("data-direct-surface-frame") || 0);
        const accepted = window.__donnerAcceptedPresentation;
        const diagnostic = window.__donnerWorkerSurfaceDiagnostic;
        return Boolean(
          surfaceFrame > beforeToken
            && accepted?.kind === "geode"
            && accepted.token === surfaceFrame
            && diagnostic?.frameToken === surfaceFrame
            && diagnostic.nonBlackPixels > 1000
            && diagnostic.coloredPixels > 500
            && diagnostic.maxChannel > 80,
        );
      }, acceptedBefore), {
      message: "expected an accepted direct-surface epoch with diagnostic GPU product pixels",
      timeout: 20000,
      intervals: [16, 25, 50, 100],
    })
    .toBe(true);
  const surfaceDiagnostic = await page.evaluate(() => window.__donnerWorkerSurfaceDiagnostic);
  console.log(`direct-surface-diagnostic=${JSON.stringify(surfaceDiagnostic)}`);
  expect(fatalMessages).toEqual([]);
});

test("forced bitmap worker-surface fallback presents retained Geode pixels", async ({ browserName, page }) => {
  test.skip(browserName !== "chromium" || kBackend !== "geode", "Geode bridge probe");
  // Complement the direct-surface diagnostic above with a real screenshot oracle on the retained
  // ImageBitmap bridge, whose pixels Chromium capture can sample.
  const fatalMessages = await openEditor(page, {
    postInitializationDwellMs: 0,
    workerSurfaceMode: "bitmap-bridge",
  });
  await expect
    .poll(() => page.evaluate(() => window.__donnerWorkerSurfaceMode))
    .toBe("bitmap-bridge");
  const canvasBox = await page.locator("canvas#canvas").boundingBox();
  expect(canvasBox).not.toBeNull();
  if (canvasBox === null) {
    return;
  }
  await page.mouse.click(canvasBox.x + canvasBox.width * 0.5, canvasBox.y + 282);
  const documentCanvas = page.locator("canvas[data-direct-surface-visible=\"true\"]");
  await expect(documentCanvas).toBeVisible({ timeout: 20000 });
  await expect
    .poll(async () => Number(await documentCanvas.getAttribute("data-bitmap-bridge-frame")), {
      message: "expected the retained bitmap bridge to present a worker WebGPU frame",
      timeout: 20000,
      intervals: [16, 25, 50, 100],
    })
    .toBeGreaterThan(0);
  await expect
    .poll(async () => (await readElementColorStats(documentCanvas)).coloredPixels, {
      message: "expected visible retained document pixels from the Safari bridge",
      timeout: 20000,
      intervals: [16, 25, 50, 100],
    })
    .toBeGreaterThan(500);

  await page.evaluate(() => {
    const violations: string[] = [];
    const canvases = [
      document.getElementById("donner-document-canvas"),
      document.getElementById("donner-document-canvas-back"),
    ];
    const checkVisibleEpoch = () => {
      for (const surfaceCanvas of canvases) {
        if (surfaceCanvas?.getAttribute("data-direct-surface-visible") !== "true") {
          continue;
        }
        const accepted = surfaceCanvas.getAttribute("data-direct-surface-frame");
        const staged = surfaceCanvas.getAttribute("data-bitmap-bridge-frame");
        if (!accepted || accepted !== staged) {
          violations.push(`${surfaceCanvas.id}:accepted=${accepted}:staged=${staged}`);
        }
      }
    };
    const observer = new MutationObserver(checkVisibleEpoch);
    for (const surfaceCanvas of canvases) {
      if (surfaceCanvas) {
        observer.observe(surfaceCanvas, { attributes: true });
      }
    }
    (window as Window & { __donnerBitmapEpochViolations?: string[] })
      .__donnerBitmapEpochViolations = violations;
    checkVisibleEpoch();
  });

  const documentBounds = await visibleSurfaceBounds(documentCanvas);
  expect(documentBounds).not.toBeNull();
  if (documentBounds === null) {
    return;
  }
  const dragStart = {
    x: documentBounds.x + documentBounds.width * (122 / 640),
    y: documentBounds.y + documentBounds.height * (92 / 400),
  };
  const beforeDragResults = await page.evaluate(
    () => window.__donnerWorkerStats?.completedResults || 0,
  );
  await page.mouse.move(dragStart.x, dragStart.y);
  await page.mouse.down();
  for (let step = 1; step <= 12; ++step) {
    await page.mouse.move(dragStart.x + step * 10, dragStart.y + step * 6);
  }
  await expect
    .poll(async () => page.evaluate(() => window.__donnerWorkerStats?.completedResults || 0), {
      message: "expected a forced bitmap drag result after the superseding pointer burst",
      timeout: scaledMs(2000),
      intervals: [16, 25, 50, 100],
    })
    .toBeGreaterThan(beforeDragResults);
  await page.mouse.up();

  const bitmapEpochs = await page.evaluate(() => ({
    violations: (window as Window & { __donnerBitmapEpochViolations?: string[] })
      .__donnerBitmapEpochViolations || [],
    visible: ["donner-document-canvas", "donner-document-canvas-back"]
      .map((id) => document.getElementById(id))
      .filter((surfaceCanvas) =>
        surfaceCanvas?.getAttribute("data-direct-surface-visible") === "true"
      )
      .map((surfaceCanvas) => ({
        accepted: surfaceCanvas?.getAttribute("data-direct-surface-frame"),
        id: surfaceCanvas?.id,
        staged: surfaceCanvas?.getAttribute("data-bitmap-bridge-frame"),
      })),
  }));
  expect(bitmapEpochs.violations).toEqual([]);
  expect(bitmapEpochs.visible).toHaveLength(1);
  expect(bitmapEpochs.visible[0]?.accepted).toBe(bitmapEpochs.visible[0]?.staged);
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
  const beforeSampleSurfaceFrame = await page.evaluate(() =>
    Math.max(
      ...["donner-document-canvas", "donner-document-canvas-back"].map((id) =>
        Number(document.getElementById(id)?.getAttribute("data-direct-surface-frame") || 0)
      ),
    )
  );
  await page.mouse.click(bounds.x + bounds.width * 0.5, bounds.y + 282);
  await expect(canvas).toHaveAttribute("data-active-sample-id", "basic-shapes");
  await expect
    .poll(async () => page.evaluate(() => window.__donnerWorkerStats?.completedResults || 0), {
      message: "expected the Basic Shapes worker result before reading diagnostics",
      timeout: 20000,
    })
    .toBeGreaterThan(beforeSampleResult);
  const documentCanvas = page.locator("canvas[data-direct-surface-visible=\"true\"]");
  await expect(documentCanvas).toBeVisible({ timeout: 20000 });
  await expect
    .poll(async () => Number(await documentCanvas.getAttribute("data-direct-surface-frame")), {
      message: "expected a fresh Basic Shapes direct-surface frame before reading diagnostics",
      timeout: 20000,
      intervals: [16, 25, 50, 100],
    })
    .toBeGreaterThan(beforeSampleSurfaceFrame);
  const documentBounds = await visibleSurfaceBounds(documentCanvas);
  expect(documentBounds).not.toBeNull();
  if (documentBounds === null) {
    return;
  }
  const coloredPixel = {
    x: documentBounds.width * (122 / 640),
    y: documentBounds.height * (92 / 400),
  };
  const sampleRequest = await requestWgpuDiagnostic(page);
  await page.mouse.move(
    documentBounds.x + coloredPixel.x - 4,
    documentBounds.y + coloredPixel.y - 4,
  );
  await expect
    .poll(async () => page.evaluate(() => window.__donnerWgpuReadbackStats?.request || 0), {
      message: "expected a diagnostic capture for the settled Basic Shapes surface",
      timeout: 20000,
    })
    .toBeGreaterThanOrEqual(sampleRequest);
  const beforeSelectionChrome = await page.evaluate(
    () => window.__donnerWgpuReadbackStats?.selectionChromePixels || 0,
  );
  await page.mouse.click(documentBounds.x + coloredPixel.x, documentBounds.y + coloredPixel.y);
  await expect
    .poll(async () => page.evaluate(() => window.__donnerInteractionStats?.selectedCount || 0), {
      message: "expected shape selection before capturing its overlay",
      timeout: scaledMs(2000),
      intervals: [16, 25, 50, 100],
    })
    .toBeGreaterThan(0);
  const selectionRequest = await requestWgpuDiagnostic(page);
  await page.mouse.move(
    documentBounds.x + coloredPixel.x + 4,
    documentBounds.y + coloredPixel.y + 4,
  );
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
