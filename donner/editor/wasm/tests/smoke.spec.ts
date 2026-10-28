import { installOwnerEventTimerHold, readOwnerEventTimerHold, releaseOwnerEventTimerHold } from "./event-presentation-delay.mjs";
import { kCarouselSamples } from "./carousel-samples.mjs";
import { completedResultHandoff } from "./completed-result-handoff.mjs";
import { packageHashes } from "./package-identity.mjs";
import {
  armPipelineFirstUseCompiles,
  pipelineCompileFaultPrologue,
  readPipelineCompileFault,
  sumPipelineCompilesPending,
} from "./pipeline-compile-readiness.mjs";
import { errors, expect, type Page, test, type TestInfo, type Worker } from "@playwright/test";
import { readFile, writeFile } from "node:fs/promises";
import { arch, cpus, loadavg, platform, release, totalmem } from "node:os";
import { join } from "node:path";
import {
  gatedPulseFrame,
  presentedPulseCompletion,
  queuedDispatchCaughtUp,
} from "./animation-pulse-observation.mjs";
import {
  awaitBeforeBlueDeadline,
  captureAfterCanvasGpuCompletion,
  captureReadyBasicShapesFrame,
  hasPresentedBasicShapesHostFrame,
} from "./basic-shapes-capture-gate";
import { boundBrowserDiagnostic, readGpuOwnerDispatchStats } from "./browser-runtime-diagnostics";
import { installBrowserStallDiagnostics } from "./browser-stall-diagnostics";
import {
  type CanvasColorStats,
  captureEditorPage,
  type CssRegion,
  type EditorPixelTarget,
  findElementColoredPixel,
  isOnlyEditorPageBackground,
  type PixelBounds,
  readCanvasColorStats,
  readEditorPixelBoundsFromPng,
  readElementColorStats,
  readTextStyleGlyphStats,
  type ScreenshotTimeoutStage,
} from "./canvas-color-stats";
import { armFailureCanvasEvidence, installFailureCanvasEvidence } from "./failure-canvas-evidence";
import { retakeFlatCapture } from "./flat-capture-retake.mjs";
import { waitForAppliedPointer } from "./gesture-streams";
import { echoGpuSessionConsole } from "./gpu-session-console";
import { quarantineStillSkips } from "./quarantine-report.mjs";
import { sampleThumbnailLaneQuiescent } from "./sample-thumbnail-quiescence.mjs";
import { expectSampleThumbnailsToSettle } from "./sample-thumbnail-settle";
import {
  armCanvasGpuDelayFromEnv,
  canvasGpuDelayFromEnv,
  type CanvasSettleResult,
  expectSurfaceFrameProbeReady,
  findCanvasOwnerWorker,
  findProbedCanvasApplicationWorker,
  holdCanvasCompletionForTest,
  holdCanvasCompletions,
  readCanvasActivity,
  settleCanvasWrites,
} from "./surface-frame-probe";
import { kStallGuardMs, scaledMs } from "./test-timing";

installBrowserStallDiagnostics(test);
installFailureCanvasEvidence(test);

// CI's GitHub-hosted macOS job sets this to skip the cases quarantined for that job only
// (#1691, #1702); every other lane and local runs still run them.
const kHostedMacosQuarantine = process.env.DONNER_HOSTED_MACOS_QUARANTINE === "1";

declare global {
  interface Window {
    __catalogFetchErrors?: { message: string; globalReceiver: boolean }[];
    __catalogFontTest?: {
      requests: { url: string; afterFirstFrame: boolean }[];
      requestsAtFirstFrame: number | null;
    };
    __donnerBackend?: string;
    __donnerWholeAppWorker?: boolean;
    __donnerCanStartWasm?: boolean;
    __donnerFirstFramePresented?: boolean;
    __donnerRasterShutdownRequests?: number;
    __donnerLayerThumbnailStats?: {
      refreshPasses: number;
      renderAttempts: number;
      previewPaused: boolean;
    };
    __donnerLastScrollEvent?: {
      zoomModifierHeld?: boolean;
      yoffset?: number;
      count?: number;
    };
    __donnerPinchWheelDeltaPerLnScale?: number;
    // Shape of a completed frame's publish. A publish that reports a GPU wait
    // failure carries only `completedResults` and the health fields below, so
    // the timing fields are optional in principle; they are declared required
    // because every assertion in this file reads them from a frame that
    // landed, and loosening them would push a null check into each one.
    __donnerWorkerStats?: {
      completedResults: number;
      acceptedForPresentation: boolean;
      documentGeneration: number;
      frameVersion: number;
      fontResourceRevision: number;
      sourceVersion: number;
      undoEntryCount: number;
      publishedAtMs: number;
      presentedAtMs?: number;
      resultFramePresented?: boolean;
      resultFrameCompletedAtMs?: number;
      framesPresentedWithoutResult?: number;
      postPollFramesCompletedWithoutResult?: number;
      preexistingFramesCompleted?: number;
      presentationBoundaryKnown?: boolean;
      workerMs: number;
      queueWaitMs: number;
      idleYieldMs: number;
      idleCallbackMs: number;
      idleLockMs: number;
      idleWaitMs: number;
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
      compositorReadbackCount: number;
      tileHandoffReadbackCount: number;
      readbackPollIterations: number;
      readbackWaitStrategy: string;
      // Present on every publish, including one that reports a GPU wait
      // failure with no frame behind it.
      deviceLost: boolean;
      gpuWaitTimeoutSite: string;
      gpuWaitTimeoutMs: number;
      publishReason: string;
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
    __donnerWgpuReadbackCompleted?: number;
    __donnerWgpuReadbackLastStartedRequest?: number;
    __donnerWgpuReadbackLastFailedRequest?: number;
    __donnerWgpuReadbackCaptureStarts?: number;
    __donnerWgpuReadbackCaptureCompletions?: number;
    __donnerWgpuReadbackCaptureFailures?: number;
    __donnerMainLoopRenderedFrames?: number;
    __donnerPresentedDocumentStats?: {
      count: number;
      documentGeneration: number;
      frameVersion: number;
      lastPresentedAtMs: number;
      lastPublicationDelayMs: number;
      samples: [number, number, number, number][];
    };
    __donnerHostFrameTiming?: {
      frames: number;
      lastSurfacePresented: boolean;
    };
    __donnerFrameLoopStats?: {
      lastFrameAtMs?: number;
    };
    __donnerInteractionStats?: {
      selectedCount: number;
      pendingClick: boolean;
      workerBusy: boolean;
      dragging: boolean;
      moved: boolean;
      pointerX: number;
      pointerY: number;
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
    __donnerRenderWorkerStartup?: {
      beforeUiSetup: boolean;
      beforeExplicitStart: boolean;
      afterExplicitStart: boolean;
    };
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
      foregroundHandoffWaits: number;
      firstAttemptCompleted: boolean;
      offscreenRendererConstructionStarts: number;
      offscreenRendererConstructionBlocked: boolean;
      drained: boolean;
    };
    __donnerSampleThumbnailRendererCreationBlocked?: {
      blocked: boolean;
      constructionStarts: number;
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

/** One published thumbnail-lane state, as the welcome picker test records it. */
interface ThumbnailLanePublication {
  ready: number;
  drained: boolean;
  active: boolean;
  pending: boolean;
}

interface WgpuCarouselThumbnailStats extends WgpuReadbackColorStats {
  fingerprint: number;
  backgroundPixels: number;
  glyphPixels: number;
}

interface OpenEditorOptions {
  frameDriver?: "proxied-main-raf";
  wgpuReadbackStats?: boolean;
  postInitializationDwellMs?: number;
  sampleThumbnailRendererCreationRequest?: number;
  sampleThumbnailRendererCreationDelayMs?: number;
}

const kFatalRuntimePattern =
  /Aborted|Assertion failed|RuntimeError|Pthread .* sent an error|getJsObject|No available adapters|WebGPU on Linux requires|WebGPU adapter (?:request )?(?:failed|unavailable)|Wasm renderer pthread wake rejected/i;
const kGpuAcquisitionPrefix = "[Geode/browser/acquire]";
const kGpuAcquisitionPattern =
  /^\[Geode\/browser\/acquire\] stage=(selection|runtime|root) outcome=(api_unavailable|adapter_null|adapter_rejected|device_rejected|device_install_failed|deadline_pending|request_failed|context_failed|construction_failed)$/;
const gpuAcquisitionEvents = new WeakMap<Page, string[]>();

function fixedGpuAcquisitionEvent(text: string): string | null {
  const match = kGpuAcquisitionPattern.exec(text);
  return match ? `stage=${match[1]} outcome=${match[2]}` : null;
}
const kSourcePaneWidth = 560;
const kRightPaneWidth = 420;
const kWelcomeContentMaxWidth = 920;

const kBackend = "geode";
const kExpectedHeadlessBackend = process.env.DONNER_WASM_EXPECTED_HEADLESS_BACKEND ?? "browser";
if (kExpectedHeadlessBackend !== "browser" && kExpectedHeadlessBackend !== "transitional") {
  throw new Error(`unsupported expected headless backend: ${kExpectedHeadlessBackend}`);
}
const kExpectedRasterWaitStrategy = kExpectedHeadlessBackend === "transitional"
  ? "timed-wait-any"
  : "device-poll";

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
  const browser = await page.evaluate(() => {
    const gpu = navigator.gpu;

    return {
      isSecureContext,
      crossOriginIsolated,
      hasSharedArrayBuffer: typeof SharedArrayBuffer !== "undefined",
      hasNavigatorGpu: Boolean(gpu),
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
  armFailureCanvasEvidence(page, test.info());
  const baseUrl = process.env.DONNER_WASM_BASE_URL || "http://127.0.0.1:8000";
  const url = new URL(baseUrl);
  if (options.frameDriver) {
    url.searchParams.set("frameDriver", options.frameDriver);
  }
  // Readback diagnostics are intentionally opt-in: even asynchronous readback
  // copies and scans the framebuffer, so production interaction tests should
  // not include that debug-only work unless they are explicitly guarding it.
  if (options.wgpuReadbackStats === true) {
    url.searchParams.set("wgpuReadbackStats", "1");
  }
  if ((options.sampleThumbnailRendererCreationRequest ?? 0) > 0) {
    url.searchParams.set(
      "sampleThumbnailRendererCreationRequest",
      String(options.sampleThumbnailRendererCreationRequest),
    );
    url.searchParams.set(
      "sampleThumbnailRendererCreationDelayMs",
      String(options.sampleThumbnailRendererCreationDelayMs ?? 0),
    );
  }
  const fatalMessages: string[] = [];
  const recordFatalMessage = (message: string) => {
    const boundedMessage = message.replace(/\s+/g, " ").slice(0, 2000);
    fatalMessages.push(boundedMessage);
    console.error(`browser fatal: ${boundedMessage}`);
  };

  echoGpuSessionConsole(page);
  page.on("console", (message) => {
    const text = message.text();
    if (text.startsWith(kGpuAcquisitionPrefix)) {
      const event = `[gpu-acquisition] ${fixedGpuAcquisitionEvent(text) ?? "invalid_marker"}`;
      const events = gpuAcquisitionEvents.get(page) ?? [];
      if (events.length < 8) events.push(event);
      gpuAcquisitionEvents.set(page, events);
      recordFatalMessage(event);
      return;
    }
    if (kFatalRuntimePattern.test(text)) {
      recordFatalMessage(`[console:${message.type()}] ${text}`);
    }
  });
  page.on("pageerror", (error) => {
    recordFatalMessage(`[pageerror] ${error.stack || error.message}`);
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
  if (!hasWebGpu && kRequireWebGpu) {
    throw new Error("Geode smoke suite requires WebGPU, but navigator.gpu is unavailable");
  }
  // Optional local probes may skip unsupported browsers; required CI lanes fail above.
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

async function readWgpuDiagnosticOutcome(page: Page) {
  return page.evaluate(() => ({
    requested: window.__donnerWgpuReadbackRequested ?? 0,
    completed: window.__donnerWgpuReadbackCompleted ?? 0,
    successfulRequest: window.__donnerWgpuReadbackStats?.request ?? 0,
    lastStartedRequest: window.__donnerWgpuReadbackLastStartedRequest ?? 0,
    lastFailedRequest: window.__donnerWgpuReadbackLastFailedRequest ?? 0,
    starts: window.__donnerWgpuReadbackCaptureStarts ?? 0,
    captures: window.__donnerWgpuReadbackCaptureCompletions ?? 0,
    failures: window.__donnerWgpuReadbackCaptureFailures ?? 0,
    hostFrame: window.__donnerHostFrameTiming?.frames ?? 0,
    hostPresented: window.__donnerHostFrameTiming?.lastSurfacePresented ?? false,
    queue: window.__donnerPresentationQueueStats ?? null,
  }));
}

function hasSuccessfulDiagnosticRequest(
  state: Awaited<ReturnType<typeof readWgpuDiagnosticOutcome>>,
  request: number,
): boolean {
  return request > 0 && state.completed >= request && state.successfulRequest >= request
    && state.lastFailedRequest < request;
}

async function waitForSuccessfulWgpuDiagnostic(page: Page, request: number): Promise<void> {
  let state = await readWgpuDiagnosticOutcome(page);
  try {
    await expect.poll(async () => {
      state = await readWgpuDiagnosticOutcome(page);
      return state.completed;
    }, {
      message: "expected a terminal post-load WGPU diagnostic outcome for the Layers panel",
      timeout: scaledMs(5000),
      intervals: [16, 25, 50, 100],
    }).toBeGreaterThanOrEqual(request);
    expect(hasSuccessfulDiagnosticRequest(state, request), JSON.stringify({ request, ...state }))
      .toBe(true);
  } finally {
    const path = test.info().outputPath(`wgpu-diagnostic-request-${request}.json`);
    await writeFile(path, JSON.stringify({ request, ...state }, null, 2));
    await test.info().attach(`wgpu-diagnostic-request-${request}`, {
      path,
      contentType: "application/json",
    });
  }
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

interface RenderAccounting {
  completedResults: number;
  documentCanvasCommits: number;
  overviewInfillRenders: number;
  interaction: Window["__donnerInteractionStats"];
  renderedFrames: number;
}

// One snapshot of everything that can explain a document render count: the
// count itself, the two renders the editor owes itself (see
// `__donnerViewportStats`), and the interaction and frame state around them.
async function readRenderAccounting(page: Page): Promise<RenderAccounting> {
  return page.evaluate(() => ({
    completedResults: window.__donnerWorkerStats?.completedResults || 0,
    documentCanvasCommits: window.__donnerViewportStats?.documentCanvasCommits ?? -1,
    overviewInfillRenders: window.__donnerViewportStats?.overviewInfillRenders ?? -1,
    interaction: window.__donnerInteractionStats,
    renderedFrames: window.__donnerMainLoopRenderedFrames || 0,
  }));
}

// Poll `readRenderAccounting` until `predicate` accepts it, then return the last
// snapshot. Callers assert on the returned snapshot so the failure carries the
// whole render accounting instead of a bare count.
async function pollRenderAccounting(
  page: Page,
  timeoutMs: number,
  predicate: (accounting: RenderAccounting) => boolean,
): Promise<RenderAccounting> {
  const deadline = Date.now() + timeoutMs;
  let accounting = await readRenderAccounting(page);
  while (!predicate(accounting) && Date.now() < deadline) {
    await page.waitForTimeout(25);
    accounting = await readRenderAccounting(page);
  }
  return accounting;
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

async function captureCatalogFontDiagnostics(
  page: Page,
  testInfo: TestInfo,
  networkRequests: readonly string[] = [],
) {
  const diagnostic = await page.evaluate(() => {
    const state = window as Window & {
      __donnerCatalogBrokers?: Map<number, {
        broker: {
          enabled: boolean;
          closed: boolean;
          running: number;
          assets: Map<string, unknown>;
          requests: Map<string, { state: string; token: number }>;
        };
      }>;
    };
    return {
      firstFrame: state.__donnerFirstFramePresented,
      frameCount: state.__donnerMainLoopRenderedFrames,
      activeSample: document.querySelector("canvas")?.getAttribute("data-active-sample-id"),
      sampleStats: state.__donnerSampleThumbnailStats,
      workerStats: state.__donnerWorkerStats,
      interactionStats: state.__donnerInteractionStats,
      fontRequests: state.__catalogFontTest,
      fetchErrors: state.__catalogFetchErrors,
      brokers: Array.from(state.__donnerCatalogBrokers ?? [], ([session, record]) => ({
        session,
        enabled: record.broker.enabled,
        closed: record.broker.closed,
        running: record.broker.running,
        assetCount: record.broker.assets.size,
        requests: Array.from(record.broker.requests, ([id, request]) => ({
          id,
          state: request.state,
          token: request.token,
        })),
      })),
    };
  });
  const diagnosticPath = testInfo.outputPath("catalog-font-final.json");
  await writeFile(
    diagnosticPath,
    JSON.stringify(
      {
        diagnostic,
        networkRequests,
        gpuAcquisitionEvents: gpuAcquisitionEvents.get(page) ?? [],
      },
      null,
      2,
    ),
  );
  await testInfo.attach("catalog-font-final", {
    path: diagnosticPath,
    contentType: "application/json",
  });
  await page.locator("canvas#canvas").screenshot({
    path: testInfo.outputPath("catalog-font-final.png"),
    timeout: 2000,
  });
}

test.beforeEach(async ({ page }, testInfo) => {
  if (!/welcome picker paints before|WGPU diagnostics do not block/.test(testInfo.title)) return;
  await page.addInitScript(() => {
    const nativeFetch = window.fetch;
    window.__catalogFetchErrors = [];
    window.fetch = async function(input, init) {
      try {
        return await Reflect.apply(nativeFetch, this, [input, init]);
      } catch (error) {
        if (String(input).includes("/fonts/")) {
          window.__catalogFetchErrors!.push({
            message: String(error),
            globalReceiver: this === window,
          });
        }
        throw error;
      }
    };
  });
});

test.afterEach(async ({ page }, testInfo) => {
  if (testInfo.status === testInfo.expectedStatus) return;
  if (!/welcome picker paints before|WGPU diagnostics do not block/.test(testInfo.title)) return;
  try {
    await captureCatalogFontDiagnostics(page, testInfo);
  } catch (error) {
    console.error(`catalog font diagnostic capture failed: ${String(error)}`);
  }
});

test("catalog font loading records only fixed GPU acquisition markers", () => {
  const marker = "[Geode/browser/acquire] stage=selection outcome=adapter_rejected";
  const secret = " token=secret https://example.invalid/private/path";
  expect(fixedGpuAcquisitionEvent(marker)).toBe("stage=selection outcome=adapter_rejected");
  expect(fixedGpuAcquisitionEvent(marker + secret)).toBeNull();
  expect(fixedGpuAcquisitionEvent("[Geode/browser/acquire] stage=selection outcome=secret"))
    .toBeNull();
});

test("welcome picker does not render a hidden document", async ({ page }) => {
  const fatalMessages = await openEditor(page, { postInitializationDwellMs: 0 });

  await page.waitForTimeout(750);
  expect(await page.evaluate(() => window.__donnerWorkerStats?.completedResults || 0)).toBe(0);
  expect(fatalMessages).toEqual([]);
});

test("welcome picker paints before asynchronously rendering real SVG thumbnails", async ({ page }) => {
  test.skip(
    kHostedMacosQuarantine,
    "Quarantined in the hosted macOS CI job: offscreen thumbnails can miss the budget (#1702)",
  );
  // Record every thumbnail-lane publication, not only the latest: proving that `drained` never
  // runs ahead of the thumbnails needs each published state, and a poll can miss one frame's.
  await page.addInitScript(() => {
    const publications: ThumbnailLanePublication[] = [];
    let latest: unknown;
    Object.defineProperty(window, "__donnerSampleThumbnailStats", {
      configurable: true,
      get: () => latest,
      set: (value?: Partial<ThumbnailLanePublication>) => {
        latest = value;
        if (publications.length < 4096) {
          publications.push({
            ready: value?.ready ?? 0,
            drained: value?.drained === true,
            active: value?.active === true,
            pending: value?.pending === true,
          });
        }
      },
    });
    (window as Window & { __donnerThumbnailLanePublications?: ThumbnailLanePublication[] })
      .__donnerThumbnailLanePublications = publications;
  });
  const fatalMessages = await openEditor(page, {
    postInitializationDwellMs: 0,
    wgpuReadbackStats: kBackend === "geode",
  });
  const initialDeviceState = await page.evaluate(() => ({
    headlessDeviceCreations: window.__donnerHeadlessDeviceCreations || 0,
  }));

  await expect
    .poll(async () => page.evaluate(() => window.__donnerSampleThumbnailStats?.ready || 0), {
      message: "expected all six catalog SVGs to publish real Donner-rendered thumbnails",
      timeout: 15000,
      intervals: [0, 25, 50, 100],
    })
    .toBe(6);

  // The lane reports drained only once every visible thumbnail is on screen and nothing is in
  // flight. Presentation tests rely on that before they click a sample.
  const readPublications = () =>
    page.evaluate(() =>
      (window as Window & { __donnerThumbnailLanePublications?: ThumbnailLanePublication[] })
        .__donnerThumbnailLanePublications ?? []
    );
  await expect
    .poll(async () => (await readPublications()).some((publication) => publication.drained), {
      message: "expected the thumbnail lane to report drained once all six thumbnails were ready",
      timeout: scaledMs(5000),
      intervals: [16, 25, 50, 100],
    })
    .toBe(true);
  const publications = await readPublications();
  const firstDrained = publications.findIndex((publication) => publication.drained);
  expect(
    publications.slice(0, firstDrained).some((publication) => publication.ready < 6),
    "the lane must report not drained while thumbnails are still outstanding",
  ).toBe(true);
  expect(
    publications.filter((publication) =>
      publication.drained && (publication.ready < 6 || publication.active || publication.pending)
    ),
    `no publication may report drained ahead of the thumbnails: ${JSON.stringify(publications)}`,
  ).toEqual([]);

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
    rendered: 6,
    ready: 6,
    pending: false,
    active: false,
    resultReady: false,
    drained: true,
  });
  expect(settled.thumbnails?.requested).toBeGreaterThanOrEqual(6);
  expect(settled.thumbnails?.started).toBe(settled.thumbnails?.requested);
  expect(settled.thumbnails?.completed).toBe(settled.thumbnails?.started);
  expect(await page.evaluate(() => window.__catalogFetchErrors)).toEqual([]);
  expect(settled.thumbnails?.publicationFrames).toHaveLength(6);
  const publicationFrames = settled.thumbnails?.publicationFrames || [];
  for (let index = 1; index < publicationFrames.length; ++index) {
    expect(publicationFrames[index]).toBeGreaterThan(publicationFrames[index - 1]);
  }
  expect(settled.deviceState).toEqual(initialDeviceState);

  const thumbnailSamples = [
    { id: "donner-splash", column: 0, row: 0 },
    { id: "geode-splash", column: 1, row: 0 },
    { id: "basic-shapes", column: 2, row: 0 },
    { id: "text-style", column: 0, row: 1 },
    { id: "gradients-clip", column: 1, row: 1 },
    { id: "animation", column: 2, row: 1 },
  ] as const;
  if (kBackend === "geode") {
    // The diagnostic readback samples at most four thumbnail textures even when the carousel
    // displays more cards. Publication counts above still cover the full catalog.
    expect(geodeThumbnailStats).toHaveLength(4);
    const fingerprints = new Set<number>();
    for (let index = 0; index < 4; ++index) {
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
    expect(fingerprints.size, "the four sampled card interiors should contain distinct source art")
      .toBe(4);
    expect(geodeThumbnailStats?.[3].backgroundPixels).toBeGreaterThan(1000);
    expect(geodeThumbnailStats?.[3].glyphPixels).toBeGreaterThan(20);
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
      ready: 6,
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

  await page.mouse.click(bounds.x + bounds.width * 0.76, bounds.y + 282);
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
  // The heartbeat gaps are logged, not gated: a wall-clock bound on page timer cadence measures
  // runner scheduling as much as the editor and belongs in a performance lane. This case gates
  // what it can order: both diagnostics complete, the main loop goes idle and wakes for the idle
  // request, and the page timer kept running. The structural guard against a blocking mapping
  // wait is the readback contract test that requires bounded event-loop yields.
  expect(fatalMessages).toEqual([]);
});

test("Firefox hands a blocked thumbnail renderer to a foreground sample load", async ({ browserName, page }) => {
  test.skip(browserName !== "firefox", "Firefox worker-owned WebGPU handoff regression");
  page.on("console", (message) => {
    if (message.type() === "error" || message.type() === "warning") {
      console.log(`firefox-handoff-console[${message.type()}]: ${message.text()}`);
    }
  });
  page.on("pageerror", (error) => {
    console.log(`firefox-handoff-pageerror: ${error.stack || error.message}`);
  });
  const fatalMessages = await openEditor(page, {
    postInitializationDwellMs: 0,
    sampleThumbnailRendererCreationRequest: 2,
    sampleThumbnailRendererCreationDelayMs: 5000,
  });
  const canvas = page.locator("canvas#canvas");
  const bounds = await canvas.boundingBox();
  expect(bounds).not.toBeNull();
  if (bounds === null) {
    return;
  }

  await expect
    .poll(
      () =>
        page.evaluate(
          () =>
            window.__donnerSampleThumbnailRendererCreationBlocked ?? {
              blocked: false,
              constructionStarts: 0,
            },
        ),
      {
        message: "the second worker-owned offscreen renderer must be blocked before replacement",
        timeout: scaledMs(10_000),
        intervals: [8, 16, 25, 50],
      },
    )
    .toEqual({ blocked: true, constructionStarts: 2 });

  const completedBefore = await page.evaluate(
    () => window.__donnerWorkerStats?.completedResults ?? 0,
  );
  await page.mouse.click(bounds.x + bounds.width * 0.24, bounds.y + 282);
  await expect(canvas).toHaveAttribute("data-active-sample-id", "donner-splash", {
    timeout: scaledMs(1000),
  });
  await expect
    .poll(
      () =>
        page.evaluate((before) => {
          const thumbnails = window.__donnerSampleThumbnailStats;
          return {
            firstAttemptCompleted: thumbnails?.firstAttemptCompleted ?? false,
            foregroundHandoffWaits: thumbnails?.foregroundHandoffWaits ?? 0,
            foregroundResultCompleted: (window.__donnerWorkerStats?.completedResults ?? 0) > before,
          };
        }, completedBefore),
      {
        message: "the first offscreen attempt must release before the foreground document result",
        timeout: scaledMs(20_000),
        intervals: [16, 25, 50, 100],
      },
    )
    .toEqual({
      firstAttemptCompleted: true,
      foregroundHandoffWaits: 1,
      foregroundResultCompleted: true,
    });
  expect(fatalMessages).toEqual([]);
});

// Hold one raster task yield until released. The gate resumes the worker from an ordinary task,
// so the release does not wait for an unrelated task to reach the worker.
async function holdRasterTaskYield(page: Page) {
  const raster = await findRasterTaskWorker(page);
  const gate = await gateRasterTaskYields(raster, scaledMs(20000));
  try {
    await gate.awaitSuspended(
      performance.now() + scaledMs(3000),
      "one raster task yield is suspended",
    );
  } catch (error) {
    await gate.release();
    throw error;
  }
  return { raster, release: gate.release, suspended: gate.isSuspended };
}

async function findRasterTaskWorker(page: Page) {
  return Promise.any(
    page.workers().map(async (worker) => {
      const active = await worker.evaluate(() => {
        const scope = globalThis as typeof globalThis & {
          __donnerRasterWorker?: boolean;
          __donnerRasterTaskYield?: { closed: boolean };
        };
        return scope.__donnerRasterWorker === true
          && scope.__donnerRasterTaskYield?.closed === false;
      });
      if (!active) throw new Error("not the active raster worker");
      return worker;
    }),
  );
}

// Deliberate fault injection for the pulse probes: delay every raster task yield so document
// frames arrive at a slow, host-independent cadence. "all" covers both pulses; "queued" covers only
// the click delivered after a raster stall.
const kPulseRasterYieldDelayMs = Number(process.env.DONNER_ANIMATION_PULSE_RASTER_DELAY_MS ?? 0);
const kPulseRasterYieldDelayPhase = process.env.DONNER_ANIMATION_PULSE_RASTER_DELAY_PHASE ?? "all";
if (
  !Number.isFinite(kPulseRasterYieldDelayMs) || kPulseRasterYieldDelayMs < 0
  || !["all", "queued"].includes(kPulseRasterYieldDelayPhase)
) {
  throw new Error("invalid DONNER_ANIMATION_PULSE_RASTER_DELAY_* fault injection");
}

async function delayRasterTaskYields(raster: Worker, delayMs: number) {
  await raster.evaluate((ms) => {
    const scope = globalThis as typeof globalThis & {
      __donnerRasterTaskYield?: { channel: MessageChannel; closed: boolean };
      __donnerRasterYieldDelayRestore?: () => void;
    };
    const state = scope.__donnerRasterTaskYield;
    const port = state?.channel.port1;
    const original = port?.onmessage;
    if (!state || state.closed || !port || !original) throw new Error("raster yield unavailable");
    if (scope.__donnerRasterYieldDelayRestore) throw new Error("raster yield delay installed");
    const delayed = (event: MessageEvent) => {
      setTimeout(() => original.call(port, event), ms);
    };
    port.onmessage = delayed;
    scope.__donnerRasterYieldDelayRestore = () => {
      // Holds wrap and restore the current handler, so only an unwrapped delay is removable.
      if (port.onmessage === delayed) port.onmessage = original;
      delete scope.__donnerRasterYieldDelayRestore;
    };
  }, delayMs);
  return () =>
    boundBrowserDiagnostic(
      raster.evaluate(() => {
        const scope = globalThis as typeof globalThis & {
          __donnerRasterYieldDelayRestore?: () => void;
        };
        scope.__donnerRasterYieldDelayRestore?.();
      }),
      1000,
    );
}

// Deliberate fault injection modelling a browser that defers a worker's promise continuation when
// the worker is resumed from inside a test evaluation: such a resumption reaches the raster worker
// only after this delay, while a resumption from an ordinary worker task is immediate. It checks
// how the helpers route their resumptions; it cannot reproduce the browser behaviour itself or
// catch an unmarked resumption site.
const kEvaluationResumeDeferMs = Number(
  process.env.DONNER_ANIMATION_EVALUATION_RESUME_DEFER_MS ?? 0,
);
if (!Number.isFinite(kEvaluationResumeDeferMs) || kEvaluationResumeDeferMs < 0) {
  throw new Error("invalid DONNER_ANIMATION_EVALUATION_RESUME_DEFER_MS fault injection");
}

type EvaluationResumeScope = typeof globalThis & {
  __donnerResumingFromTestEvaluation?: boolean;
  __donnerEvaluationResumeDeferred?: boolean;
};

// The raster worker's shutdown clears the yield handler and with it this deferral.
async function deferEvaluationResumes(raster: Worker, deferMs: number) {
  await raster.evaluate((ms) => {
    const scope = globalThis as EvaluationResumeScope & {
      __donnerRasterTaskYield?: { channel: MessageChannel; closed: boolean };
    };
    const state = scope.__donnerRasterTaskYield;
    const port = state?.channel.port1;
    const original = port?.onmessage;
    if (!state || state.closed || !port || !original) throw new Error("raster yield unavailable");
    if (scope.__donnerEvaluationResumeDeferred) throw new Error("resume deferral installed");
    scope.__donnerEvaluationResumeDeferred = true;
    port.onmessage = (event: MessageEvent) => {
      if (scope.__donnerResumingFromTestEvaluation) {
        setTimeout(() => original.call(port, event), ms);
      } else {
        original.call(port, event);
      }
    };
  }, deferMs);
}

type RasterGateScope = typeof globalThis & {
  __donnerRasterTaskYield?: {
    channel: MessageChannel;
    resume: (() => void) | null;
    closed: boolean;
  };
  __donnerRasterGate?: {
    id: string;
    suspended: () => boolean;
    step: () => void;
    release: () => void;
  };
  __donnerRasterRenderTrace?: number[] & { overflow?: boolean };
};

// Hold every raster task yield and release them one at a time. The worker rasterizes at most one
// frame between two yields, so while a yield is held no newer frame can reach presentation, and
// the render trace names the document time of each rasterized frame. A finite worker-side lease
// releases the gate if the test stops driving it.
async function gateRasterTaskYields(raster: Worker, leaseMs: number) {
  const lease = { id: `${Date.now()}-${leaseMs}`, expiresEpochMs: Date.now() + leaseMs };
  await raster.evaluate(({ id, expiresEpochMs }) => {
    const scope = globalThis as RasterGateScope;
    if (scope.__donnerRasterGate) throw new Error("raster gate already installed");
    const state = scope.__donnerRasterTaskYield;
    const port = state?.channel.port1;
    const original = port?.onmessage;
    if (!state || state.closed || !port || !original) throw new Error("raster yield unavailable");
    let held: MessageEvent | undefined;
    let released = false;
    // Resume the worker from an ordinary task, never from inside a test evaluation. A resumption
    // made inside an evaluation can leave the worker's promise continuation waiting for a later,
    // unrelated task in this worker; Firefox showed this as steps of one to two seconds.
    const resumeChannel = new MessageChannel();
    let resumeEvent: MessageEvent | undefined;
    resumeChannel.port1.onmessage = () => {
      const event = resumeEvent;
      resumeEvent = undefined;
      if (event) original.call(port, event);
    };
    const resume = (event: MessageEvent) => {
      if (resumeEvent) throw new Error("a raster yield resumption is already pending");
      resumeEvent = event;
      resumeChannel.port2.postMessage(0);
    };
    const release = () => {
      if (released) return;
      released = true;
      clearTimeout(timer);
      port.onmessage = original;
      if (scope.__donnerRasterGate?.id === id) delete scope.__donnerRasterGate;
      delete scope.__donnerRasterRenderTrace;
      const event = held;
      held = undefined;
      if (event) resume(event);
    };
    port.onmessage = (event) => {
      if (held) {
        release();
        throw new Error("multiple raster yield messages held");
      }
      held = event;
    };
    scope.__donnerRasterRenderTrace = [];
    scope.__donnerRasterGate = {
      id,
      suspended: () => held !== undefined && state.resume !== null,
      step: () => {
        const event = held;
        if (!event) throw new Error("no raster yield is held");
        held = undefined;
        resume(event);
      },
      release,
    };
    const timer = setTimeout(release, Math.max(1, expiresEpochMs - Date.now()));
  }, lease);
  return {
    async awaitSuspended(deadlineAtMs: number, message = "the raster worker suspends at a yield") {
      await expect.poll(() =>
        awaitBeforeBlueDeadline(
          raster.evaluate((id) => {
            const gate = (globalThis as RasterGateScope).__donnerRasterGate;
            return gate?.id === id && gate.suspended();
          }, lease.id),
          deadlineAtMs,
        ), {
        timeout: Math.max(1, deadlineAtMs - performance.now()),
        intervals: [5],
        message,
      }).toBe(true);
    },
    isSuspended: () =>
      awaitBeforeBlueDeadline(
        raster.evaluate((id) => {
          const gate = (globalThis as RasterGateScope).__donnerRasterGate;
          return gate?.id === id && gate.suspended();
        }, lease.id),
        performance.now() + scaledMs(1000),
      ),
    renderedTimes: (deadlineAtMs: number) =>
      awaitBeforeBlueDeadline(
        raster.evaluate((id) => {
          const scope = globalThis as RasterGateScope;
          if (scope.__donnerRasterGate?.id !== id) throw new Error("the raster gate was released");
          const trace = scope.__donnerRasterRenderTrace ?? [];
          if (trace.overflow) throw new Error("the raster render trace overflowed");
          return [...trace];
        }, lease.id),
        deadlineAtMs,
      ),
    step: (deadlineAtMs: number) =>
      awaitBeforeBlueDeadline(
        raster.evaluate((id) => {
          const scope = globalThis as RasterGateScope & EvaluationResumeScope;
          const gate = scope.__donnerRasterGate;
          if (gate?.id !== id) throw new Error("the raster gate was released");
          scope.__donnerResumingFromTestEvaluation = true;
          try {
            gate.step();
          } finally {
            scope.__donnerResumingFromTestEvaluation = false;
          }
        }, lease.id),
        deadlineAtMs,
      ),
    release: () =>
      awaitBeforeBlueDeadline(
        raster.evaluate((id) => {
          const scope = globalThis as RasterGateScope & EvaluationResumeScope;
          const gate = scope.__donnerRasterGate;
          if (gate?.id !== id) return;
          scope.__donnerResumingFromTestEvaluation = true;
          try {
            gate.release();
          } finally {
            scope.__donnerResumingFromTestEvaluation = false;
          }
        }, lease.id),
        performance.now() + scaledMs(1000),
      ),
  };
}

type RasterGate = Awaited<ReturnType<typeof gateRasterTaskYields>>;

async function readPreviewPointerTrace(owner: Worker, deadlineAtMs: number) {
  return awaitBeforeBlueDeadline(
    owner.evaluate(() => {
      const trace = (globalThis as unknown as Record<string, unknown>)
        .__donnerPreviewPointerTrace as (unknown[] & { generation?: number }) | undefined;
      return {
        generation: trace?.generation,
        entries: Array.isArray(trace) ? [...trace] : [],
      };
    }),
    deadlineAtMs,
  );
}

// Step a gated raster worker until its newest rasterized frame shows the expanded pulse of this
// click and a later timeline sample proves that frame was presented. The gate stays held, so that
// frame remains the presented one until the caller releases the gate.
async function retainGatedPulseFrame({
  gate,
  owner,
  generation,
  target,
  notBeforeEpochMs,
  deadlineAtMs,
  decisions,
}: {
  gate: RasterGate;
  owner: Worker;
  generation: number;
  target: number | null;
  notBeforeEpochMs: number;
  deadlineAtMs: number;
  decisions: unknown[];
}) {
  for (;;) {
    await gate.awaitSuspended(deadlineAtMs);
    const rendered = await gate.renderedTimes(deadlineAtMs);
    const snapshot = await readPreviewPointerTrace(owner, deadlineAtMs);
    const decision = gatedPulseFrame(rendered, snapshot, generation, target, notBeforeEpochMs);
    if (decisions.length < 128) {
      decisions.push({ atEpochMs: Date.now(), renderedFrames: rendered.length, ...decision });
    }
    if (decision.action === "capture") return { ...decision, renderedFrames: rendered.length };
    if (decision.action === "overshoot" || decision.action === "invalid") {
      throw new Error(`no expanded pulse frame can be retained: ${JSON.stringify(decision)}`);
    }
    if (performance.now() >= deadlineAtMs) throw new Error("pulse frame deadline exceeded");
    if (decision.action === "step") {
      await gate.step(deadlineAtMs);
    } else {
      // The expanded frame is rasterized; the editor has not yet admitted the proving sample.
      await new Promise((resolve) => setTimeout(resolve, 16));
    }
  }
}

// Capturing pixels must not let the gated worker rasterize a newer frame.
async function expectGatedFrameRetained(
  gate: RasterGate,
  retained: { renderedFrames: number },
  deadlineAtMs: number,
) {
  await gate.awaitSuspended(deadlineAtMs, "the raster worker is still suspended after capture");
  expect(
    (await gate.renderedTimes(deadlineAtMs)).length,
    "no newer frame was rasterized during capture",
  ).toBe(retained.renderedFrames);
}

// Run a probe against a gated raster worker and always release the gate.
async function withRasterGate<T>(gate: RasterGate, body: () => Promise<T>, message: string) {
  const errors: unknown[] = [];
  let result: T | undefined;
  try {
    result = await body();
  } catch (error) {
    errors.push(error);
  }
  try {
    await gate.release();
  } catch (error) {
    errors.push(error);
  }
  if (errors.length === 1) throw errors[0];
  if (errors.length > 1) throw new AggregateError(errors, message);
  return result as T;
}

// While a pulse probe observes its window, preview time advances one 60 Hz tick per admitted
// sample instead of following the wall clock, so one slow host frame cannot carry the timeline past
// the window. Wall-clock playback resumes from the same time afterwards. Only this helper sets the
// opt-in, so it must be unset when the probe starts.
async function withFramePacedPreviewClock<T>(owner: Worker, body: () => Promise<T>) {
  const setPacing = (enabled: boolean) =>
    awaitBeforeBlueDeadline(
      owner.evaluate((on) => {
        const scope = globalThis as unknown as Record<string, unknown>;
        const previous = scope.__donnerFramePacedPreviewClock;
        if (on) scope.__donnerFramePacedPreviewClock = true;
        else delete scope.__donnerFramePacedPreviewClock;
        return previous === undefined ? "unset" : String(previous);
      }, enabled),
      performance.now() + scaledMs(1000),
    );
  expect(await setPacing(true), "the frame-paced preview clock is test-only").toBe("unset");
  const errors: unknown[] = [];
  let result: T | undefined;
  try {
    result = await body();
  } catch (error) {
    errors.push(error);
  }
  try {
    await setPacing(false);
  } catch (error) {
    errors.push(error);
  }
  if (errors.length === 1) throw errors[0];
  if (errors.length > 1) throw new AggregateError(errors, "frame-paced pulse probe failed");
  return result as T;
}

async function verifyRasterProgressWithTimersHeld(page: Page) {
  const raster = await Promise.any(
    page.workers().map(async (worker) => {
      await worker.evaluate(() => {
        const scope = globalThis as typeof globalThis & {
          __donnerRasterWorker?: boolean;
          __donnerReleaseHeldTimers?: () => void;
          __donnerHeldTimerState?: () => { pending: number; probeRan: boolean };
        };
        if (!scope.__donnerRasterWorker) throw new Error("not the raster worker");
        const originalSet = globalThis.setTimeout.bind(globalThis);
        const originalClear = globalThis.clearTimeout.bind(globalThis);
        const held = new Map<number, () => void>();
        let nextId = -1;
        let probeRan = false;
        globalThis.setTimeout = (handler, delay, ...args) => {
          if (Number(delay ?? 0) !== 0) return originalSet(handler, delay, ...args);
          if (held.size >= 64) throw new Error("held raster timer probe exceeded its bound");
          const id = nextId--;
          held.set(id, () => originalSet(handler, 0, ...args));
          return id;
        };
        globalThis.clearTimeout = (id) => {
          if (!held.delete(Number(id))) originalClear(id);
        };
        scope.__donnerHeldTimerState = () => ({ pending: held.size, probeRan });
        scope.__donnerReleaseHeldTimers = () => {
          globalThis.setTimeout = originalSet;
          globalThis.clearTimeout = originalClear;
          const callbacks = [...held.values()];
          held.clear();
          delete scope.__donnerHeldTimerState;
          delete scope.__donnerReleaseHeldTimers;
          for (const callback of callbacks) callback();
        };
        setTimeout(() => {
          probeRan = true;
        }, 0);
      });
      return worker;
    }),
  );
  try {
    const before = await page.evaluate(() => window.__donnerWorkerStats?.completedResults ?? 0);
    await expect.poll(
      () => page.evaluate(() => window.__donnerWorkerStats?.completedResults ?? 0),
      { timeout: scaledMs(3000), message: "raster progresses while zero-delay timers are held" },
    ).toBeGreaterThanOrEqual(before + 3);
    const state = await raster.evaluate(() => {
      const scope = globalThis as typeof globalThis & {
        __donnerHeldTimerState?: () => { pending: number; probeRan: boolean };
      };
      return scope.__donnerHeldTimerState?.();
    });
    expect(state?.pending).toBeGreaterThanOrEqual(1);
    expect(state?.probeRan).toBe(false);
  } finally {
    await raster.evaluate(() => {
      const scope = globalThis as typeof globalThis & { __donnerReleaseHeldTimers?: () => void };
      scope.__donnerReleaseHeldTimers?.();
    });
  }
  expect(
    await raster.evaluate(() =>
      new Promise<boolean>((resolve) => {
        setTimeout(() => resolve(true), 0);
      })
    ),
  ).toBe(true);
}

async function startOwnerFrameProbe(page: Page, sampleRaf: boolean): Promise<Worker> {
  const startup = Promise.any(
    page.workers().map(async (worker) => {
      await worker.evaluate((sampleRaf) => {
        const state = globalThis as unknown as Record<string, unknown>;
        if (!state.__donnerGpuOwner) throw new Error("not the GPU owner");
        const counts = Array(9).fill(0);
        state.__donnerPreviewAdvanceCounts = counts;
        const samples: number[][] = [];
        const registrations: number[][] = [];
        const deadline = performance.now() + 25000;
        const callbackIds = new Map<FrameRequestCallback, number>();
        const originalRaf = globalThis.requestAnimationFrame.bind(globalThis);
        const previousRaf = globalThis.requestAnimationFrame;
        let recording = true;
        const tracedRaf: typeof requestAnimationFrame = (callback) => {
          if (!recording || registrations.length >= 4096 || performance.now() >= deadline) {
            return originalRaf(callback);
          }
          if (!callbackIds.has(callback) && callbackIds.size < 16) {
            callbackIds.set(callback, callbackIds.size + 1);
          }
          const record = [callbackIds.get(callback) ?? 0, performance.now(), -1, -1, -1];
          if (recording && registrations.length < 4096) registrations.push(record);
          return originalRaf(function(this: unknown, timestamp) {
            record[2] = timestamp;
            record[3] = performance.now();
            try {
              return callback.call(this, timestamp);
            } finally {
              record[4] = performance.now();
            }
          });
        };
        globalThis.requestAnimationFrame = tracedRaf;
        let request = 0;
        const tick = (timestamp: number) => {
          samples.push([timestamp, performance.now()]);
          if (samples.length < 4096 && performance.now() < deadline) {
            request = requestAnimationFrame(tick);
          }
        };
        if (sampleRaf) request = requestAnimationFrame(tick);
        state.__donnerStopOwnerRafProbe = () => {
          cancelAnimationFrame(request);
          recording = false;
          if (globalThis.requestAnimationFrame === tracedRaf) {
            globalThis.requestAnimationFrame = previousRaf;
          }
          delete state.__donnerStopOwnerRafProbe;
          delete state.__donnerPreviewAdvanceCounts;
          return { timeOrigin: performance.timeOrigin, samples, counts, registrations };
        };
      }, sampleRaf);
      return worker;
    }),
  );
  const owner = await boundBrowserDiagnostic(startup, 2000);
  if (!owner) {
    void startup.then(stopOwnerFrameProbe).catch(() => null);
    throw new Error("GPU owner rAF probe did not start");
  }
  return owner;
}

async function stopOwnerFrameProbe(worker: Worker) {
  return boundBrowserDiagnostic(
    worker.evaluate(() => {
      const state = globalThis as unknown as Record<string, unknown>;
      const stop = state.__donnerStopOwnerRafProbe as
        | (() => {
          timeOrigin: number;
          samples: number[][];
          counts: number[];
          registrations: number[][];
        })
        | undefined;
      return stop?.() ?? null;
    }),
    2000,
  );
}

// Withhold the GPU owner's scheduled frames. Every requestAnimationFrame callback is held instead
// of registered, so no scheduled frame can run until `releaseWorkerAnimationFrames`. While the
// hold is in place, only event frames and explicitly driven frames reach the canvas.
async function holdWorkerAnimationFrames(page: Page) {
  let timer: ReturnType<typeof setTimeout> | undefined;
  try {
    await Promise.race([
      Promise.any(
        page.workers().map((worker) =>
          worker.evaluate(() => {
            const scope = globalThis as typeof globalThis & {
              __donnerGpuOwner?: boolean;
              __donnerHeldRaf?: unknown;
            };
            if (!scope.__donnerGpuOwner) throw new Error("not the GPU owner");
            const originalRequest = globalThis.requestAnimationFrame;
            const originalCancel = globalThis.cancelAnimationFrame;
            const held = new Map<number, FrameRequestCallback>();
            // Real handles are small positive integers; held handles stay clear of them.
            let nextHandle = 0x40000000;
            let requested = 0;
            let ranAfterRelease = 0;
            let released = false;
            globalThis.requestAnimationFrame = (callback) => {
              ++requested;
              const handle = ++nextHandle;
              held.set(handle, callback);
              return handle;
            };
            globalThis.cancelAnimationFrame = (handle) => {
              if (!held.delete(handle)) originalCancel.call(globalThis, handle);
            };
            scope.__donnerHeldRaf = {
              state: () => ({ requested, held: held.size, released, ranAfterRelease }),
              release: () => {
                if (released) return;
                released = true;
                globalThis.requestAnimationFrame = originalRequest;
                globalThis.cancelAnimationFrame = originalCancel;
                for (const callback of held.values()) {
                  originalRequest.call(globalThis, (timestamp) => {
                    ++ranAfterRelease;
                    callback(timestamp);
                  });
                }
                held.clear();
              },
            };
          })
        ),
      ),
      new Promise<never>((_, reject) => {
        timer = setTimeout(() => reject(new Error("GPU owner did not accept the rAF hold")), 2000);
      }),
    ]);
  } finally {
    if (timer !== undefined) clearTimeout(timer);
  }
}

type HeldAnimationFrameState = {
  requested: number;
  held: number;
  released: boolean;
  ranAfterRelease: number;
};

// Counters of the hold installed above, or null when no GPU owner carries one.
async function readHeldAnimationFrames(page: Page): Promise<HeldAnimationFrameState | null> {
  for (const worker of page.workers()) {
    const state = await worker.evaluate(() => {
      const hold = (globalThis as unknown as {
        __donnerHeldRaf?: { state: () => HeldAnimationFrameState };
      }).__donnerHeldRaf;
      return hold ? hold.state() : null;
    }).catch(() => null);
    if (state) return state;
  }
  return null;
}

async function releaseWorkerAnimationFrames(page: Page) {
  for (const worker of page.workers()) {
    await worker.evaluate(() => {
      (globalThis as unknown as { __donnerHeldRaf?: { release: () => void } }).__donnerHeldRaf
        ?.release();
    }).catch(() => null);
  }
}

// The test-only barrier between a finished document render and its result
// (`AsyncRenderer::requestCompletionBarrierForTesting`). A request arms it when the next sample
// activates; the first render of that sample to finish then waits in "holding", with no result to
// poll and no completion wake fired, until it is released. Each command returns the prior state.
const completionBarrierStates = ["off", "requested", "armed", "holding"] as const;

async function completionBarrier(page: Page, command: "read" | "request" | "release") {
  const state = await page.evaluate(
    (code) =>
      (window as unknown as {
        Module: { _donner_render_completion_barrier_for_testing: (command: number) => number };
      }).Module._donner_render_completion_barrier_for_testing(code),
    { read: 0, request: 1, release: 2 }[command],
  );
  return completionBarrierStates[state] ?? `unknown ${state}`;
}

async function readCarouselFrameDiagnostics(page: Page) {
  let timer: ReturnType<typeof setTimeout> | undefined;
  try {
    return await Promise.race([
      page.evaluate(() => {
        const state = window as unknown as Record<string, unknown>;
        return Object.fromEntries([
          "__donnerFrameLoopStats",
          "__donnerHostFrameTiming",
          "__donnerFrameTickStats",
          "__donnerFrameAdmissionStats",
          "__donnerAsyncifySuspendStats",
          "__donnerSampleThumbnailStats",
          "__donnerPresentationQueueStats",
          "__donnerRasterWorkState",
        ].map((name) => [name, state[name] ?? null]));
      }).catch(() => null),
      new Promise<null>((resolve) => {
        timer = setTimeout(() => resolve(null), 2000);
      }),
    ]);
  } finally {
    if (timer !== undefined) clearTimeout(timer);
  }
}

// Queue timeline around the first sample click: every GPU submission with its completion, the
// raster worker phases, and each UI frame's presentation admission, all on the page clock. It is
// what separates a presentation held behind earlier GPU work from a late wake.
async function readCarouselGpuTimeline(page: Page) {
  const withDeadline = <T>(work: Promise<T>): Promise<T | null> =>
    boundBrowserDiagnostic(work, 2000);

  const pageState = await withDeadline(page.evaluate(() => {
    const state = window as unknown as Record<string, unknown>;
    return {
      timeOrigin: performance.timeOrigin,
      presentation: state.__donnerPresentationTimeline ?? null,
      phases: state.__donnerRasterWorkPhases ?? null,
    };
  }));
  let gpu: unknown[] | null = null;
  for (const worker of page.workers()) {
    const entries = await withDeadline(worker.evaluate(() => {
      const read = (globalThis as unknown as Record<string, unknown>).__donnerReadGpuTimeline;
      return typeof read === "function" ? (read as () => unknown[])() : null;
    }));
    if (entries !== null && entries.length > 0) {
      gpu = entries;
      break;
    }
  }
  if (pageState === null) return null;
  const toPage = (epochMs: number) =>
    epochMs > 0 ? Math.round((epochMs - pageState.timeOrigin) * 10) / 10 : 0;
  const gpuOnPageClock = gpu?.map((entry) => {
    const fields = entry as (string | number)[];
    switch (fields[0]) {
      case "s":
        return [...fields.slice(0, 5), toPage(fields[5] as number), toPage(fields[6] as number)];
      case "m":
        return [...fields.slice(0, 3), toPage(fields[3] as number), toPage(fields[4] as number)];
      case "p":
        return [
          fields[0],
          fields[1],
          toPage(fields[2] as number),
          toPage(fields[3] as number),
          fields[4],
        ];
      default:
        return [...fields.slice(0, 2), toPage(fields[2] as number), ...fields.slice(3)];
    }
  }) ?? null;
  return { presentation: pageState.presentation, phases: pageState.phases, gpu: gpuOnPageClock };
}

// The worker that owns the browser GPU device. Every GPU call is made there, so its bridge counts
// every pipeline compile the editor has in flight.
async function findGpuOwnerWorker(page: Page): Promise<Worker | null> {
  return boundBrowserDiagnostic(Promise.any(page.workers().map(async (worker) => {
    const owned = await worker.evaluate(() =>
      Boolean((globalThis as typeof globalThis & { __donnerGpuOwner?: boolean }).__donnerGpuOwner)
    );
    if (!owned) throw new Error("not the GPU owner");
    return worker;
  })), 2000);
}

async function readOwnerGpuStatistics(owner: Worker) {
  return await boundBrowserDiagnostic(owner.evaluate(() =>
    (globalThis as typeof globalThis & { __donnerReadGpuObjectStats?: () => unknown })
      .__donnerReadGpuObjectStats?.() ?? null
  ), 2000) as Record<string, unknown> | null;
}

async function readPipelineCompilesPending(owner: Worker) {
  return sumPipelineCompilesPending([await readOwnerGpuStatistics(owner)]);
}

// How many pipelines the GPU owner has created at their first use. Such a creation compiles in
// order with the device's work, so on a cold shader cache it holds every later submission,
// presentation included.
async function readPipelinesCreatedAtFirstUse(page: Page) {
  const owner = await findGpuOwnerWorker(page);
  const statistics = owner && await readOwnerGpuStatistics(owner);
  const count = statistics?.pipelineCompilesOnFirstUse;
  return typeof count === "number" ? count : null;
}

// The editor starts its pipelines' asynchronous compiles when it first builds them, and its first
// frame can present while they are still compiling: a cold shader cache can take seconds, and the
// browser can hold later GPU work behind those compiles. An interaction measured against a nominal
// bound therefore starts once the GPU owner reports no compile in flight. The wait observes that
// state under the stall guard; the measurement after it stays raw.
async function waitForPipelineCompilesToSettle(page: Page) {
  const startedAt = performance.now();
  const readiness = { pendingAtStart: -1, maxPending: 0, polls: 0, waitMs: 0 };
  let owner: Worker | null = null;
  let last = { pending: 0, reporting: 0 };
  try {
    await expect.poll(async () => {
      readiness.polls += 1;
      // A loaded owner can miss one bounded probe; finding it again is part of the same wait.
      owner ??= await findGpuOwnerWorker(page);
      last = owner === null ? { pending: 0, reporting: 0 } : await readPipelineCompilesPending(owner);
      if (readiness.pendingAtStart < 0 && last.reporting > 0) readiness.pendingAtStart = last.pending;
      readiness.maxPending = Math.max(readiness.maxPending, last.pending);
      return last.reporting > 0 && last.pending === 0;
    }, {
      message: "the GPU owner's asynchronous pipeline compiles settle before the measured click",
      intervals: [16, 25, 50, 100],
    }).toBe(true);
  } finally {
    readiness.waitMs = Math.round(performance.now() - startedAt);
    console.log(`pipeline-compile-readiness ${JSON.stringify({ ...readiness, last })}`);
  }
  return readiness;
}

// Delays the GPU owner's pipeline compiles from the editor's first instruction, as a cold shader
// cache does. Every context loads the editor script, so the fault rides in front of it. The compile
// delay must outlast boot to the first presented frame, completions included, so the readiness
// wait still finds compiles pending. The case asserts that the click waited for them and that the
// interaction created no pipeline at its first use; the completion delay and the first-use compile
// time each exceed the nominal handoff bound that the performance lane measures.
const kPipelineCompileFault = { compileDelayMs: 8000, completionDelayMs: 1200, firstUseCompileMs: 1200 };

async function readOwnerPipelineCompileFault(page: Page) {
  const owner = await findGpuOwnerWorker(page);
  return owner && await boundBrowserDiagnostic(owner.evaluate(readPipelineCompileFault), 2000);
}

async function armPipelineCompileFault(page: Page) {
  const prologue = pipelineCompileFaultPrologue(kPipelineCompileFault);
  await page.route("**/editor.js", async (route) => {
    const response = await route.fetch();
    await route.fulfill({ response, body: prologue + await response.text() });
  });
}

const kBasicShapesTile = kCarouselSamples.find((sample) => sample.id === "basic-shapes")!;
const kDonnerSplashTile = kCarouselSamples.find((sample) => sample.id === "donner-splash")!;

for (
  const sample of [
    ...kCarouselSamples,
    {
      ...kBasicShapesTile,
      name: "Basic Shapes with withheld animation frames",
      holdWorkerRaf: true,
    },
    {
      ...kBasicShapesTile,
      name: "Basic Shapes with delayed presentation control",
      holdWorkerRaf: true,
      delayPresentation: true,
    },
    {
      ...kDonnerSplashTile,
      name: "Donner Splash after delayed pipeline compiles",
      delayPipelineCompiles: true,
    },
  ]
) {
  test(`carousel loads ${sample.name} on the first interactive frame`, async ({ page, browserName }, testInfo) => {
    const delayedControl = "delayPresentation" in sample;
    const compileFaultControl = "delayPipelineCompiles" in sample;
    test.skip(
      (delayedControl || compileFaultControl) && browserName !== "chromium",
      "controlled Chromium scheduling fault",
    );
    let presentationFault:
      | { delayMs: number; requested: number; flushed: number; releaseRequestedAtMs: number }
      | undefined;
    if (compileFaultControl) {
      // The fault adds up to its compile delay before the measured click.
      testInfo.setTimeout(testInfo.timeout + kPipelineCompileFault.compileDelayMs);
      await armPipelineCompileFault(page);
    }
    const fatalMessages = await openEditor(page, { postInitializationDwellMs: 0 });
    const canvas = page.locator("canvas#canvas");
    const bounds = await canvas.boundingBox();
    expect(bounds).not.toBeNull();
    if (bounds === null) {
      return;
    }

    // The completion wake must not depend on the scheduler's animation frames. Delaying those
    // frames cannot show that: a delayed frame that happens to become due between the completion
    // wake and the dispatch of its event frame legitimately carries the result first, and the event
    // frame still follows at once. So the scheduled frames are withheld outright from before the
    // click until the result is on the canvas.
    const holdsScheduledFrames = "holdWorkerRaf" in sample;
    if (holdsScheduledFrames) {
      expect(
        await page.evaluate(() =>
          (window as Window & { __donnerFrameDriver?: string }).__donnerFrameDriver
        ),
      ).toBe("worker-raf");
      await holdWorkerAnimationFrames(page);
      await expect.poll(async () => (await readHeldAnimationFrames(page))?.held ?? 0, {
        message: "the scheduler is waiting on a withheld animation frame before the click",
      }).toBeGreaterThan(0);
      expect(await completionBarrier(page, "request"), "the completion barrier is test-only")
        .toBe("off");
    }
    const pipelineReadiness = await waitForPipelineCompilesToSettle(page);
    let pipelineCompileFault: ReturnType<typeof readPipelineCompileFault> = null;
    if (compileFaultControl) {
      // The fault must have reached the GPU owner and still held its compiles when the wait began,
      // or this case would not show that the click waited for them.
      pipelineCompileFault = await readOwnerPipelineCompileFault(page);
      expect(pipelineCompileFault?.compilesDelayed ?? 0, "the GPU owner's compiles were delayed")
        .toBeGreaterThan(0);
      expect(pipelineCompileFault?.compilesPending).toBe(0);
      expect(pipelineReadiness.pendingAtStart, "compiles were still pending when the wait began")
        .toBeGreaterThan(0);
      // From here on, a pipeline created at its first use holds the device timeline as a cold
      // compile does. Boot's own first-use creations ran before this and are not measured.
      const owner = await findGpuOwnerWorker(page);
      expect(owner && await owner.evaluate(armPipelineFirstUseCompiles)).toBe(true);
    }
    // Fault injection, off unless `DONNER_CAROUSEL_CANVAS_GPU_DELAY` is `<everyFrames>:<iterations>`:
    // queues fixed GPU work right after the first canvas write of every Nth frame from the click
    // on, so the presenting frame retires behind it as on a slow or shared GPU. CPU frame timing,
    // task boundaries and every ordering below are unchanged.
    let carouselGpuDelay: Awaited<ReturnType<typeof armAttachedCanvasGpuDelay>> = null;
    if (canvasGpuDelayFromEnv("DONNER_CAROUSEL_CANVAS_GPU_DELAY") !== null) {
      await expectSurfaceFrameProbeReady(page);
      const owner = await findProbedCanvasApplicationWorker(page);
      if (owner === null) throw new Error("the application canvas owner is unavailable");
      carouselGpuDelay = await armAttachedCanvasGpuDelay(
        owner,
        "DONNER_CAROUSEL_CANVAS_GPU_DELAY",
        "carousel-canvas-gpu-delay",
        testInfo,
      );
    }
    const firstUseCreationsBeforeClick = await readPipelinesCreatedAtFirstUse(page);
    let firstUseCreationsAtPresentation: number | null = null;
    const deviceCreationsBeforeClick = await page.evaluate(
      () => window.__donnerHeadlessDeviceCreations ?? -1,
    );
    const beforeSample = await page.evaluate(
      () => window.__donnerWorkerStats?.completedResults || 0,
    );
    const beforeFrames = await page.evaluate(() => window.__donnerMainLoopRenderedFrames || 0);
    const clickStartedAt = await page.evaluate(() => performance.now());
    await page.mouse.click(bounds.x + bounds.width * sample.xFraction, bounds.y + sample.y);
    if (holdsScheduledFrames) {
      // Input waits for a scheduled frame by design, so drive the frames that apply the click
      // through the main-thread tick instead of the withheld worker animation frames. The frame
      // that activates the sample arms the completion barrier and defers its render request to a
      // following frame, so keep ticking until the raster worker holds that finished render.
      await expect.poll(async () =>
        page.evaluate((holding) => {
          const module = (window as unknown as {
            Module: {
              _donner_render_completion_barrier_for_testing: (command: number) => number;
              _donner_whole_app_vsync_tick: () => void;
            };
          }).Module;
          if (module._donner_render_completion_barrier_for_testing(0) === holding) return true;
          module._donner_whole_app_vsync_tick();
          return false;
        }, completionBarrierStates.indexOf("holding")), {
        message: `expected ${sample.name} to activate and finish its render from driven frames`,
        timeout: kStallGuardMs,
        intervals: [8, 16, 25, 50],
      }).toBe(true);
      // A tick called just before the poll saw the hold can still be queued or running. Let every
      // driven tick finish while the result cannot exist yet; from then on only an event frame can
      // poll it.
      await expect.poll(
        () =>
          page.evaluate(() =>
            (window as unknown as { Module: { _donner_whole_app_vsync_pending: () => number } })
              .Module._donner_whole_app_vsync_pending()
          ),
        {
          message: "every driven tick has finished before the held render is released",
          timeout: kStallGuardMs,
        },
      ).toBe(0);
      // A draining tick may have superseded the held render; the barrier then holds the render
      // that replaced it. No driven tick runs now, so only an event frame could supersede the hold
      // before the release.
      await expect.poll(() => completionBarrier(page, "read"), {
        message: "the sample's finished render is held once driven ticks have drained",
        timeout: kStallGuardMs,
      }).toBe("holding");
      if (delayedControl) {
        const owner = await boundBrowserDiagnostic(Promise.any(page.workers().map(async (worker) => {
          await worker.evaluate(installOwnerEventTimerHold);
          return worker;
        })), 2000);
        if (!owner) throw new Error("the GPU owner did not acknowledge the event-task hold");
        try {
          expect(await completionBarrier(page, "release"), "the finished render was still held")
            .toBe("holding");
          await expect.poll(async () => (await owner.evaluate(readOwnerEventTimerHold))?.pending ?? 0,
            { message: "the completed result must request a held event task" }).toBeGreaterThan(0);
          await page.waitForTimeout(1500);
        } finally {
          // Page clock just before the release: the held event frame cannot present before it. A
          // failed read must not skip the release below.
          const releaseRequestedAtMs = await page.evaluate(() => performance.now())
            .catch(() => Number.NaN);
          const released = await owner.evaluate(releaseOwnerEventTimerHold);
          presentationFault = { delayMs: 1500, releaseRequestedAtMs, ...released };
        }
        expect(presentationFault.flushed).toBeGreaterThan(0);
      } else {
        expect(await completionBarrier(page, "release"), "the finished render was still held")
          .toBe("holding");
      }
    }
    // This case asserts ordering only: which frame presents the result and that it carries the
    // completed document. The completion-to-presentation time is reported here and bounded in the
    // performance lane (browser-responsiveness.perf.ts), because the presenting frame's GPU work
    // can retire behind other GPU work on a shared runner. A controlled negative case delays owner
    // event tasks only after its render is finished.
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
    // Last observation the poll made, reported on both the passing and the
    // failing path. Three different faults share this wait's failure message -
    // the sample never activated, the worker never returned a result, or a
    // result landed that no app frame carried - and only the raw observation
    // separates them.
    //
    // `workerBusy` answers the remaining question - whether a render is still
    // in flight or was never started - but it is read ONCE after the wait
    // ends, never inside the poll. A diagnostic that adds a round trip to
    // every iteration changes the timing of the wait it is measuring, which
    // is exactly the way an instrument stops observing its own subject.
    let lastPresentationState: {
      activeSample: Window["__donnerActiveSampleStats"];
      frames: number;
      worker: Window["__donnerWorkerStats"];
    } | undefined;
    // The exact qualifying capture must finish GPU execution. Its producer-clock
    // completion timestamp is converted to the page clock before publication.
    const presentationReady = async () => {
      const state = await page.evaluate(() => ({
        activeSample: window.__donnerActiveSampleStats,
        frames: window.__donnerMainLoopRenderedFrames || 0,
        worker: window.__donnerWorkerStats,
        documentPresentation: window.__donnerPresentedDocumentStats,
      }));
      lastPresentationState = state;
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
      // Match GPU-complete document telemetry; CPU frame acceptance can precede its retirement.
      const pixelsPresented = state.activeSample?.sampleId === sample.id
        && completedResults > beforeSample
        && state.frames > beforeFrames
        && state.worker?.resultFramePresented === true
        && state.documentPresentation?.documentGeneration === state.worker.documentGeneration
        && state.documentPresentation?.frameVersion === state.worker.frameVersion
        && Number.isFinite(state.worker?.resultFrameCompletedAtMs);
      if (pixelsPresented) {
        phaseTimings.presentedMs ??= state.worker!.resultFrameCompletedAtMs! - clickStartedAt;
      }
      return pixelsPresented;
    };
    try {
      await expect.poll(presentationReady, {
        message: `expected ${sample.name} to present a document frame in the editor canvas`,
        timeout: kStallGuardMs,
        intervals: [8, 16, 25, 50],
      }).toBe(true);
      firstUseCreationsAtPresentation = await readPipelinesCreatedAtFirstUse(page);
    } finally {
      // Best-effort, like the diagnostics below: a failed disarm must not replace the real failure.
      const injected = await carouselGpuDelay?.finish()
        .catch((error: unknown) => ({ disarmError: String(error) }));
      if (injected) console.log(`carousel-canvas-gpu-delay ${JSON.stringify(injected)}`);
      // Best-effort: a whole-test timeout tears the page down, and a rejection
      // here must not replace the real failure.
      const workerBusy = await page
        .evaluate(() => window.__donnerInteractionStats?.workerBusy)
        .catch(() => undefined);
      console.log(
        `carousel-frame-diagnostics sample=${sample.id} ${
          JSON.stringify(await readCarouselFrameDiagnostics(page))
        }`,
      );
      console.log(
        `carousel-gpu-timeline sample=${sample.id} clickStartedAt=${clickStartedAt} ${
          JSON.stringify(await readCarouselGpuTimeline(page))
        }`,
      );
      console.log(
        `carousel-presentation sample=${sample.id} timings=${JSON.stringify(phaseTimings)} worker=${
          JSON.stringify(completedWorkerStats)
        } last=${JSON.stringify(lastPresentationState)} workerBusy=${JSON.stringify(workerBusy)}${
          holdsScheduledFrames
            ? ` heldFrames=${JSON.stringify(await readHeldAnimationFrames(page))}`
            : ""
        }`,
      );
    }
    if (holdsScheduledFrames) {
      // The frame that presented the result must be an event frame. With the worker's animation
      // frames withheld, the only other frames are the driven ticks above, and every one of those
      // finished before the result existed; a tick carrying the result would hide a completion
      // wake that no longer requests a frame.
      expect(
        (completedWorkerStats as { presentingFrame?: string } | undefined)?.presentingFrame,
        `the completed result was presented by a ${
          (completedWorkerStats as { presentingFrame?: string } | undefined)?.presentingFrame
        } frame, not an event frame: ${JSON.stringify(phaseTimings)}`,
      ).toBe("event");
      // Confirm the hold really was in force throughout, then hand the withheld frames back and
      // check the scheduler resumes.
      const held = await readHeldAnimationFrames(page);
      expect(held, "the withheld animation frames stayed withheld until presentation").toEqual(
        expect.objectContaining({ released: false, ranAfterRelease: 0 }),
      );
      expect(held!.held).toBeGreaterThan(0);
      await releaseWorkerAnimationFrames(page);
      await expect.poll(async () => (await readHeldAnimationFrames(page))?.ranAfterRelease ?? 0, {
        message: "the scheduler resumes once its withheld animation frames are released",
      }).toBeGreaterThan(0);
    }
    if (sample.id === "geode-splash") {
      try {
        await expect.poll(async () => {
          const state = await page.evaluate(() => ({
            thumbnails: window.__donnerLayerThumbnailStats,
            interaction: window.__donnerInteractionStats,
            frames: window.__donnerMainLoopRenderedFrames,
          }));
          console.log(`geode-post-load ${JSON.stringify(state)}`);
          return Boolean(
            state.thumbnails && state.thumbnails.rowCount > 1
              && state.thumbnails.deferredCount === 0
              && state.thumbnails.textureSnapshotCount > 0,
          );
        }, {
          message: "Geode splash finishes its post-load layer previews without hanging",
          timeout: 10000,
          intervals: [100, 250, 500],
        }).toBe(true);
        for (let step = 0; step < 5; ++step) {
          const x = bounds.x + 400 + step * 10;
          const y = bounds.y + 200;
          await page.mouse.move(x, y);
          await expect.poll(() =>
            page.evaluate(() => ({
              x: window.__donnerInteractionStats?.pointerX,
              y: window.__donnerInteractionStats?.pointerY,
            })), {
            message: "Geode splash continues applying input after layer previews",
            timeout: 3000,
          }).toEqual({ x, y });
        }
      } finally {
        console.log(
          `geode-post-load-final ${JSON.stringify(await readCarouselFrameDiagnostics(page))}`,
        );
      }
    }
    expect(phaseTimings.presentedMs).toBeDefined();
    expect(phaseTimings.completedMs).toBeDefined();
    if (browserName === "chromium" || holdsScheduledFrames) {
      const handoff = completedResultHandoff(phaseTimings.completedMs, phaseTimings.presentedMs);
      const runtime = await page.evaluate(() => ({
        backend: window.__donnerBackend,
        viewport: { width: innerWidth, height: innerHeight },
        dpr: devicePixelRatio,
      }));
      const pipelinesCreatedAtFirstUse = {
        beforeClick: firstUseCreationsBeforeClick,
        atPresentation: firstUseCreationsAtPresentation,
      };
      const report = { workload: sample.id, heldWorkerRaf: holdsScheduledFrames, browser: browserName,
        ...runtime, ...packageHashes(), ...handoff, phaseTimings, presentationFault,
        pipelineReadiness, pipelinesCreatedAtFirstUse, pipelineCompileFault,
        pipelineCompileFaultAfterPresentation: compileFaultControl
          ? await readOwnerPipelineCompileFault(page)
          : undefined,
        worker: completedWorkerStats };
      const reportPath = testInfo.outputPath("nominal-completed-result-handoff.json");
      await writeFile(reportPath, JSON.stringify(report, null, 2));
      await testInfo.attach("nominal-completed-result-handoff", { path: reportPath,
        contentType: "application/json" });
      for (const value of Object.values(phaseTimings)) {
        expect(Number.isFinite(value)).toBe(true);
        expect(value).toBeGreaterThanOrEqual(0);
      }
      expect(Number.isFinite(handoff.handoffMs)).toBe(true);
      expect(handoff.handoffMs).toBeGreaterThanOrEqual(0);
      if (delayedControl) {
        expect(presentationFault!.requested).toBeGreaterThan(0);
        expect(presentationFault!.flushed).toBeGreaterThan(0);
        // The held event tasks carried the presentation: the presenting frame retired only after
        // they were released.
        expect(
          completedWorkerStats!.resultFrameCompletedAtMs!,
          `the result presented before the held event tasks were released: ${JSON.stringify(report)}`,
        ).toBeGreaterThan(presentationFault!.releaseRequestedAtMs);
      }
      // The pipelines this interaction draws with were built ahead of use, so it creates none at
      // first use, in order with its own GPU work. Checked where it is verified, in Chromium;
      // WebKit creates every pipeline at first use by design.
      if (browserName === "chromium") {
        expect(
          pipelinesCreatedAtFirstUse.beforeClick !== null
            && pipelinesCreatedAtFirstUse.atPresentation !== null,
          `the GPU owner reported its first-use pipeline count: ${JSON.stringify(report)}`,
        ).toBe(true);
        expect(
          pipelinesCreatedAtFirstUse.atPresentation,
          `pipelines created at first use during the interaction: ${JSON.stringify(report)}`,
        ).toBe(pipelinesCreatedAtFirstUse.beforeClick);
      }
    }
    expect(
      completedWorkerStats?.postPollFramesCompletedWithoutResult,
      `a post-poll admitted frame omitted the completed capture: ${JSON.stringify(completedWorkerStats)}`,
    ).toBe(0);
    expect(completedWorkerStats?.presentationBoundaryKnown).toBe(true);
    expect(completedWorkerStats?.framesPresentedWithoutResult).toBeGreaterThanOrEqual(
      completedWorkerStats?.postPollFramesCompletedWithoutResult ?? 0,
    );
    if (kBackend === "geode" && (completedWorkerStats?.readbackCount || 0) > 0) {
      expect(completedWorkerStats?.readbackWaitStrategy).toBe(kExpectedRasterWaitStrategy);
    }
    if (kBackend === "geode") {
      expect(await page.evaluate(() => window.__donnerHeadlessDeviceCreations)).toBe(
        deviceCreationsBeforeClick,
      );
    }
    expect(fatalMessages).toEqual([]);
  });
}

test(
  "catalog font loading adopts deferred WOFF2 without authored edits",
  async ({ page }, testInfo) => {
    const baseUrl = new URL(process.env.DONNER_WASM_BASE_URL || "http://127.0.0.1:8000");
    const manifestResponse = await page.request.get(new URL("catalog-fonts.json", baseUrl).href);
    expect(manifestResponse.ok()).toBe(true);
    const manifest = await manifestResponse.json() as {
      fonts: { family: string; sha256: string; path: string }[];
    };
    expect(manifest.fonts).toHaveLength(12);
    const inter = manifest.fonts.find((font) => font.family === "Inter");
    expect(inter).toBeDefined();
    if (!inter) throw new Error("The packaged catalog must contain Inter");
    expect(inter.sha256).toMatch(/^[a-f0-9]{64}$/);
    expect(inter.path).toBe(`fonts/${inter.sha256}.woff2`);
    const interUrl = new URL(inter.path, baseUrl).href;
    const catalogFontUrls = new Set(manifest.fonts.map((font) => new URL(font.path, baseUrl).href));

    await page.addInitScript(() => {
      const probe = {
        requests: [] as { url: string; afterFirstFrame: boolean }[],
        requestsAtFirstFrame: null as number | null,
      };
      window.__catalogFontTest = probe;
      window.addEventListener("donner:first-frame-presented", () => {
        probe.requestsAtFirstFrame = probe.requests.length;
      }, { once: true });
      const fetch = window.fetch.bind(window);
      window.fetch = (input, init) => {
        const url = new URL(input instanceof Request ? input.url : String(input), location.href);
        if (url.pathname.includes("/fonts/")) {
          probe.requests.push({
            url: url.href,
            afterFirstFrame: window.__donnerFirstFramePresented === true,
          });
        }
        return fetch(input, init);
      };
    });

    const networkRequests: string[] = [];
    page.on("request", (request) => {
      if (/\.(?:ttf|otf|woff2?)$/.test(new URL(request.url()).pathname)) {
        networkRequests.push(request.url());
      }
    });
    let releaseFont!: () => void;
    const fontGate = new Promise<void>((resolve) => {
      releaseFont = resolve;
    });
    await page.route("**/fonts/*.woff2", async (route) => {
      await fontGate;
      await route.continue();
    });

    try {
      const fatalMessages = await openEditor(page, { postInitializationDwellMs: 0 });
      expect(await page.evaluate(() => window.__catalogFontTest?.requestsAtFirstFrame)).toBe(0);
      // Background previews may request other catalog faces, while Inter must be requested once
      // for the visible Text and Style card before its document is opened.
      await expect.poll(() => networkRequests.filter((url) => url === interUrl), {
        timeout: scaledMs(2000),
      }).toEqual([interUrl]);
      const canvas = page.locator("canvas#canvas");
      const bounds = await canvas.boundingBox();
      if (!bounds) throw new Error("The presented editor canvas must have bounds");
      const priorDocumentGeneration = await page.evaluate(() =>
        window.__donnerWorkerStats?.documentGeneration ?? 0
      );
      await page.mouse.click(bounds.x + bounds.width * 0.24, bounds.y + 390);
      await expect(canvas).toHaveAttribute("data-active-sample-id", "text-style", {
        timeout: 1000,
      });
      await expect.poll(async () =>
        page.evaluate((priorGeneration) => ({
          newDocument: (window.__donnerWorkerStats?.documentGeneration ?? 0) > priorGeneration,
          revision: window.__donnerWorkerStats?.fontResourceRevision,
          accepted: window.__donnerWorkerStats?.acceptedForPresentation,
          presented: window.__donnerWorkerStats?.presentedAtMs !== undefined,
        }), priorDocumentGeneration), {
        message: "the sample must present fallback text while its WOFF2 response is held",
        timeout: scaledMs(2000),
        intervals: [16, 25, 50, 100],
      }).toEqual({ newDocument: true, revision: 0, accepted: true, presented: true });
      const region = {
        x: kSourcePaneWidth + 20,
        y: 80,
        width: 1600 - kSourcePaneWidth - kRightPaneWidth - 40,
        height: 680,
      };
      const fallbackGlyphs = await readTextStyleGlyphStats(page, region);
      expect(fallbackGlyphs.backgroundPixels).toBeGreaterThan(10_000);
      expect(fallbackGlyphs.glyphPixels).toBeGreaterThan(200);
      const fallback = await page.evaluate(() => window.__donnerWorkerStats!);
      expect(fallback.undoEntryCount).toBe(0);
      expect(networkRequests.every((url) => catalogFontUrls.has(url))).toBe(true);
      expect(networkRequests.filter((url) => url === interUrl)).toEqual([interUrl]);
      expect(
        (await page.evaluate(() => window.__catalogFontTest?.requests))?.filter(
          (request) => request.url === interUrl,
        ),
      ).toEqual([
        { url: interUrl, afterFirstFrame: true },
      ]);
      await testInfo.attach("fallback.png", {
        body: await canvas.screenshot(),
        contentType: "image/png",
      });

      const responsePromise = page.waitForResponse(interUrl);
      releaseFont();
      const response = await responsePromise;
      expect(response.status()).toBe(200);
      expect(response.headers()["content-type"]).toBe("font/woff2");
      await expect.poll(async () =>
        page.evaluate((previous) => {
          const current = window.__donnerWorkerStats;
          return current !== undefined && current.completedResults > previous.completedResults
            && current.fontResourceRevision > previous.fontResourceRevision
            && current.acceptedForPresentation && current.publishReason === "render-result"
            && current.presentedAtMs !== undefined;
        }, fallback), {
        message: "verified WOFF2 must reach an accepted and presented font-resource revision",
        timeout: scaledMs(2000),
        intervals: [16, 25, 50, 100],
      }).toBe(true);
      const adopted = await page.evaluate(() => window.__donnerWorkerStats!);
      expect(adopted.documentGeneration).toBe(fallback.documentGeneration);
      expect(adopted.frameVersion).toBeGreaterThan(fallback.frameVersion);
      expect(adopted.sourceVersion).toBe(fallback.sourceVersion);
      expect(adopted.undoEntryCount).toBe(fallback.undoEntryCount);
      const loadedGlyphs = await readTextStyleGlyphStats(page, region);
      expect(loadedGlyphs.backgroundPixels).toBeGreaterThan(10_000);
      expect(loadedGlyphs.glyphPixels).toBeGreaterThan(200);
      expect(networkRequests.every((url) => catalogFontUrls.has(url))).toBe(true);
      expect(networkRequests.filter((url) => url === interUrl)).toEqual([interUrl]);
      expect(
        (await page.evaluate(() => window.__catalogFontTest?.requests))?.filter(
          (request) => request.url === interUrl,
        ),
      ).toEqual([
        { url: interUrl, afterFirstFrame: true },
      ]);
      await testInfo.attach("loaded.png", {
        body: await canvas.screenshot(),
        contentType: "image/png",
      });
      await testInfo.attach("font-adoption.json", {
        body: JSON.stringify({ fallback, adopted, fallbackGlyphs, loadedGlyphs }, null, 2),
        contentType: "application/json",
      });
      expect(fatalMessages).toEqual([]);
    } finally {
      try {
        await captureCatalogFontDiagnostics(page, testInfo, networkRequests);
      } catch (error) {
        console.error(`catalog font diagnostic capture failed: ${String(error)}`);
      } finally {
        releaseFont();
      }
    }
  },
);

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
  await page.mouse.click(bounds.x + bounds.width * 0.76, bounds.y + 282);
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
  await waitForAppliedPointer(page, blueRectCenter, {
    message: "drag-budget press",
    timeoutMs: scaledMs(4000),
  });
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
  // Both ends of the measurement are page-clock timestamps: the move's
  // dispatch time is stamped on the page just before the move goes out, and
  // the drag frame's time is the frame sample the editor publishes when that
  // frame finishes. Timing the poll loop instead adds the poll's own
  // scheduling latency, which on a loaded shared runner exceeds the whole
  // budget without any product stall.
  const dragMoveStartedAtPageMs = await page.evaluate(() => performance.now());
  await page.mouse.move(blueRectCenter.x + 120, blueRectCenter.y + 70);
  let firstDragFrameMs = Number.POSITIVE_INFINITY;
  await expect
    .poll(async () => {
      const state = await page.evaluate(() => ({
        frames: window.__donnerMainLoopRenderedFrames || 0,
        lastFrameAtMs: window.__donnerFrameLoopStats?.lastFrameAtMs ?? 0,
      }));
      if (state.frames > beforeMoveFrames) {
        firstDragFrameMs = Math.min(
          firstDragFrameMs,
          state.lastFrameAtMs - dragMoveStartedAtPageMs,
        );
        return true;
      }
      return false;
    }, {
      message: "expected the shape's first visible drag frame while the pointer remained down",
      timeout: firstDragFrameBudgetMs,
      intervals: [8, 16, 25, 33, 50],
    })
    .toBe(true);
  console.log(`first-drag-frame-ms=${firstDragFrameMs.toFixed(1)}`);
  expect(firstDragFrameMs).toBeLessThan(firstDragFrameBudgetMs);
  await page.mouse.up();
  expect(fatalMessages).toEqual([]);
});

test(
  "production Geode wasm presents visible editor pixels after held canvas GPU completion",
  async ({ browserName, page }, testInfo) => {
    test.skip(browserName !== "chromium" || kBackend !== "geode", "controlled browser GPU gate");
    test.skip(
      kHostedMacosQuarantine,
      "Quarantined in the hosted macOS CI job: the GPU device request can miss its window (#1691)",
    );
    const fatalMessages = await openEditor(page, { postInitializationDwellMs: 0 });
    await expectSurfaceFrameProbeReady(page);
    const applicationOwner = await findProbedCanvasApplicationWorker(page);
    expect(applicationOwner, "the canvas owner must have the completion probe installed").not
      .toBeNull();
    let hold: Awaited<ReturnType<typeof holdCanvasCompletionForTest>> | null = null;
    let requiredOwner: Worker | null = null;
    let releaseSucceeded = false;
    const captureErrors: unknown[] = [];
    try {
      const canvas = page.locator("canvas#canvas");
      const bounds = await canvas.boundingBox();
      expect(bounds).not.toBeNull();
      if (bounds === null) throw new Error("the canvas bounds are unavailable");
      const before = await page.evaluate(() => ({
        results: window.__donnerWorkerStats?.completedResults ?? 0,
        frames: window.__donnerMainLoopRenderedFrames ?? 0,
      }));
      await page.mouse.click(bounds.x + bounds.width * 0.76, bounds.y + 282);
      const readState = () =>
        page.evaluate(() => ({
          sampleId: window.__donnerActiveSampleStats?.sampleId ?? null,
          completedResults: window.__donnerWorkerStats?.completedResults ?? 0,
          presentedAtMs: window.__donnerWorkerStats?.presentedAtMs ?? null,
          renderedFrames: window.__donnerMainLoopRenderedFrames ?? 0,
          hostFrames: window.__donnerHostFrameTiming?.frames ?? null,
          hostPresented: window.__donnerHostFrameTiming?.lastSurfacePresented === true,
          frameId: window.__donnerOverlayStats?.frameId ?? 0,
          captureId: window.__donnerOverlayStats?.captureId ?? 0,
          completedFrameId: window.__donnerPresentationQueueStats?.frameId ?? 0,
          completedCaptureId: window.__donnerPresentationQueueStats?.captureId ?? 0,
          completedInputRepresented:
            window.__donnerPresentationQueueStats?.inputRepresented === true,
          framesInFlight: window.__donnerPresentationQueueStats?.framesInFlight ?? null,
          completedSerial: window.__donnerPresentationQueueStats?.completedSerial ?? null,
          submittedSerial: window.__donnerPresentationQueueStats?.submittedSerial ?? null,
        }));
      await expect.poll(
        async () => hasPresentedBasicShapesHostFrame(await readState(), before.results),
        { timeout: scaledMs(5_000) },
      ).toBe(true);
      let readyState: Awaited<ReturnType<typeof readState>> | undefined;
      await expect.poll(async () => {
        const state = await readState();
        const ready = hasPresentedBasicShapesHostFrame(state, before.results)
          && state.framesInFlight === 0 && state.completedSerial === state.submittedSerial;
        if (ready) readyState = state;
        return ready;
      }, { timeout: scaledMs(5_000) }).toBe(true);
      if (!readyState) throw new Error("the completed Basic Shapes frame was not observed");
      expect(readyState.renderedFrames).toBeGreaterThan(before.frames);
      // Hold a fresh document frame, not welcome frames that can occupy all presentation slots.
      const completionHold = await holdCanvasCompletionForTest(page, [applicationOwner!]);
      hold = completionHold;
      await page.evaluate(() => {
        window.__donnerEditorFrameRequested = true;
      });
      await expect.poll(completionHold.observedCalls).toBeGreaterThan(0);

      let screenshots = 0;
      let settled = false;
      const gatedCapture = captureReadyBasicShapesFrame(
        page,
        readyState,
        before.results,
        performance.now() + scaledMs(5_000),
        (remainingMs) => {
          ++screenshots;
          return captureEditorPage(page, remainingMs);
        },
        (owner) => {
          requiredOwner = owner;
        },
      ).finally(() => {
        settled = true;
      });
      // Wait until this exact gate is entered, or a bypass starts the capture.
      // A page round trip alone can finish before worker-owner discovery.
      let lastEnteredWaits: number | null = null;
      try {
        await expect.poll(async () => {
          if (screenshots > 0) return true;
          lastEnteredWaits = await completionHold.enteredWaits();
          return lastEnteredWaits > 0;
        }, { timeout: scaledMs(5_000) }).toBe(true);
      } catch (error) {
        console.error(JSON.stringify({
          heldCanvasCapture: {
            readyStateValid: hasPresentedBasicShapesHostFrame(readyState, before.results),
            screenshots,
            settled,
            requiredOwnerIdentified: requiredOwner !== null,
            enteredWaits: lastEnteredWaits,
          },
        }));
        throw error;
      }
      expect(screenshots).toBe(0);
      expect(settled).toBe(false);
      expect(requiredOwner).not.toBeNull();
      if (requiredOwner === null) throw new Error("the held canvas owner was not identified");
      await completionHold.release(requiredOwner);
      releaseSucceeded = true;
      const capture = await gatedCapture;
      expect(capture).not.toBeNull();
      if (capture === null) throw new Error("the held canvas capture is unavailable");
      const scoredFramePath = testInfo.outputPath("held-canvas-scored-frame.png");
      await writeFile(scoredFramePath, capture.png);
      await testInfo.attach("held-canvas-scored-frame", {
        path: scoredFramePath,
        contentType: "image/png",
      });
      expect(capture.usable).toBe(true);
      expect(screenshots).toBe(1);
      const viewport = page.viewportSize();
      expect(viewport).not.toBeNull();
      if (viewport !== null) {
        expect(
          readEditorPixelBoundsFromPng(capture.png, "basic-blue", viewport, {
            minX: 0,
            minY: 0,
            maxX: viewport.width,
            maxY: viewport.height,
          })?.pixels ?? 0,
        ).toBeGreaterThan(500);
      }
      let staleCaptures = 0;
      const staleCapture = async () => {
        ++staleCaptures;
      };
      expect(
        await captureReadyBasicShapesFrame(
          page,
          { ...readyState, hostPresented: false, completedFrameId: 0 },
          before.results,
          performance.now() + 100,
          staleCapture,
        ),
      ).toBeNull();
      expect(
        await captureReadyBasicShapesFrame(
          page,
          { ...readyState, hostFrames: readyState.renderedFrames - 1 },
          before.results,
          performance.now() + 100,
          staleCapture,
        ),
      ).toBeNull();
      expect(staleCaptures).toBe(0);
    } catch (error) {
      captureErrors.push(error);
    } finally {
      if (!releaseSucceeded && hold !== null) {
        try {
          await hold.release(requiredOwner ?? applicationOwner!);
        } catch (cleanupError) {
          captureErrors.push(cleanupError);
        }
      }
    }
    if (captureErrors.length === 1) throw captureErrors[0];
    if (captureErrors.length > 1) {
      throw new AggregateError(captureErrors, "capture and hold cleanup failed");
    }
    // A parked secondary worker must not turn a successful owner release into
    // a failure, while an unresponsive required owner must still fail closed.
    let ownerEvaluations = 0;
    let parkedEvaluations = 0;
    const syntheticOwner = {
      evaluate: async () => {
        ++ownerEvaluations;
        return true;
      },
    } as unknown as Worker;
    const parkedSecondary = {
      evaluate: async () => {
        ++parkedEvaluations;
        return parkedEvaluations === 1 ? true : new Promise<never>(() => {});
      },
    } as unknown as Worker;
    const syntheticHold = await holdCanvasCompletionForTest(page, [
      syntheticOwner,
      parkedSecondary,
    ]);
    await syntheticHold.release(syntheticOwner);
    await syntheticHold.release(syntheticOwner);
    expect(ownerEvaluations).toBe(3);
    expect(parkedEvaluations).toBe(1);
    let requiredEvaluations = 0;
    const unavailableOwner = {
      evaluate: async () => {
        ++requiredEvaluations;
        if (requiredEvaluations === 1) return true;
        throw new Error("required owner unavailable");
      },
    } as unknown as Worker;
    const requiredHold = await holdCanvasCompletionForTest(page, [unavailableOwner]);
    await expect(requiredHold.release(unavailableOwner))
      .rejects.toThrow("could not release the required canvas completion owner");
    let forbiddenCaptures = 0;
    const noCapture = async () => {
      ++forbiddenCaptures;
    };
    await expect(captureAfterCanvasGpuCompletion(
      { evaluate: () => Promise.reject(new Error("rejected completion")) } as unknown as Worker,
      performance.now() + 100,
      noCapture,
    )).rejects.toThrow("rejected completion");
    await expect(captureAfterCanvasGpuCompletion(
      { evaluate: () => new Promise<never>(() => {}) } as unknown as Worker,
      performance.now() + 10,
      noCapture,
    )).rejects.toThrow("exceeded the blue-pixel deadline");
    expect(forbiddenCaptures).toBe(0);
    let boundedShots = 0;
    let forwardedTimeout: number | undefined;
    const timedOutPage = {
      screenshot: async (options?: { timeout?: number }) => {
        ++boundedShots;
        forwardedTimeout = options?.timeout;
        throw new errors.TimeoutError("Timeout exceeded while taking page screenshot");
      },
    } as unknown as Page;
    const boundedCapture = await captureEditorPage(timedOutPage, 7);
    expect(boundedCapture.usable).toBe(false);
    expect(boundedShots).toBe(1);
    expect(forwardedTimeout).toBe(7);
    let lateCaptures = 0;
    await expect(captureAfterCanvasGpuCompletion(
      { evaluate: () => Promise.resolve(1) } as unknown as Worker,
      performance.now() + 200,
      async () => {
        ++lateCaptures;
        await new Promise<void>((resolve) => setTimeout(resolve, 250));
        return "late";
      },
    )).rejects.toThrow("exceeded the blue-pixel deadline");
    expect(lateCaptures).toBe(1);
    expect(fatalMessages).toEqual([]);
  },
);

test("Firefox keeps Basic Shapes resize pixels and outline synchronized", async ({ browserName, page }) => {
  test.skip(browserName !== "firefox" || kBackend !== "geode", "Firefox Geode regression");
  test.skip(
    browserName === "firefox" && quarantineStillSkips(),
    "Quarantined: Firefox can capture a blank editor page (#1634)",
  );
  armFailureCanvasEvidence(page, test.info());
  const fatalMessages = await openEditor(page, { postInitializationDwellMs: 0 });
  await expectSurfaceFrameProbeReady(page);
  const canvas = page.locator("canvas#canvas");
  const bounds = await canvas.boundingBox();
  expect(bounds).not.toBeNull();
  if (bounds === null) {
    return;
  }

  // Resize synchronization is the subject here, so the Basic Shapes render's deadline below must
  // not absorb the picker's first-use thumbnail work.
  await expectSampleThumbnailsToSettle(page, {
    message: "the sample thumbnail lane must drain before Basic Shapes replaces it",
    timeout: scaledMs(20_000),
  });
  const beforeSample = await page.evaluate(
    () => window.__donnerWorkerStats?.completedResults || 0,
  );
  await page.mouse.click(bounds.x + bounds.width * 0.76, bounds.y + 282);
  await expect(canvas).toHaveAttribute("data-active-sample-id", "basic-shapes", { timeout: 1000 });
  await expect
    .poll(async () => page.evaluate(() => window.__donnerWorkerStats?.completedResults || 0), {
      message: "expected Basic Shapes to finish presenting before resize",
      timeout: scaledMs(1000),
      intervals: [16, 25, 50, 100],
    })
    .toBeGreaterThan(beforeSample);

  const editorBounds = await canvas.boundingBox();
  expect(editorBounds).not.toBeNull();
  if (editorBounds === null) {
    return;
  }

  // Gecko can return an empty image for a clipped transferred-canvas screenshot,
  // even when that clip covers the whole canvas. Capture the page without a clip
  // and inspect only the published artboard.
  const captureViewport = page.viewportSize();
  if (captureViewport === null) {
    throw new Error("the Firefox viewport is unavailable for the resize pixel probe");
  }
  type ProbeRegion = { x: number; y: number; width: number; height: number };
  let probeRegion: ProbeRegion | null = null;
  let blueCss: PixelBounds | null = null;
  let screenshotTimeouts = 0;
  const blueDeadlineAtMs = performance.now() + scaledMs(5_000);
  let lastBlueProbe:
    | {
      shot: Buffer;
      state: object;
      region: ProbeRegion | null;
      bluePixels: number;
      captureUsable: boolean;
      captureAttempts: number;
      timedOutCaptures: number;
      shotFromPriorAttempt: boolean;
      timeoutStage: ScreenshotTimeoutStage | null;
    }
    | null = null;
  try {
    await expect.poll(async () => {
      const state = await page.evaluate(() => ({
        sampleId: window.__donnerActiveSampleStats?.sampleId ?? null,
        completedResults: window.__donnerWorkerStats?.completedResults ?? 0,
        presentedAtMs: window.__donnerWorkerStats?.presentedAtMs ?? null,
        renderedFrames: window.__donnerMainLoopRenderedFrames ?? 0,
        hostFrames: window.__donnerHostFrameTiming?.frames ?? null,
        hostPresented: window.__donnerHostFrameTiming?.lastSurfacePresented === true,
        frameId: window.__donnerOverlayStats?.frameId ?? 0,
        captureId: window.__donnerOverlayStats?.captureId ?? 0,
        completedFrameId: window.__donnerPresentationQueueStats?.frameId ?? 0,
        completedCaptureId: window.__donnerPresentationQueueStats?.captureId ?? 0,
        completedInputRepresented: window.__donnerPresentationQueueStats?.inputRepresented === true,
        viewport: window.__donnerViewportStats ?? null,
      }));
      const viewport = state.viewport;
      const region = viewport === null ? null : {
        x: Math.max(viewport.documentX, viewport.paneX),
        y: Math.max(viewport.documentY, viewport.paneY),
        width: Math.min(
          viewport.documentX + viewport.documentWidth,
          viewport.paneX + viewport.paneWidth,
        ) - Math.max(viewport.documentX, viewport.paneX),
        height: Math.min(
          viewport.documentY + viewport.documentHeight,
          viewport.paneY + viewport.paneHeight,
        ) - Math.max(viewport.documentY, viewport.paneY),
      };
      if (region === null || region.width <= 0 || region.height <= 0) return 0;
      const capture = await captureReadyBasicShapesFrame(
        page,
        state,
        beforeSample,
        blueDeadlineAtMs,
        (remainingMs) => captureEditorPage(page, remainingMs),
      );
      if (capture === null) return 0;
      screenshotTimeouts += capture.timedOutCaptures;
      const blue = capture.usable && region !== null && region.width > 0 && region.height > 0
        ? readEditorPixelBoundsFromPng(capture.png, "basic-blue", captureViewport, {
          minX: region.x,
          minY: region.y,
          maxX: region.x + region.width,
          maxY: region.y + region.height,
        })
        : null;
      lastBlueProbe = {
        shot: capture.png.length > 0 ? capture.png : lastBlueProbe?.shot ?? Buffer.alloc(0),
        state,
        region,
        bluePixels: blue?.pixels ?? 0,
        captureUsable: capture.usable,
        captureAttempts: capture.attempts,
        timedOutCaptures: screenshotTimeouts,
        shotFromPriorAttempt: capture.png.length === 0 && (lastBlueProbe?.shot.length ?? 0) > 0,
        timeoutStage: capture.timeoutStage,
      };
      if (
        region !== null && blue !== null && state.sampleId === "basic-shapes"
        && state.completedResults > beforeSample && state.presentedAtMs !== null
        && blue.pixels > 500
      ) {
        probeRegion = region;
        blueCss = {
          minX: blue.minX - region.x,
          minY: blue.minY - region.y,
          maxX: blue.maxX - region.x,
          maxY: blue.maxY - region.y,
          pixels: blue.pixels,
        };
        if (performance.now() >= blueDeadlineAtMs) {
          throw new Error("Basic Shapes blue-pixel acceptance exceeded the deadline");
        }
        return blue.pixels;
      }
      return 0;
    }, {
      message: "the resize target must be visibly blue in the artboard capture",
      timeout: Math.max(1, blueDeadlineAtMs - performance.now()),
      intervals: [250, 400, 600],
    }).toBeGreaterThan(500);
  } catch (error) {
    if (lastBlueProbe !== null) {
      const pngPath = test.info().outputPath("basic-shapes-blue-probe.png");
      const statePath = test.info().outputPath("basic-shapes-blue-probe.json");
      if (lastBlueProbe.shot.length > 0) {
        await writeFile(pngPath, lastBlueProbe.shot);
      }
      await writeFile(
        statePath,
        JSON.stringify(
          {
            state: lastBlueProbe.state,
            beforeSample,
            region: lastBlueProbe.region,
            bluePixels: lastBlueProbe.bluePixels,
            captureUsable: lastBlueProbe.captureUsable,
            captureAttempts: lastBlueProbe.captureAttempts,
            timedOutCaptures: lastBlueProbe.timedOutCaptures,
            shotFromPriorAttempt: lastBlueProbe.shotFromPriorAttempt,
            timeoutStage: lastBlueProbe.timeoutStage,
            captureViewport,
          },
          null,
          2,
        ),
      );
      if (lastBlueProbe.shot.length > 0) {
        await test.info().attach("basic-shapes-blue-probe.png", {
          path: pngPath,
          contentType: "image/png",
        });
      }
      await test.info().attach("basic-shapes-blue-probe.json", {
        path: statePath,
        contentType: "application/json",
      });
    }
    throw error;
  }
  if (probeRegion === null || blueCss === null) {
    throw new Error("resize target disappeared after a verified blue artboard capture");
  }
  const artboardInCapture = {
    minX: probeRegion.x,
    minY: probeRegion.y,
    maxX: probeRegion.x + probeRegion.width,
    maxY: probeRegion.y + probeRegion.height,
  };
  const toArtboard = (pixels: PixelBounds | null): PixelBounds | null =>
    pixels === null ? null : {
      minX: pixels.minX - artboardInCapture.minX,
      minY: pixels.minY - artboardInCapture.minY,
      maxX: pixels.maxX - artboardInCapture.minX,
      maxY: pixels.maxY - artboardInCapture.minY,
      pixels: pixels.pixels,
    };
  const readResizePixels = async () => {
    const shot = await page.screenshot();
    const blue = readEditorPixelBoundsFromPng(
      shot,
      "basic-blue",
      captureViewport,
      artboardInCapture,
    );
    const margin = 32;
    const tealSearch = blue === null ? artboardInCapture : {
      minX: Math.max(artboardInCapture.minX, blue.minX - margin),
      minY: Math.max(artboardInCapture.minY, blue.minY - margin),
      maxX: Math.min(artboardInCapture.maxX, blue.maxX + margin),
      maxY: Math.min(artboardInCapture.maxY, blue.maxY + margin),
    };
    const teal = readEditorPixelBoundsFromPng(
      shot,
      "selection-teal",
      captureViewport,
      tealSearch,
    );
    return { blue: toArtboard(blue), teal: toArtboard(teal) };
  };
  const blueRectCenter = {
    x: probeRegion.x + (blueCss.minX + blueCss.maxX) / 2,
    y: probeRegion.y + (blueCss.minY + blueCss.maxY) / 2,
  };
  const resizeHandle = {
    x: probeRegion.x + blueCss.maxX + 1,
    y: probeRegion.y + blueCss.maxY + 1,
  };
  await page.mouse.click(blueRectCenter.x, blueRectCenter.y);
  // Gecko can also hand back one empty capture after selection. Require blue and teal from the
  // same verified image before using its bounds as the resize baseline.
  let initialPixels = await readResizePixels();
  await expect.poll(async () => {
    initialPixels = await readResizePixels();
    return (initialPixels.blue?.pixels ?? 0) > 500 && (initialPixels.teal?.pixels ?? 0) > 50;
  }, {
    message: "the selected resize target and outline must share a visible artboard capture",
    timeout: scaledMs(5_000),
    intervals: [250, 400, 600],
  }).toBe(true);
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
  await waitForAppliedPointer(page, resizeHandle, {
    message: "resize-handle press",
    timeoutMs: scaledMs(4000),
  });
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
      const { blue, teal } = await readResizePixels();
      if (blue === null || teal === null) {
        return false;
      }
      const reachedFinalPointer = blue.maxX >= initialPixels.blue.maxX + 100
        && blue.maxY >= initialPixels.blue.maxY + 60;
      const horizontalGap = Math.abs(teal.maxX - blue.maxX);
      const verticalGap = Math.abs(teal.maxY - blue.maxY);
      return reachedFinalPointer && horizontalGap <= 8 && verticalGap <= 8;
    }, {
      message: "expected the final resized pixels and selection outline from one document render",
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
  await page.mouse.click(bounds.x + bounds.width * 0.76, bounds.y + 282);
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
  const selectionAccounting = await pollRenderAccounting(
    page,
    scaledMs(2000),
    (accounting) => accounting.completedResults === beforeSelection + 1,
  );
  expect(
    selectionAccounting.completedResults,
    `expected the selection click's single prewarm render and nothing further: `
      + `beforeSelection=${beforeSelection} `
      + `before=${JSON.stringify(beforeSelectionAccounting)} `
      + `after=${JSON.stringify(selectionAccounting)}`,
  ).toBe(beforeSelection + 1);
  const captureDragPixels = async (name: string) => {
    const viewport = await page.evaluate(() => window.__donnerViewportStats);
    expect(viewport).toBeDefined();
    const size = page.viewportSize();
    expect(size).not.toBeNull();
    if (viewport === undefined || size === null) throw new Error("missing drag pixel viewport");
    const image = await page.screenshot();
    const path = test.info().outputPath(`${name}.png`);
    await writeFile(path, image);
    await test.info().attach(name, { path, contentType: "image/png" });
    const clip = { x: 0, y: 0, width: size.width, height: size.height };
    const artboard = {
      minX: Math.max(viewport.documentX, viewport.paneX),
      minY: Math.max(viewport.documentY, viewport.paneY),
      maxX: Math.min(
        viewport.documentX + viewport.documentWidth,
        viewport.paneX + viewport.paneWidth,
      ),
      maxY: Math.min(
        viewport.documentY + viewport.documentHeight,
        viewport.paneY + viewport.paneHeight,
      ),
    };
    const blue = readEditorPixelBoundsFromPng(image, "basic-blue", clip, artboard);
    expect(blue?.pixels ?? 0).toBeGreaterThan(500);
    if (blue === null) throw new Error("missing drag artwork pixels");
    const teal = readEditorPixelBoundsFromPng(image, "selection-teal", clip, {
      minX: blue.minX - 16,
      minY: blue.minY - 16,
      maxX: blue.maxX + 16,
      maxY: blue.maxY + 16,
    });
    expect(teal?.pixels ?? 0).toBeGreaterThan(50);
    if (teal === null) throw new Error("missing selection pixels");
    return { blue, teal };
  };
  const startPixels = await captureDragPixels("retained-drag-start");
  await page.mouse.move(dragStart.x, dragStart.y);
  await page.mouse.down();
  await expect
    .poll(async () => page.evaluate(() => window.__donnerInteractionStats?.dragging ?? false), {
      message: "expected the app to consume mouse-down before moving the drag pointer",
      timeout: scaledMs(1000),
      intervals: [16, 25, 50, 100],
    })
    .toBe(true);
  // An active drag presents by transforming the prewarmed selected-layer texture inside UI
  // frames; the worker does not re-rasterize until the pointer releases. The live signal for
  // "the drag is visibly presenting" is a changed drag preview, and the document counter staying
  // flat through the drag is the architecture's own contract.
  const beforeDragFrames = await page.evaluate(
    () => window.__donnerMainLoopRenderedFrames || 0,
  );
  await page.mouse.move(dragStart.x + 18, dragStart.y + 12);
  await page.mouse.move(dragStart.x + 32, dragStart.y + 20);
  await expect
    .poll(
      async () => page.evaluate(() => window.__donnerInteractionStats?.moved ?? false),
      {
        message: "expected pointer movement to produce a nonzero drag preview",
        timeout: scaledMs(1000),
        intervals: [16, 25, 50, 100],
      },
    )
    .toBe(true);
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
  // Separate the two ways this can go wrong. A release only owes a document
  // commit when a drag was live to release: if the buffered press never became
  // a drag (the press is buffered and dispatched on a later frame, so a slow
  // runner can dispatch it after the gesture is over), the settlement assertion
  // below would report a missing settle for a press that never moved anything.
  // Name that case here instead.
  const beforeMouseUpAccounting = await pollRenderAccounting(
    page,
    scaledMs(1000),
    (accounting) =>
      accounting.interaction?.dragging === true
      && accounting.interaction.moved === true,
  );
  const beforeMouseUpResults = beforeMouseUpAccounting.completedResults;
  expect(
    beforeMouseUpAccounting.interaction?.dragging ?? false,
    `expected the shape drag to still be live at mouse-up: `
      + `${JSON.stringify(beforeMouseUpAccounting)}`,
  ).toBe(true);
  expect(
    beforeMouseUpAccounting.interaction?.moved ?? false,
    `expected the live shape drag to have a nonzero preview at mouse-up: `
      + `${JSON.stringify(beforeMouseUpAccounting)}`,
  ).toBe(true);
  await page.mouse.up();
  // The release owes exactly one document commit. Carry the same render
  // accounting the selection assertion above attaches, so a settlement that
  // never lands names what the editor was doing instead of reporting a count.
  const settlementAccounting = await pollRenderAccounting(
    page,
    scaledMs(1000),
    (accounting) => accounting.completedResults > beforeMouseUpResults,
  );
  expect(
    settlementAccounting.completedResults,
    `expected the drag-release settlement frame: `
      + `beforeMouseUp=${JSON.stringify(beforeMouseUpAccounting)} `
      + `after=${JSON.stringify(settlementAccounting)}`,
  ).toBeGreaterThan(beforeMouseUpResults);

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
  const settledPixels = await captureDragPixels("retained-drag-settled");
  const centerX = (pixels: PixelBounds) => (pixels.minX + pixels.maxX) / 2;
  const centerY = (pixels: PixelBounds) => (pixels.minY + pixels.maxY) / 2;
  expect(Math.abs(centerX(settledPixels.blue) - centerX(startPixels.blue) - 32))
    .toBeLessThanOrEqual(3);
  expect(Math.abs(centerY(settledPixels.blue) - centerY(startPixels.blue) - 20))
    .toBeLessThanOrEqual(3);
  expect(Math.abs(centerX(settledPixels.blue) - centerX(settledPixels.teal))).toBeLessThanOrEqual(
    3,
  );
  expect(Math.abs(centerY(settledPixels.blue) - centerY(settledPixels.teal))).toBeLessThanOrEqual(
    3,
  );
  const workerStats = await page.evaluate(() => window.__donnerWorkerStats);
  expect(workerStats).toBeDefined();
  console.log(`wasm-worker-stats=${JSON.stringify(workerStats)}`);
  // The settled scene reuses unchanged retained spans and published payload identities.
  expect(workerStats?.immediateTileCount).toBe(0);
  expect(workerStats?.cachedTileCount).toBe(0);
  expect(workerStats?.compositorReadbackCount).toBe(0);
  expect(workerStats?.tileHandoffReadbackCount).toBe(0);
  expect(workerStats?.readbackCount).toBe(0);
  expect(workerStats?.immediateRasterizeMs).toBe(0);
  expect(workerStats?.cachedRasterizeMs).toBe(0);
  expect(["timed-wait-any", "device-poll"]).toContain(workerStats?.readbackWaitStrategy);
  // A run that presented frames must say so explicitly rather than by the
  // absence of a failure field. A hung GPU wait publishes the same object with
  // `deviceLost` set and the wait named, so a rare stall prints a line that
  // identifies itself instead of leaving the stats undefined.
  expect(
    workerStats?.deviceLost,
    `worker device was declared lost: ${JSON.stringify(workerStats)}`,
  ).toBe(false);
  expect(workerStats?.gpuWaitTimeoutSite).toBe("none");
  expect(workerStats?.gpuWaitTimeoutMs).toBe(0);
  expect(workerStats?.publishReason).toBe("render-result");
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

  await page.mouse.click(canvasBox.x + canvasBox.width * 0.76, canvasBox.y + 282);
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

test("WGPU diagnostic completion distinguishes failure from old successful pixels", async ({ page }) => {
  await page.setContent("<canvas></canvas>");
  await page.evaluate(() => {
    Object.assign(window, {
      __donnerWgpuReadbackRequested: 2,
      __donnerWgpuReadbackCompleted: 2,
      __donnerWgpuReadbackStats: { request: 1 },
      __donnerWgpuReadbackLastFailedRequest: 2,
      __donnerWgpuReadbackCaptureFailures: 1,
    });
  });
  const failed = await readWgpuDiagnosticOutcome(page);
  expect(failed.completed).toBe(2);
  expect(failed.successfulRequest).toBe(1);
  expect(hasSuccessfulDiagnosticRequest(failed, 2)).toBe(false);
  await page.evaluate(() => {
    Object.assign(window, {
      __donnerWgpuReadbackRequested: 3,
      __donnerWgpuReadbackCompleted: 3,
      __donnerWgpuReadbackStats: { request: 3 },
    });
  });
  const recovered = await readWgpuDiagnosticOutcome(page);
  expect(hasSuccessfulDiagnosticRequest(recovered, 3)).toBe(true);
  expect(hasSuccessfulDiagnosticRequest(recovered, 4)).toBe(false);
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
  await page.mouse.click(bounds.x + bounds.width * 0.76, bounds.y + 282);
  await expect(canvas).toHaveAttribute("data-active-sample-id", "basic-shapes");
  await expect
    .poll(async () => page.evaluate(() => window.__donnerWorkerStats?.completedResults || 0), {
      message: "expected Basic Shapes to present before sampling the layer panel",
      timeout: 20000,
    })
    .toBeGreaterThan(beforeSample);
  if (kBackend === "geode") {
    const request = await requestWgpuDiagnostic(page);
    await waitForSuccessfulWgpuDiagnostic(page, request);
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

  // The 40 px Edit/Preview dock sits above the Layers panel. Keep the probes inside
  // the same named thumbnail rows after that dock shifts the panel down.
  const animationModeDockHeight = 40;
  const thumbnailRegions = [
    {
      name: "Sunburst",
      x: bounds.width - kRightPaneWidth + 60,
      y: 112 + animationModeDockHeight,
      width: 30,
      height: 26,
    },
    {
      name: "Blue_center_burst",
      x: bounds.width - kRightPaneWidth + 60,
      y: 172 + animationModeDockHeight,
      width: 30,
      height: 26,
    },
  ];
  // The published stats above prove the thumbnails rendered, not that the
  // frame carrying them has reached the browser composite; sample the pixels
  // to convergence instead of once.
  for (const region of thumbnailRegions) {
    await expect
      .poll(async () => (await readCanvasColorStats(page, region)).coloredPixels, {
        message: `${region.name} must show its rendered SVG thumbnail`,
        timeout: scaledMs(2000),
        intervals: [16, 25, 50, 100],
      })
      .toBeGreaterThan(20);
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
  await page.mouse.click(bounds.x + bounds.width * 0.76, bounds.y + 282);
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
  const beforeSelectionState = await page.evaluate(() => ({
    readback: window.__donnerWgpuReadbackStats,
    interaction: window.__donnerInteractionStats,
    worker: window.__donnerWorkerStats,
  }));
  const beforePath = test.info().outputPath("selection-overlay-before.json");
  await writeFile(beforePath, JSON.stringify({ sampleRequest, ...beforeSelectionState }, null, 2));
  await test.info().attach("selection-overlay-before", {
    path: beforePath,
    contentType: "application/json",
  });
  expect(beforeSelectionState.interaction?.selectedCount, "expected an unselected baseline").toBe(
    0,
  );
  const beforeSelectionChrome = beforeSelectionState.readback?.selectionChromePixels || 0;
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
    let selectionProbeRequest = selectionRequest;
    await expect
      .poll(
        async () => {
          const snapshot = await page.evaluate(() => ({
            request: window.__donnerWgpuReadbackStats?.request || 0,
            chrome: window.__donnerWgpuReadbackStats?.selectionChromePixels || 0,
          }));
          const delta = snapshot.chrome - beforeSelectionChrome;
          // A capture can precede the first frame that draws the selection chrome.
          // Refresh only after the previous request produced its diagnostic pixels.
          if (delta <= 50 && snapshot.request >= selectionProbeRequest) {
            selectionProbeRequest = await requestWgpuDiagnostic(page);
          }
          return delta;
        },
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
  } finally {
    const afterState = await page.evaluate(() => ({
      readback: window.__donnerWgpuReadbackStats,
      interaction: window.__donnerInteractionStats,
      worker: window.__donnerWorkerStats,
    }));
    const afterPath = test.info().outputPath("selection-overlay-after.json");
    await writeFile(afterPath, JSON.stringify({ selectionRequest, ...afterState }, null, 2));
    await test.info().attach("selection-overlay-after", {
      path: afterPath,
      contentType: "application/json",
    });
  }

  expect(fatalMessages).toEqual([]);
});

async function readChromiumGpuInfo(page: Page) {
  const browser = page.context().browser();
  if (!browser || browser.browserType().name() !== "chromium") return null;
  const creation = browser.newBrowserCDPSession();
  const session = await boundBrowserDiagnostic(creation, 2000);
  if (!session) {
    void creation.then((late) => boundBrowserDiagnostic(late.detach(), 2000), () => null);
    return null;
  }
  try {
    const info = await boundBrowserDiagnostic(session.send("SystemInfo.getInfo"), 2000);
    if (!info?.gpu) return null;
    const gpu = info.gpu;
    const bounded = (value: unknown) =>
      typeof value === "string"
        ? value.slice(0, 256)
        : typeof value === "number" && Number.isFinite(value)
        ? value
        : typeof value === "boolean"
        ? value
        : null;
    return {
      devices: (gpu.devices ?? []).slice(0, 8).map((device: Record<string, unknown>) =>
        Object.fromEntries(
          ["vendorId", "deviceId", "vendorString", "deviceString", "driverVendor", "driverVersion"]
            .map((key) => [key, bounded(device[key])]),
        )
      ),
      gl: Object.fromEntries(
        ["glVendor", "glRenderer", "glVersion", "displayType"]
          .map((key) => [key, bounded(gpu.auxAttributes?.[key])]),
      ),
      features: Object.fromEntries(
        ["gpu_compositing", "rasterization", "webgl", "webgl2", "webgpu"]
          .map((key) => [key, bounded(gpu.featureStatus?.[key])]),
      ),
    };
  } finally {
    await boundBrowserDiagnostic(session.detach(), 2000);
  }
}

async function measurePageRaf(page: Page, durationMs: number) {
  const loadBefore = loadavg();
  const sample = await boundBrowserDiagnostic(
    page.evaluate(async (durationMs) => {
      const startedAtMs = performance.now();
      const readState = () =>
        structuredClone({
          visibilityState: document.visibilityState,
          hasFocus: document.hasFocus(),
          frames: window.__donnerMainLoopRenderedFrames ?? 0,
          worker: window.__donnerWorkerStats ?? null,
          layerThumbnails: window.__donnerLayerThumbnailStats ?? null,
          thumbnails: window.__donnerSampleThumbnailStats ?? null,
        });
      const before = readState();
      return await new Promise<{
        startedAtMs: number;
        finishedAtMs: number;
        before: ReturnType<typeof readState>;
        after: ReturnType<typeof readState>;
        samples: number[][];
      }>((resolve) => {
        const samples: number[][] = [];
        let frame = 0;
        let timer = 0;
        let finished = false;
        const finish = () => {
          if (finished) return;
          finished = true;
          cancelAnimationFrame(frame);
          clearTimeout(timer);
          resolve({
            startedAtMs,
            finishedAtMs: performance.now(),
            before,
            after: readState(),
            samples,
          });
        };
        const tick = (timestamp: number) => {
          if (finished) return;
          samples.push([timestamp, performance.now()]);
          if (performance.now() - startedAtMs >= durationMs || samples.length >= 4096) {
            finish();
          } else {
            frame = requestAnimationFrame(tick);
          }
        };
        timer = window.setTimeout(finish, durationMs + 1000);
        frame = requestAnimationFrame(tick);
      });
    }, durationMs),
    durationMs + 2000,
  );
  return { sample, loadBefore, loadAfter: loadavg() };
}

type AnimationRegionCapture = {
  png: Buffer;
  read: (
    target: EditorPixelTarget,
    search?: { minX: number; minY: number; maxX: number; maxY: number },
  ) => PixelBounds | null;
  viewport: { width: number; height: number };
};

type AnimationRegionCaptureFn = (
  region: CssRegion,
  deadlineAtMs?: number,
) => Promise<AnimationRegionCapture>;

/**
 * Upper bound on screenshot passes under one completion hold. While the hold is in place the app's
 * held frames never retire, so at most three new frames are admitted and every rejected pass saw
 * at least one of them; a few frames submitted before the hold can also still retire and free
 * slots, so seven is a conservative bound.
 */
const kMaxCanvasQuiescencePasses = 7;

/**
 * Longest a capture holds the app's canvas completions at once, from arming the hold to the end
 * of the screenshot. The app declares its presentation device lost once its oldest frame has
 * waited 5 s without observed progress (`PresentationSubmissionQueue::kCompletionDeadline`), so a
 * hold must end well before that. A capture whose GPU work or screenshot would take longer
 * releases the hold, lets the held frames retire and holds again. This bounds the hold only; it is
 * not a deadline for the capture.
 */
const kMaxCanvasCompletionHoldMs = 3500;

/** Part of the hold the settle wait may use, leaving the rest for the screenshot. */
const kMaxCanvasSettleHoldMs = 2000;

/** Shortest screenshot timeout worth starting under a hold; less than this re-holds instead. */
const kMinHeldScreenshotMs = 250;

/**
 * Settle \p owner's canvas writes, or return null once \p untilMs passes first. A settle failure
 * still propagates.
 */
async function settleCanvasWritesUntil(
  owner: Worker,
  untilMs: number,
): Promise<CanvasSettleResult | null> {
  const settle = settleCanvasWrites(owner);
  let timer: ReturnType<typeof setTimeout> | undefined;
  const expired = new Promise<null>((resolve) => {
    timer = setTimeout(() => resolve(null), Math.max(0, untilMs - performance.now()));
  });
  try {
    return await Promise.race([settle, expired]);
  } finally {
    clearTimeout(timer);
    // A settle abandoned at the bound may still reject later, for example when the page closes.
    settle.catch(() => {});
  }
}

/**
 * Page captures of the animation sample, each of a canvas frame whose GPU work has finished.
 *
 * A Firefox page capture can show a canvas frame whose submitted GPU work has not run yet: only
 * the frame's clear, or nothing at all. Waiting for submitted work before the screenshot is not
 * enough while the animation keeps presenting, because a newer frame can be presented during the
 * screenshot. Each capture therefore holds the app's canvas completions, so it stops admitting new
 * frames after at most three, waits for the GPU to finish every canvas write, and accepts the
 * screenshot only when no canvas write was submitted from that wait until the screenshot ended.
 */
function createAnimationRegionCapture(
  page: Page,
  owner: Worker,
  browserName: string,
  testInfo: TestInfo,
): AnimationRegionCaptureFn {
  let flatCaptureSequence = 0;
  const capture = async (
    region: CssRegion,
    deadlineAtMs?: number,
  ): Promise<AnimationRegionCapture> => {
    const viewport = page.viewportSize();
    if (!viewport) throw new Error("the animation capture viewport is unavailable");
    // Firefox can omit the transferred WebGPU canvas from clipped captures. Use the same
    // un-clipped viewport capture as the presentation regressions, then score the original region.
    const unclipped = browserName === "firefox";
    const screenshot = (timeout: number) =>
      page.screenshot({
        ...(unclipped ? {} : { clip: region }),
        timeout,
      });
    const deadline = deadlineAtMs ?? (performance.now() + scaledMs(5000));
    let captureTiming: { startedEpochMs: number; finishedEpochMs: number };
    const captureAttempts: Record<string, unknown>[] = [];
    const sequence = flatCaptureSequence++;
    const captureCompleted = async () => {
      const startedEpochMs = Date.now();
      const attempt: Record<string, unknown> = { startedEpochMs };
      captureAttempts.push(attempt);
      const passes: Record<string, unknown>[] = [];
      attempt.passes = passes;
      try {
        // Only a pass in which no canvas frame was acquired or written between settling and the
        // end of the screenshot is accepted. The hold bounds how many frames can arrive meanwhile;
        // the capture deadline bounds how often a slow GPU makes it hold again.
        for (;;) {
          const heldAt = performance.now();
          // Not raced against the deadline: a hold armed after the capture gave up would never be
          // released.
          const hold = await holdCanvasCompletions(owner);
          let holdSpent = false;
          try {
            for (let pass = 1; pass <= kMaxCanvasQuiescencePasses && !holdSpent; ++pass) {
              const record: Record<string, unknown> = { pass, settleDispatchEpochMs: Date.now() };
              passes.push(record);
              const settled = await settleCanvasWritesUntil(
                owner,
                Math.min(deadline, heldAt + kMaxCanvasSettleHoldMs),
              );
              if (performance.now() >= deadline) {
                throw new Error("animation capture deadline exceeded");
              }
              const holdRemaining = heldAt + kMaxCanvasCompletionHoldMs - performance.now();
              if (settled === null || holdRemaining < kMinHeldScreenshotMs) {
                // The GPU is slow enough that holding on would approach the app's own completion
                // deadline. Let the held frames retire, then hold again.
                record.holdBudgetSpent = true;
                holdSpent = true;
                break;
              }
              record.settled = settled;
              record.settledEpochMs = Date.now();
              const remaining = deadline - performance.now();
              const screenshotTimeout = Math.min(remaining, holdRemaining);
              record.screenshotStartedEpochMs = Date.now();
              let image: Buffer;
              try {
                image = await screenshot(screenshotTimeout);
              } catch (error) {
                if (screenshotTimeout >= remaining || !(error instanceof errors.TimeoutError)) {
                  throw error;
                }
                record.holdBudgetSpent = true;
                holdSpent = true;
                break;
              }
              record.screenshotFinishedEpochMs = Date.now();
              const after = await awaitBeforeBlueDeadline(readCanvasActivity(owner), deadline);
              record.canvasActivityAfterScreenshot = after;
              if (performance.now() >= deadline) {
                throw new Error("animation capture deadline exceeded");
              }
              if (after.canvasSubmits === settled.canvasSubmits && after.frames === settled.frames) {
                captureTiming = { startedEpochMs, finishedEpochMs: Date.now() };
                return image;
              }
            }
          } finally {
            await hold.release().catch((error: unknown) => {
              attempt.holdReleaseError = String(error);
            });
          }
          if (!holdSpent) {
            throw new Error(
              "the canvas kept presenting new frames under the capture's completion hold",
            );
          }
        }
      } catch (error) {
        attempt.error = String(error);
        throw error;
      } finally {
        attempt.finishedEpochMs = Date.now();
        await writeFile(
          testInfo.outputPath(`animation-capture-${sequence}-timing.json`),
          JSON.stringify({ region, viewport, captureAttempts }, null, 2),
        );
      }
    };
    // Retain a single exact-background retake as evidence. This does not establish that
    // the application never flickered; every nonuniform image still reaches the pixel oracle.
    const png: Buffer = unclipped
      ? await retakeFlatCapture({
        capture: captureCompleted,
        isFlat: isOnlyEditorPageBackground,
        deadline,
        retain: async (image: Buffer, attempt: number) => {
          const stem = `animation-flat-capture-${sequence}-${attempt}`;
          const pngPath = testInfo.outputPath(`${stem}.png`);
          await writeFile(pngPath, image);
          let presentation: unknown;
          try {
            presentation = await awaitBeforeBlueDeadline(
              page.evaluate(() => ({
                queue: window.__donnerPresentationQueueStats,
                worker: window.__donnerWorkerStats,
              })),
              deadline,
            );
          } catch (error) {
            presentation = { diagnosticError: String(error) };
          }
          await writeFile(
            testInfo.outputPath(`${stem}.json`),
            JSON.stringify(
              {
                ...captureTiming,
                presentationObservedAfterCapture: presentation,
                region,
                viewport,
                exactPageBackground: isOnlyEditorPageBackground(image),
                pageBackgroundRgba: [16, 19, 23, 255],
              },
              null,
              2,
            ),
          );
          await testInfo.attach(stem, { path: pngPath, contentType: "image/png" });
        },
      })
      : await captureCompleted();
    if (performance.now() >= deadline) throw new Error("animation capture deadline exceeded");
    const offsetX = unclipped ? region.x : 0;
    const offsetY = unclipped ? region.y : 0;
    const read = (
      target: EditorPixelTarget,
      search = { minX: 0, minY: 0, maxX: region.width, maxY: region.height },
    ): PixelBounds | null => {
      const bounds = readEditorPixelBoundsFromPng(png, target, unclipped ? viewport : region, {
        minX: offsetX + search.minX,
        minY: offsetY + search.minY,
        maxX: offsetX + search.maxX,
        maxY: offsetY + search.maxY,
      });
      return bounds === null ? null : {
        ...bounds,
        minX: bounds.minX - offsetX,
        minY: bounds.minY - offsetY,
        maxX: bounds.maxX - offsetX,
        maxY: bounds.maxY - offsetY,
      };
    };
    return { png, read, viewport };
  };
  return capture;
}

function createAnimationOrbProbe(
  captureAnimationRegion: AnimationRegionCaptureFn,
  track: CssRegion,
  testInfo: TestInfo,
  evidenceContext: Record<string, unknown>,
) {
  let traceScreenshots = false;
  const screenshots: { startedEpochMs: number; finishedEpochMs: number; height: number | null }[] =
    [];
  const capture = async (deadlineAtMs?: number) => {
    const started = Date.now();
    const frame = await captureAnimationRegion(track, deadlineAtMs);
    const result = frame.read("animation-mint");
    if (traceScreenshots && screenshots.length < 64) {
      screenshots.push({
        startedEpochMs: started,
        finishedEpochMs: Date.now(),
        height: result ? result.maxY - result.minY : null,
      });
    }
    if (!result) {
      const missingPath = testInfo.outputPath("animation-orb-missing.png");
      await writeFile(missingPath, frame.png);
      await writeFile(
        testInfo.outputPath("animation-orb-missing-region.json"),
        JSON.stringify({ track, viewport: frame.viewport, ...evidenceContext }, null, 2),
      );
      await testInfo.attach("animation-orb-missing", {
        path: missingPath,
        contentType: "image/png",
      });
      throw new Error("the presented animation orb disappeared");
    }
    return result;
  };
  return {
    capture,
    screenshots,
    setTraceScreenshots: (enabled: boolean) => {
      traceScreenshots = enabled;
    },
  };
}

function createAnimationCadenceReader(page: Page, samples: unknown[]) {
  return async () => {
    const sample = await page.evaluate(() => {
      const orZero = (value: number | null | undefined) => value ?? 0;
      return {
        time: performance.now(),
        timeOrigin: performance.timeOrigin,
        frames: orZero(window.__donnerMainLoopRenderedFrames),
        documents: orZero(window.__donnerWorkerStats?.completedResults),
        presentedDocuments: orZero(window.__donnerPresentedDocumentStats?.count),
        presentedAtMs: window.__donnerPresentedDocumentStats?.lastPresentedAtMs,
        presentationPublicationDelayMs: window.__donnerPresentedDocumentStats
          ?.lastPublicationDelayMs,
        worker: window.__donnerWorkerStats,
        resources: window.__donnerPresentationResourceStats,
        thumbnails: window.__donnerSampleThumbnailStats,
        layerThumbnails: window.__donnerLayerThumbnailStats,
        explicitCaptureStarts: orZero(window.__donnerWgpuReadbackCaptureStarts),
        explicitCaptureCompletions: orZero(window.__donnerWgpuReadbackCaptureCompletions),
        pageState: { visibilityState: document.visibilityState, hasFocus: document.hasFocus() },
      };
    });
    if (samples.length < 256) samples.push(sample);
    return sample;
  };
}

// Deliberate fault injection: how long the app thread's event loop is held while a test dispatches
// input. Pointer events reach that thread through its proxied event queue, so they wait behind the
// hold; the hold must outlast the dispatch, which the helper checks rather than assumes.
const kAppThreadInputHoldMs = 3000;

// Busy-hold the app thread's event loop while `dispatch` sends input from the page, so every event
// is still undelivered when the hold ends and the app must apply them in the order they were sent.
async function dispatchWhileAppThreadHeld(
  page: Page,
  owner: Worker,
  dispatch: () => Promise<void>,
) {
  const channel = `donner-app-thread-hold-${Date.now()}`;
  await page.evaluate((name) => {
    const scope = window as typeof window & { __donnerAppThreadHoldStarted?: Promise<void> };
    scope.__donnerAppThreadHoldStarted = new Promise<void>((resolve) => {
      const listener = new BroadcastChannel(name);
      listener.onmessage = () => {
        listener.close();
        resolve();
      };
    });
  }, channel);
  const hold = owner.evaluate(({ name, holdMs }) => {
    const notifier = new BroadcastChannel(name);
    const startedEpochMs = Date.now();
    notifier.postMessage("held");
    while (Date.now() - startedEpochMs < holdMs) continue;
    notifier.close();
    return Date.now();
  }, { name: channel, holdMs: kAppThreadInputHoldMs });
  try {
    // A hold that fails or ends before the page sees it start must fail here, not hang.
    const holdEnded = hold.then(() => {
      throw new Error("the app-thread hold ended before the page observed it");
    });
    holdEnded.catch(() => undefined);
    await Promise.race([
      page.evaluate(() =>
        (window as typeof window & { __donnerAppThreadHoldStarted?: Promise<void> })
          .__donnerAppThreadHoldStarted
      ),
      holdEnded,
    ]);
    await dispatch();
    const dispatchedEpochMs = await page.evaluate(() => Date.now());
    const releasedEpochMs = await hold;
    expect(dispatchedEpochMs, "input dispatch finishes while the app thread is held")
      .toBeLessThan(releasedEpochMs);
  } finally {
    await hold.catch(() => undefined);
  }
}

async function finishAnimationPreview({
  page,
  editCenter,
  editWidth,
  orb,
  start,
  scale,
  rafControls,
  captureRafControl,
}: {
  page: Page;
  editCenter: { x: number; y: number };
  editWidth: number;
  orb: () => Promise<PixelBounds>;
  start: PixelBounds;
  scale: number;
  rafControls: boolean;
  captureRafControl: (name: string) => Promise<void>;
}) {
  if (kEvaluationResumeDeferMs > 0) {
    // The quit probe below shuts this raster worker down, which removes the deferral with it.
    await deferEvaluationResumes(await findRasterTaskWorker(page), kEvaluationResumeDeferMs);
  }
  const heldForEdit = await holdRasterTaskYield(page);
  try {
    const frames = await page.evaluate(() => window.__donnerMainLoopRenderedFrames ?? 0);
    await page.mouse.click(editCenter.x, editCenter.y);
    await expect.poll(() => page.evaluate(() => window.__donnerMainLoopRenderedFrames ?? 0), {
      timeout: scaledMs(3000),
      message: "Edit remains responsive while a raster yield is held",
    }).toBeGreaterThan(frames + 1);
  } finally {
    await heldForEdit.release();
  }
  await expect.poll(async () => (await orb()).minX, { timeout: scaledMs(5000) }).toBeCloseTo(
    start.minX,
    0,
  );
  await page.mouse.click(editCenter.x + editWidth + 1, editCenter.y);
  await expect.poll(async () => Math.abs((await orb()).minX - start.minX), {
    timeout: scaledMs(5000),
    message: "Preview restarts after the suspended-yield cancellation",
  }).toBeGreaterThan(8 * scale);
  const owner = await Promise.any(
    page.workers().map(async (worker) => {
      const owned = await worker.evaluate(() =>
        Boolean((globalThis as typeof globalThis & { __donnerGpuOwner?: boolean }).__donnerGpuOwner)
      );
      if (!owned) throw new Error("not the GPU owner");
      return worker;
    }),
  );
  const shutdowns = await page.evaluate(() => window.__donnerRasterShutdownRequests ?? 0);
  const heldForQuit = await holdRasterTaskYield(page);
  try {
    try {
      // Shortcuts are ignored during Preview, so the quit works only if the Edit click is applied
      // first. Dispatch both while the app thread is held, so neither has reached it yet.
      await dispatchWhileAppThreadHeld(page, owner, async () => {
        await page.mouse.click(editCenter.x, editCenter.y);
        await page.keyboard.down("Control");
        await page.keyboard.down("q");
      });
      await expect.poll(() => page.evaluate(() => window.__donnerRasterShutdownRequests ?? 0), {
        timeout: scaledMs(5000),
        message: "shutdown begins while the raster yield remains held",
      }).toBeGreaterThan(shutdowns);
    } finally {
      await page.keyboard.up("q");
      await page.keyboard.up("Control");
    }
    expect(await heldForQuit.suspended()).toBe(true);
  } finally {
    await heldForQuit.release();
  }
  await expect.poll(() =>
    heldForQuit.raster.evaluate(() => {
      const scope = globalThis as typeof globalThis & {
        __donnerRasterWorker?: boolean;
        __donnerRasterTaskYield?: unknown;
      };
      return {
        marker: Boolean(scope.__donnerRasterWorker),
        channel: Boolean(scope.__donnerRasterTaskYield),
      };
    }), { timeout: scaledMs(5000), message: "raster teardown closes its task channel" })
    .toEqual({ marker: false, channel: false });
  await expect.poll(
    () =>
      owner.evaluate(() =>
        Boolean((globalThis as typeof globalThis & { __donnerGpuOwner?: boolean }).__donnerGpuOwner)
      ),
    { timeout: scaledMs(5000), message: "GPU owner shuts down after joining the raster worker" },
  ).toBe(false);
  if (rafControls) {
    await page.goto("about:blank");
    await captureRafControl("blankAfter");
  }
}

async function exerciseAnimationPointerPulse({
  page,
  testInfo,
  box,
  originX,
  originY,
  scale,
  captureAnimationRegion,
  interactionCaptureOwner,
  orbProbe,
  orb,
  start,
}: {
  page: Page;
  testInfo: TestInfo;
  box: CssRegion;
  originX: number;
  originY: number;
  scale: number;
  captureAnimationRegion: AnimationRegionCaptureFn;
  interactionCaptureOwner: Worker | null;
  orbProbe: ReturnType<typeof createAnimationOrbProbe>;
  orb: (deadlineAtMs?: number) => Promise<PixelBounds>;
  start: PixelBounds;
}) {
  let preQueuedPointerTrace: unknown;
  let initialPulseObservation: unknown = null;
  let queuedPulseObservation: unknown = null;
  let pulseCompletionTrace: unknown = null;
  let pulseClickStartedEpochMs: number | null = null;
  let pulseClickFinishedEpochMs: number | null = null;
  let delayedPulseClickStartedEpochMs: number | null = null;
  let delayedPulseClickFinishedEpochMs: number | null = null;
  let queuedPulseSteppingEpochMs: number | null = null;
  let heldPulseHeight: number | null = null;
  let queuedPulseCatchUp: unknown = null;
  let removeRasterYieldDelay: (() => Promise<unknown>) | null = null;
  const initialPulseDecisions: unknown[] = [];
  const queuedPulseDecisions: unknown[] = [];
  try {
    await Promise.all(
      page.workers().map((worker) =>
        boundBrowserDiagnostic(
          worker.evaluate(() => {
            (globalThis as unknown as Record<string, unknown>).__donnerPreviewPointerTrace = [];
          }),
          1000,
        )
      ),
    );
    const pad = { x: box.x + originX + 184 * scale, y: box.y + originY + 314 * scale };
    const padRegion = {
      x: box.x + originX + 80 * scale,
      y: box.y + originY + 280 * scale,
      width: 200 * scale,
      height: 65 * scale,
    };
    const padPixels = async (
      target: "animation-hover" | "animation-pressed",
      deadline: number,
    ) => {
      const capture = await captureAnimationRegion(padRegion, deadline);
      const pixels = capture.read(target)?.pixels ?? 0;
      if (pixels <= 1000) {
        await writeFile(testInfo.outputPath(`${target}-probe.png`), capture.png);
        await writeFile(
          testInfo.outputPath(`${target}-probe.json`),
          JSON.stringify({ padRegion, viewport: capture.viewport, pixels }, null, 2),
        );
      }
      return pixels;
    };
    const verifyPadInput = async (
      eventType: number,
      target: "animation-hover" | "animation-pressed",
      action: () => Promise<void>,
    ) => {
      if (interactionCaptureOwner === null) throw new Error("animation owner is unavailable");
      const deadline = performance.now() + scaledMs(5000);
      const notBefore = Date.now();
      await awaitBeforeBlueDeadline(action(), deadline);
      // Screenshot capture can stall Firefox's worker progress. Let the actual input
      // dispatch and its following document presentation finish before asking for pixels.
      await expect.poll(
        () =>
          awaitBeforeBlueDeadline(
            interactionCaptureOwner.evaluate(({ eventType, notBefore }) => {
              const trace =
                (globalThis as unknown as Record<string, unknown>).__donnerPreviewPointerTrace;
              return Array.isArray(trace) && trace.some((row) =>
                Array.isArray(row)
                && row[0] >= notBefore && row[1] === 2 && row[2] === eventType && row[5] === true
              );
            }, { eventType, notBefore }),
            deadline,
          ),
        {
          timeout: Math.max(1, deadline - performance.now()),
          intervals: [16],
          message: "the pointer event reaches the preview document",
        },
      ).toBe(true);
      const previous = await awaitBeforeBlueDeadline(
        page.evaluate(() => window.__donnerWorkerStats?.completedResults ?? 0),
        deadline,
      );
      await expect.poll(() =>
        awaitBeforeBlueDeadline(
          page.evaluate((before) => {
            const stats = window.__donnerWorkerStats;
            return (stats?.completedResults ?? 0) > before
              && stats?.resultFramePresented === true;
          }, previous),
          deadline,
        ), {
        timeout: Math.max(1, deadline - performance.now()),
        intervals: [16],
        message: "a document frame after input dispatch is presented",
      }).toBe(true);
      await expect.poll(() => padPixels(target, deadline), {
        timeout: Math.max(1, deadline - performance.now()),
        intervals: [50],
      }).toBeGreaterThan(1000);
      if (performance.now() >= deadline) throw new Error("animation input deadline exceeded");
    };
    await verifyPadInput(7, "animation-hover", () => page.mouse.move(pad.x, pad.y));
    await verifyPadInput(2, "animation-pressed", () => page.mouse.down());
    await verifyPadInput(3, "animation-hover", () => page.mouse.up());
    orbProbe.setTraceScreenshots(true);
    if (interactionCaptureOwner === null) throw new Error("animation owner is unavailable");
    const owner = interactionCaptureOwner;
    const raster = await findRasterTaskWorker(page);
    if (kPulseRasterYieldDelayMs > 0 && kPulseRasterYieldDelayPhase === "all") {
      removeRasterYieldDelay = await delayRasterTaskYields(raster, kPulseRasterYieldDelayMs);
    }
    const initialPulseTraceGeneration = Date.now();
    await interactionCaptureOwner.evaluate((generation) => {
      const trace = (globalThis as unknown as Record<string, unknown>).__donnerPreviewPointerTrace;
      if (!Array.isArray(trace)) throw new Error("the pointer trace is unavailable");
      Object.assign(trace, { generation });
    }, initialPulseTraceGeneration);
    const initialPulseDeadline = performance.now() + scaledMs(5000);
    const pulseClickNotBefore = Date.now();
    pulseClickStartedEpochMs = pulseClickNotBefore;
    let initialPulseTarget: number | null = null;
    // Gate raster progress before the click; the click is dispatched at the next stepped frame,
    // without the long stall of the queued probe below. The frame retained for capture is the one
    // whose document time the raster trace names, not a later frame rasterized while a hold
    // installs.
    // Capture may take seconds in Firefox, so the gate holds that frame until pixels are read.
    const initialGate = await gateRasterTaskYields(raster, scaledMs(6000));
    await withRasterGate(initialGate, () =>
      withFramePacedPreviewClock(owner, async () => {
        await awaitBeforeBlueDeadline(
          page.mouse.click(box.x + originX + 456 * scale, pad.y),
          initialPulseDeadline,
        );
        pulseClickFinishedEpochMs = Date.now();
        const retained = await retainGatedPulseFrame({
          gate: initialGate,
          owner,
          generation: initialPulseTraceGeneration,
          target: null,
          notBeforeEpochMs: pulseClickNotBefore,
          deadlineAtMs: initialPulseDeadline,
          decisions: initialPulseDecisions,
        });
        initialPulseObservation = retained;
        initialPulseTarget = retained.dispatchTarget;
        await expect.poll(async () => {
          const bounds = await orb(initialPulseDeadline);
          return bounds.maxY - bounds.minY;
        }, {
          timeout: Math.max(1, initialPulseDeadline - performance.now()),
          intervals: [50],
          message: "Click triggers the radius pulse",
        }).toBeGreaterThan(58 * scale);
        await expectGatedFrameRetained(initialGate, retained, initialPulseDeadline);
        if (performance.now() >= initialPulseDeadline) {
          throw new Error("initial pulse deadline exceeded");
        }
      }), "initial pulse observation failed");
    const pulseCompletionDeadline = performance.now() + scaledMs(5000);
    // Observe a post-pulse presentation before screenshots can stall worker progress.
    await expect.poll(async () => {
      const snapshot = await awaitBeforeBlueDeadline(
        interactionCaptureOwner.evaluate(() => {
          const trace = (globalThis as unknown as Record<string, unknown>)
            .__donnerPreviewPointerTrace as (unknown[] & { generation?: number }) | undefined;
          return {
            generation: trace?.generation,
            entries: Array.isArray(trace) ? [...trace] : [],
          };
        }),
        pulseCompletionDeadline,
      );
      pulseCompletionTrace = snapshot;
      return presentedPulseCompletion(
        snapshot,
        initialPulseTraceGeneration,
        initialPulseTarget,
        pulseClickNotBefore,
      ) !== null;
    }, {
      timeout: Math.max(1, pulseCompletionDeadline - performance.now()),
      intervals: [16],
      message: "a frame after the first pulse is presented before capture",
    }).toBe(true);
    await expect.poll(async () => {
      const bounds = await orb(pulseCompletionDeadline);
      return Math.abs((bounds.maxY - bounds.minY) - (start.maxY - start.minY));
    }, {
      timeout: Math.max(1, pulseCompletionDeadline - performance.now()),
      intervals: [50],
      message: "First pulse returns to its base diameter",
    }).toBeLessThanOrEqual(2 * scale);
    if (kPulseRasterYieldDelayMs > 0 && kPulseRasterYieldDelayPhase === "queued") {
      removeRasterYieldDelay = await delayRasterTaskYields(raster, kPulseRasterYieldDelayMs);
    }
    // Hold raster progress longer than the pulse before delivering another click. Its target
    // remains the displayed pad, but its begin time must use the dispatch-time preview clock.
    const pulseTraceGeneration = Date.now();
    if (interactionCaptureOwner === null) throw new Error("animation owner is unavailable");
    preQueuedPointerTrace = await interactionCaptureOwner.evaluate((generation) => {
      const scope = globalThis as unknown as Record<string, unknown>;
      const previous = scope.__donnerPreviewPointerTrace;
      scope.__donnerPreviewPointerTrace = Object.assign([], { generation });
      return previous;
    }, pulseTraceGeneration);
    const firstClick = Array.isArray(preQueuedPointerTrace)
      ? preQueuedPointerTrace.find((row) => row[1] === 2 && row[2] === 0 && row[5] === true)
      : undefined;
    if (!firstClick) throw new Error("the normal pulse dispatch trace is unavailable");
    // The gate holds one yield for the stall, then releases yields one at a time after the click.
    const queuedGate = await gateRasterTaskYields(raster, scaledMs(20000));
    await withRasterGate(queuedGate, () =>
      withFramePacedPreviewClock(owner, async () => {
        await queuedGate.awaitSuspended(
          performance.now() + scaledMs(3000),
          "one raster task yield is suspended",
        );
        const heldBounds = await orb();
        heldPulseHeight = heldBounds.maxY - heldBounds.minY;
        expect(
          Math.abs(heldPulseHeight - (start.maxY - start.minY)),
          "held frame contains the base orb, not the previous pulse",
        ).toBeLessThanOrEqual(2 * scale);
        const heldAt = Date.now();
        await expect.poll(() => Date.now() - heldAt, {
          timeout: scaledMs(3000),
          intervals: [50],
          message: "raster stall exceeds the short pulse duration",
        }).toBeGreaterThanOrEqual(1000);
        delayedPulseClickStartedEpochMs = Date.now();
        await page.mouse.click(box.x + originX + 456 * scale, pad.y);
        delayedPulseClickFinishedEpochMs = Date.now();
        const queuedPulseDeadline = performance.now() + scaledMs(5000);
        queuedPulseSteppingEpochMs = Date.now();
        // Avoid screenshot-induced stalls while queued input is delivered. Playback cadence is
        // measured in its own case, without holds or screenshots.
        const retained = await retainGatedPulseFrame({
          gate: queuedGate,
          owner,
          generation: pulseTraceGeneration,
          target: firstClick[3],
          notBeforeEpochMs: delayedPulseClickStartedEpochMs,
          deadlineAtMs: queuedPulseDeadline,
          decisions: queuedPulseDecisions,
        });
        queuedPulseObservation = retained;
        // Frame pacing advances only admitted ticks, so state the catch-up directly: the click
        // must begin on the dispatch-time clock, not at the stale clock its input was queued on.
        const catchUp = queuedDispatchCaughtUp(
          await readPreviewPointerTrace(owner, queuedPulseDeadline),
          pulseTraceGeneration,
          firstClick[3],
          delayedPulseClickStartedEpochMs,
        );
        queuedPulseCatchUp = catchUp;
        expect(catchUp.caughtUp, `queued click dispatch caught up: ${JSON.stringify(catchUp)}`)
          .toBe(true);
        await expect.poll(async () => {
          const bounds = await orb(queuedPulseDeadline);
          return bounds.maxY - bounds.minY;
        }, {
          timeout: Math.max(1, queuedPulseDeadline - performance.now()),
          intervals: [50],
          message: "Queued click pulses after stale-frame catch-up",
        }).toBeGreaterThan(58 * scale);
        await expectGatedFrameRetained(queuedGate, retained, queuedPulseDeadline);
        if (performance.now() >= queuedPulseDeadline) {
          throw new Error("queued pulse deadline exceeded");
        }
      }), "queued pulse observation failed");
  } finally {
    orbProbe.setTraceScreenshots(false);
    await removeRasterYieldDelay?.();
    const pointerTraces = await Promise.all(
      page.workers().map((worker) =>
        boundBrowserDiagnostic(
          worker.evaluate(() => {
            const scope = globalThis as unknown as Record<string, unknown>;
            const trace = scope.__donnerPreviewPointerTrace;
            delete scope.__donnerPreviewPointerTrace;
            return trace;
          }),
          1000,
        )
      ),
    );
    const pulsePath = testInfo.outputPath("animation-pulse-input.json");
    await writeFile(
      pulsePath,
      JSON.stringify(
        {
          pulseClickStartedEpochMs,
          pulseClickFinishedEpochMs,
          delayedPulseClickStartedEpochMs,
          delayedPulseClickFinishedEpochMs,
          queuedPulseSteppingEpochMs,
          heldPulseHeight,
          queuedPulseCatchUp,
          initialPulseObservation,
          preQueuedPointerTrace,
          queuedPulseObservation,
          pulseCompletionTrace,
          initialPulseDecisions,
          queuedPulseDecisions,
          pulseScreenshots: orbProbe.screenshots,
          pointerTraces,
        },
        null,
        2,
      ),
    );
    await testInfo.attach("animation-pulse-input", {
      contentType: "application/json",
      path: pulsePath,
    });
  }
}

async function measureAnimationCadence(page: Page, testInfo: TestInfo) {
  // Measure ordinary playback with screenshot/readback probes stopped.
  testInfo.setTimeout(testInfo.timeout + 18000);
  const loopBefore = await page.evaluate(() => window.__donnerFrameLoopStats);
  const schedulerBefore = await readCarouselFrameDiagnostics(page);
  const loadBefore = loadavg();
  const workerSamples: unknown[] = [];
  const readCadence = createAnimationCadenceReader(page, workerSamples);
  const ownerBefore = await readGpuOwnerDispatchStats(page);
  const before = await readCadence();
  const probeMainRaf = process.env.DONNER_ANIMATION_RAF_PROBE === "1";
  const probeOwnerRaf = probeMainRaf && process.env.DONNER_ANIMATION_OWNER_RAF_PROBE !== "0";
  if (probeMainRaf) {
    await page.evaluate(() => {
      const state = window as unknown as Record<string, unknown>;
      const samples: number[][] = [];
      const deadline = performance.now() + 25000;
      let request = 0;
      const tick = (timestamp: number) => {
        samples.push([timestamp, performance.now()]);
        if (samples.length < 4096 && performance.now() < deadline) {
          request = requestAnimationFrame(tick);
        }
      };
      request = requestAnimationFrame(tick);
      state.__donnerStopAnimationRafProbe = () => {
        cancelAnimationFrame(request);
        delete state.__donnerStopAnimationRafProbe;
        return samples;
      };
    });
  }
  let after: Awaited<ReturnType<typeof readCadence>>;
  let mainRafSamples: number[][] | null = null;
  let ownerProbe: Worker | undefined;
  let ownerRaf: {
    timeOrigin: number;
    samples: number[][];
    counts: number[];
    registrations: number[][];
  } | null = null;
  try {
    if (probeMainRaf) ownerProbe = await startOwnerFrameProbe(page, probeOwnerRaf);
    await expect.poll(async () => (await readCadence()).time - before.time, {
      timeout: 25000,
      intervals: [100],
    }).toBeGreaterThanOrEqual(20000);
    after = await readCadence();
  } finally {
    if (ownerProbe) {
      ownerRaf = await stopOwnerFrameProbe(ownerProbe);
    }
    if (probeMainRaf) {
      mainRafSamples = await boundBrowserDiagnostic(
        page.evaluate(() => {
          const state = window as unknown as Record<string, unknown>;
          const stop = state.__donnerStopAnimationRafProbe as () => number[][];
          return stop();
        }),
        2000,
      );
    }
  }
  return {
    loopBefore,
    schedulerBefore,
    loadBefore,
    workerSamples,
    ownerBefore,
    before,
    probeMainRaf,
    probeOwnerRaf,
    after,
    mainRafSamples,
    ownerRaf,
  };
}

type AnimationCadenceMeasurement = Awaited<ReturnType<typeof measureAnimationCadence>>;

function assertAnimationWorkerTimings(after: AnimationCadenceMeasurement["after"]) {
  for (const field of ["idleYieldMs", "idleCallbackMs", "idleLockMs", "idleWaitMs"] as const) {
    expect(after.worker?.[field], `worker timing ${field}`).toBeGreaterThanOrEqual(0);
    expect(Number.isFinite(after.worker?.[field]), `finite worker timing ${field}`).toBe(true);
  }
}

async function writeAnimationCadenceEvidence({
  page,
  testInfo,
  browserGpuInfo,
  box,
  measurement,
}: {
  page: Page;
  testInfo: TestInfo;
  browserGpuInfo: unknown;
  box: CssRegion;
  measurement: AnimationCadenceMeasurement;
}) {
  const {
    loopBefore,
    schedulerBefore,
    loadBefore,
    workerSamples,
    ownerBefore,
    before,
    probeMainRaf,
    probeOwnerRaf,
    after,
    mainRafSamples,
    ownerRaf,
  } = measurement;
  const ownerAfter = await readGpuOwnerDispatchStats(page);
  const loopAfter = await page.evaluate(() => window.__donnerFrameLoopStats);
  const schedulerAfter = await readCarouselFrameDiagnostics(page);
  const presentation = await page.evaluate(() => window.__donnerPresentedDocumentStats);
  const presentedSamples = (presentation?.samples ?? [])
    .filter((sample) =>
      sample[0] > before.presentedDocuments && sample[0] <= after.presentedDocuments
    )
    .sort((a, b) => a[0] - b[0]);
  const seconds = (after.time - before.time) / 1000;
  const producerSpanMs = presentedSamples.length > 1
    ? presentedSamples.at(-1)![3] - presentedSamples[0][3]
    : 0;
  const presentationGapsMs = presentedSamples.slice(1)
    .map((sample, index) => sample[3] - presentedSamples[index][3]);
  const ownerTimeOrigin = ownerRaf?.timeOrigin ?? 0;
  const cadencePath = testInfo.outputPath("animation-cadence.json");
  const cadence = {
    browser: testInfo.project.name,
    launch: {
      headless: testInfo.project.use.headless ?? null,
      launchHeadless: testInfo.project.use.launchOptions?.headless ?? null,
    },
    browserGpuInfo,
    seconds,
    uiFps: (after.frames - before.frames) / seconds,
    documentFps: (after.documents - before.documents) / seconds,
    deliveredPresentationFps: (after.presentedDocuments - before.presentedDocuments) / seconds,
    presentedDocumentFps: producerSpanMs > 0
      ? (presentedSamples.length - 1) * 1000 / producerSpanMs
      : null,
    presentation: {
      before: before.presentedDocuments,
      after: after.presentedDocuments,
      samples: presentedSamples,
      producerSpanMs,
      gapsMs: presentationGapsMs,
    },
    host: {
      platform: platform(),
      release: release(),
      architecture: arch(),
      cpuModel: cpus()[0]?.model,
      logicalCpus: cpus().length,
      memoryBytes: totalmem(),
      loadBefore,
      loadAfter: loadavg(),
    },
    viewport: box,
    gpuOwner: { before: ownerBefore, after: ownerAfter },
    workerSamples,
    pageState: { before: before.pageState, after: after.pageState },
    frameLoop: { before: loopBefore, after: loopAfter },
    scheduler: {
      before: schedulerBefore,
      after: schedulerAfter,
      mainRafProbeEnabled: probeMainRaf,
      ownerRafProbeEnabled: probeOwnerRaf,
      ownerRaf: ownerRaf
        ? {
          timeOrigin: ownerTimeOrigin,
          previewAdvanceCounts: ownerRaf.counts,
          registrations: ownerRaf.registrations.filter((record) => {
            const observedMs = ownerTimeOrigin + record[3] - before.timeOrigin;
            return record[4] >= 0 && observedMs >= before.time && observedMs <= after.time;
          }),
          samples: ownerRaf.samples.filter((sample) => {
            const observedMs = ownerTimeOrigin + sample[1] - before.timeOrigin;
            return observedMs >= before.time && observedMs <= after.time;
          }),
        }
        : null,
      intervalStartMs: before.time,
      intervalEndMs: after.time,
      mainRafSamples:
        mainRafSamples?.filter((sample) => sample[1] >= before.time && sample[1] <= after.time)
          ?? null,
    },
    timeline: await readCarouselGpuTimeline(page),
    environment: await page.evaluate(() => ({
      userAgent: navigator.userAgent,
      devicePixelRatio,
      worker: window.__donnerWorkerStats,
      resources: window.__donnerPresentationResourceStats,
    })),
  };
  await writeFile(cadencePath, JSON.stringify(cadence, null, 2));
  await testInfo.attach("animation-cadence", {
    contentType: "application/json",
    path: cadencePath,
  });
  return { ownerAfter, presentedSamples };
}

function assertAnimationCadence(
  measurement: AnimationCadenceMeasurement,
  evidence: Awaited<ReturnType<typeof writeAnimationCadenceEvidence>>,
) {
  const { before, after, ownerBefore } = measurement;
  const { ownerAfter, presentedSamples } = evidence;
  expect(presentedSamples.length, "complete distinct-presentation sample interval").toBe(
    after.presentedDocuments - before.presentedDocuments,
  );
  expect(presentedSamples.length).toBeGreaterThan(10);
  for (let index = 1; index < presentedSamples.length; ++index) {
    expect(presentedSamples[index][0]).toBe(presentedSamples[index - 1][0] + 1);
    expect(presentedSamples[index][3]).toBeGreaterThanOrEqual(presentedSamples[index - 1][3]);
  }
  expect(before.layerThumbnails?.previewPaused, "sidebar work is paused in Preview").toBe(true);
  expect(after.layerThumbnails?.previewPaused, "sidebar work remains paused").toBe(true);
  for (const field of ["refreshPasses", "renderAttempts"] as const) {
    expect(Number.isSafeInteger(before.layerThumbnails?.[field]), `initial sidebar ${field}`)
      .toBe(true);
    expect(after.layerThumbnails?.[field], `no sidebar ${field} during playback`)
      .toBe(before.layerThumbnails?.[field]);
  }
  const gpuBefore = ownerBefore?.gpu as
    | { readbackCopies: number; cpuTextureWrites: number }
    | undefined;
  expect(gpuBefore, "GPU residency counters are available").toBeTruthy();
  for (const field of ["readbackCopies", "cpuTextureWrites"] as const) {
    expect(Number.isFinite(gpuBefore?.[field]), `finite initial ${field}`).toBe(true);
    const gpuAfter = ownerAfter?.gpu as typeof gpuBefore;
    expect(Number.isFinite(gpuAfter?.[field]), `finite final ${field}`).toBe(true);
  }
  expect(ownerAfter?.gpu, "ordinary animation stays GPU resident").toMatchObject({
    readbackCopies: gpuBefore?.readbackCopies,
    cpuTextureWrites: gpuBefore?.cpuTextureWrites,
  });
}

async function reportAnimationCadence({
  page,
  testInfo,
  browserGpuInfo,
  box,
  measurement,
}: {
  page: Page;
  testInfo: TestInfo;
  browserGpuInfo: unknown;
  box: CssRegion;
  measurement: AnimationCadenceMeasurement;
}) {
  assertAnimationWorkerTimings(measurement.after);
  const evidence = await writeAnimationCadenceEvidence({
    page,
    testInfo,
    browserGpuInfo,
    box,
    measurement,
  });
  assertAnimationCadence(measurement, evidence);
}

// Deliberate fault injection: the first animated Preview each browser opens in a run waits this long
// before loading the editor, like a first boot on a freshly provisioned host. A marker in the test's
// temporary directory applies it once per browser and run, also across worker restarts.
const kAnimationFirstBootDelayMs = Number(
  process.env.DONNER_ANIMATION_FIRST_BOOT_DELAY_MS ?? 0,
);
if (!Number.isFinite(kAnimationFirstBootDelayMs) || kAnimationFirstBootDelayMs < 0) {
  throw new Error("invalid DONNER_ANIMATION_FIRST_BOOT_DELAY_MS fault injection");
}

async function injectAnimationFirstBootDelay(page: Page, browserName: string) {
  if (kAnimationFirstBootDelayMs === 0) return;
  const temporaryDirectory = process.env.TEST_TMPDIR;
  if (!temporaryDirectory) throw new Error("the first-boot fault injection needs TEST_TMPDIR");
  try {
    await writeFile(join(temporaryDirectory, `animation-first-boot-delayed-${browserName}`), "", {
      flag: "wx",
    });
  } catch (error) {
    if ((error as { code?: string }).code === "EEXIST") return;
    throw error;
  }
  await page.waitForTimeout(kAnimationFirstBootDelayMs);
}

/**
 * Fault injection, off unless `DONNER_ANIMATION_CANVAS_GPU_DELAY` is `<everyFrames>:<iterations>`:
 * while openAnimatedPreview opens the sample and starts the Preview, postpones GPU execution of
 * the rest of every Nth canvas frame behind fixed fragment work. A capture taken before that
 * frame's GPU work completes can observe a partially executed canvas.
 */
async function armAnimationCanvasGpuDelay(owner: Worker, testInfo: TestInfo) {
  // Ends the injection after the captures that open the Preview, so later probes run without it.
  return armAttachedCanvasGpuDelay(
    owner,
    "DONNER_ANIMATION_CANVAS_GPU_DELAY",
    "animation-canvas-gpu-delay",
    testInfo,
  );
}

/**
 * \ref armCanvasGpuDelayFromEnv, attaching what was injected as `<name>.json` when it finishes.
 */
async function armAttachedCanvasGpuDelay(
  owner: Worker,
  variable: string,
  name: string,
  testInfo: TestInfo,
) {
  const delay = await armCanvasGpuDelayFromEnv(owner, variable);
  if (delay === null) return null;
  return {
    finish: async () => {
      const report = await delay.finish();
      const path = testInfo.outputPath(`${name}.json`);
      await writeFile(path, JSON.stringify(report, null, 2));
      await testInfo.attach(name, { path, contentType: "application/json" });
      return report;
    },
  };
}

async function openAnimatedPreview(page: Page, browserName: string, testInfo: TestInfo) {
  await injectAnimationFirstBootDelay(page, browserName);
  const rafControls = process.env.DONNER_ANIMATION_RAF_CONTROLS === "1";
  if (rafControls) testInfo.setTimeout(testInfo.timeout + 21000);
  const rafControlSamples: Record<string, Awaited<ReturnType<typeof measurePageRaf>>> = {};
  const captureRafControl = async (name: string) => {
    rafControlSamples[name] = await measurePageRaf(page, 5000);
    const controlsPath = testInfo.outputPath("animation-raf-controls.json");
    await writeFile(controlsPath, JSON.stringify(rafControlSamples, null, 2));
    await testInfo.attach("animation-raf-controls-" + name, {
      contentType: "application/json",
      path: controlsPath,
    });
  };
  if (rafControls) await captureRafControl("blankBefore");
  const frameDriver = process.env.DONNER_ANIMATION_FRAME_DRIVER;
  if (frameDriver !== undefined && frameDriver !== "proxied-main-raf") {
    throw new Error("DONNER_ANIMATION_FRAME_DRIVER must be proxied-main-raf when set");
  }
  const fatalMessages = await openEditor(page, { postInitializationDwellMs: 0, frameDriver });
  const browserGpuInfo = rafControls ? await readChromiumGpuInfo(page) : null;
  if (frameDriver) {
    expect(
      await page.evaluate(() =>
        (window as unknown as { __donnerFrameDriver?: string }).__donnerFrameDriver
      ),
      "the requested frame driver is active",
    ).toBe(frameDriver);
  }
  await expectSampleThumbnailsToSettle(page, {
    message: "thumbnail capture work is drained before animation measurement",
    timeout: scaledMs(20000),
  });
  const canvas = page.locator("canvas#canvas");
  const box = await canvas.boundingBox();
  if (!box) throw new Error("the editor canvas has no bounds");
  // Firefox page captures can show a canvas frame whose submitted GPU work has not finished: only
  // part of the frame, such as its clear, or nothing at all. Every capture below holds the canvas
  // owner's frames and waits for their GPU work (see createAnimationRegionCapture), so install the
  // completion probe before the sample opens.
  await expectSurfaceFrameProbeReady(page);
  const probedOwner = await findProbedCanvasApplicationWorker(page);
  if (!probedOwner) throw new Error("the application canvas owner is unavailable");
  const gpuDelay = await armAnimationCanvasGpuDelay(probedOwner, testInfo);
  await page.mouse.click(box.x + box.width * 0.76, box.y + 390);
  const ownerSlot: { worker: Worker | null } = { worker: null };
  await expect.poll(async () => {
    ownerSlot.worker = await findCanvasOwnerWorker(page);
    return ownerSlot.worker !== null;
  }, { message: "animation captures observe the canvas GPU owner" }).toBe(true);
  const canvasOwner = ownerSlot.worker;
  if (!canvasOwner) throw new Error("the canvas GPU owner is unavailable");
  expect(canvasOwner, "the canvas owner is the probed application owner").toBe(probedOwner);
  const captureAnimationRegion = createAnimationRegionCapture(
    page,
    canvasOwner,
    browserName,
    testInfo,
  );
  const documentRegion = { minX: 150, minY: 150, maxX: box.width - 430, maxY: box.height * 0.58 };
  let initial: PixelBounds | null = null;
  let mode: PixelBounds | null = null;
  await expect.poll(async () => {
    const capture = await captureAnimationRegion(box);
    initial = capture.read("animation-mint", documentRegion);
    mode = capture.read("selection-teal", {
      minX: box.width - 410,
      minY: 0,
      maxX: box.width - 10,
      maxY: 120,
    });
    return initial !== null && initial.maxY - initial.minY > 30 && mode !== null;
  }, { timeout: scaledMs(12000), message: "Animation sample and Edit control are visible" }).toBe(
    true,
  );
  if (!initial || !mode) throw new Error("Animation sample did not become visible");
  // Selecting the document can reopen thumbnail work when its fonts arrive.
  await expect.poll(async () => {
    const stats = await page.evaluate(() => window.__donnerSampleThumbnailStats);
    return { stats, quiescent: sampleThumbnailLaneQuiescent(stats) };
  }, {
    message: "sample-triggered thumbnail work finishes before the Edit baseline",
    timeout: scaledMs(20000),
  }).toEqual(expect.objectContaining({ quiescent: true }));
  await expect.poll(() =>
    page.evaluate(() => {
      const queue = window.__donnerPresentationQueueStats;
      return {
        presented: window.__donnerWorkerStats?.resultFramePresented === true,
        framesInFlight: queue?.framesInFlight ?? null,
        completedSerial: queue?.completedSerial ?? null,
        submittedSerial: queue?.submittedSerial ?? null,
      };
    }).then((state) => ({
      ...state,
      complete: state.completedSerial !== null && state.completedSerial === state.submittedSerial,
    })), {
    timeout: scaledMs(5000),
    message: "the Edit baseline reaches completed browser presentation",
  }).toEqual(expect.objectContaining({ presented: true, framesInFlight: 0, complete: true }));
  const initialOrb: PixelBounds = initial;
  const edit: PixelBounds = mode;
  const scale = (initialOrb.maxY - initialOrb.minY + 1) / 48;
  const originX = (initialOrb.minX + initialOrb.maxX) / 2 - 96 * scale;
  const originY = (initialOrb.minY + initialOrb.maxY) / 2 - 175 * scale;
  const editCenter = {
    x: box.x + (edit.minX + edit.maxX) / 2,
    y: box.y + (edit.minY + edit.maxY) / 2,
  };
  const track = {
    x: box.x + originX + 48 * scale,
    y: box.y + originY + 120 * scale,
    width: 560 * scale,
    height: 110 * scale,
  };
  const orbProbe = createAnimationOrbProbe(captureAnimationRegion, track, testInfo, {
    box,
    browserName,
    initialOrb,
  });
  const orb = orbProbe.capture;
  const start = await orb();
  if (rafControls) await captureRafControl("editState");
  await page.mouse.click(editCenter.x + edit.maxX - edit.minX + 1, editCenter.y);
  try {
    await expect.poll(async () => Math.abs((await orb()).minX - start.minX), {
      timeout: scaledMs(10000),
      message: "Preview moves the rendered orb",
    }).toBeGreaterThan(8 * scale);
  } finally {
    await gpuDelay?.finish();
  }

  return {
    box,
    browserGpuInfo,
    canvasOwner,
    captureAnimationRegion,
    captureRafControl,
    editCenter,
    editWidth: edit.maxX - edit.minX,
    fatalMessages,
    orb,
    orbProbe,
    originX,
    originY,
    rafControls,
    scale,
    start,
  };
}

// Declared first so that, in the single-worker animation target, each browser's one-time first-boot
// cost (browser startup, GPU adapter creation, first shader compiles) lands here, not in the probes.
test(
  "animation preview plays on the browser's first boot",
  async ({ page, browserName }, testInfo) => {
    const { fatalMessages } = await openAnimatedPreview(page, browserName, testInfo);
    expect(fatalMessages).toEqual([]);
    expect(await page.evaluate(() => window.__donnerRenderWorkerStartup ?? null)).toEqual({
      beforeUiSetup: false,
      beforeExplicitStart: false,
      afterExplicitStart: true,
    });
  },
);

// The fixed 20 s uninstrumented playback interval is its own case, so the pulse and pointer probes
// below do not share a budget with it.
test(
  "animation preview plays at its playback cadence",
  async ({ page, browserName }, testInfo) => {
    const { box, browserGpuInfo, fatalMessages } = await openAnimatedPreview(
      page,
      browserName,
      testInfo,
    );
    const cadenceMeasurement = await measureAnimationCadence(page, testInfo);
    await reportAnimationCadence({
      page,
      testInfo,
      browserGpuInfo,
      box,
      measurement: cadenceMeasurement,
    });
    const { before, after } = cadenceMeasurement;
    await verifyRasterProgressWithTimersHeld(page);
    expect(after.documents - before.documents).toBeGreaterThan(10);
    expect(fatalMessages).toEqual([]);
  },
);

test(
  "animation preview plays and dispatches mouse events",
  async ({ page, browserName }, testInfo) => {
    const {
      box,
      canvasOwner: interactionCaptureOwner,
      captureAnimationRegion,
      editCenter,
      fatalMessages,
      orb,
      orbProbe,
      originX,
      originY,
      scale,
      start,
    } = await openAnimatedPreview(page, browserName, testInfo);
    const previewCapture = await captureAnimationRegion(box);
    await writeFile(testInfo.outputPath("animation-preview.png"), previewCapture.png);

    await exerciseAnimationPointerPulse({
      page,
      testInfo,
      box,
      originX,
      originY,
      scale,
      captureAnimationRegion,
      interactionCaptureOwner,
      orbProbe,
      orb,
      start,
    });

    await page.mouse.click(editCenter.x, editCenter.y);
    await expect.poll(async () => (await orb()).minX, { timeout: scaledMs(5000) }).toBeCloseTo(
      start.minX,
      0,
    );
    expect(fatalMessages).toEqual([]);
  },
);

test(
  "animation preview plays through cancellation and shutdown while raster task yields are held",
  async ({ page, browserName }, testInfo) => {
    // Exercise the fault-injected lifecycle in a fresh Preview instead of inheriting the pointer
    // pulse case's test deadline.
    const {
      captureRafControl,
      editCenter,
      editWidth,
      fatalMessages,
      orb,
      rafControls,
      scale,
      start,
    } = await openAnimatedPreview(page, browserName, testInfo);
    await finishAnimationPreview({
      page,
      editCenter,
      editWidth,
      orb,
      start,
      scale,
      rafControls,
      captureRafControl,
    });
    expect(fatalMessages).toEqual([]);
  },
);

// Firefox snapshots a canvas whose control was transferred to a worker by running a task on that
// worker and blocking the page's main thread until it answers, for at most
// `gfx.offscreencanvas.snapshot-timeout-ms` (10 s). If the worker is itself blocked on a
// synchronous call to the main thread, neither can proceed until that timeout, and the snapshot
// paints the page without the canvas. While the pointer is over the ImGui panels, the GLFW backend
// sets the OS cursor from the canvas owner on every frame, so keep the pointer moving over a panel
// and require every page capture to include the canvas.
test("page screenshots keep the canvas while hover changes the cursor", async ({ page }, testInfo) => {
  const fatalMessages = await openEditor(page, { postInitializationDwellMs: 0 });
  await expectSampleThumbnailsToSettle(page, {
    message: "thumbnail work is drained before the captures",
    timeout: kStallGuardMs,
  });
  const box = await page.locator("canvas#canvas").boundingBox();
  if (!box) throw new Error("the editor canvas has no bounds");
  // Open the sample the animation lane uses; its document area sets the editor's own cursor and
  // the inspector panel on the right leaves the cursor to ImGui.
  await page.mouse.click(box.x + box.width * 0.76, box.y + 390);
  const canvasCursor = () =>
    page.evaluate(() => getComputedStyle(document.getElementById("canvas")!).cursor);
  const documentPoint = { x: box.x + box.width * 0.4, y: box.y + box.height * 0.375 };
  const panelPoint = { x: box.x + box.width - 200, y: box.y + box.height * 0.375 };
  // The editor's own document cursor appears only once the opened document is interactive.
  await page.mouse.move(documentPoint.x, documentPoint.y);
  await expect.poll(canvasCursor, {
    message: "the opened document sets the editor cursor",
    timeout: kStallGuardMs,
  }).toMatch(/^url\(/);
  // A clip over the open document keeps each capture cheap; without the canvas it is flat.
  const region = {
    x: Math.round(documentPoint.x - 200),
    y: Math.round(documentPoint.y - 150),
    width: 400,
    height: 300,
  };
  // Captured while the pointer is over the document, where the cursor comes from the editor's
  // asynchronous registry: the region must show document content, or a flat capture below would
  // not mean the canvas was lost.
  expect(
    isOnlyEditorPageBackground(await page.screenshot({ clip: region, timeout: kStallGuardMs })),
    "the capture region shows document content",
  ).toBe(false);
  // The panel leaves the cursor to ImGui, whose GLFW backend sets it from the canvas owner.
  await page.mouse.move(panelPoint.x, panelPoint.y);
  await expect.poll(canvasCursor, { message: "the inspector panel uses an ImGui cursor" })
    .not.toMatch(/^url\(/);
  const captures: Array<Record<string, unknown>> = [];
  // A frame's cursor call meets a screenshot on well under one capture in a hundred on a fast
  // host, so take enough captures that the unfixed deadlock is likely within one run;
  // main_thread_proxy_tests pins the fix itself deterministically.
  const kCaptures = 300;
  // Each capture is a bounded step: a stalled one returns flat after the browser's snapshot
  // timeout. Extend the case by one second per capture, several times the loaded-host cost.
  testInfo.setTimeout(testInfo.timeout + kCaptures * 1000);
  try {
    for (let step = 0; step < kCaptures; step++) {
      // Every move starts new frames, and each frame sets the OS cursor from the canvas owner.
      const x = panelPoint.x + (step % 2) * 8;
      const y = panelPoint.y;
      await page.mouse.move(x, y);
      const startedAt = Date.now();
      const capture = await page.screenshot({ clip: region, timeout: kStallGuardMs });
      const record: Record<string, unknown> = {
        step,
        x,
        y,
        screenshotMs: Date.now() - startedAt,
        flat: isOnlyEditorPageBackground(capture),
      };
      captures.push(record);
      expect(
        record.flat,
        `page capture ${step} with the pointer at (${x}, ${y}) did not include the canvas`,
      ).toBe(false);
    }
  } finally {
    const recordPath = testInfo.outputPath("cursor-captures.json");
    await writeFile(recordPath, JSON.stringify(captures, null, 2));
    await testInfo.attach("cursor-captures", { contentType: "application/json", path: recordPath });
  }
  expect(await canvasCursor(), "the inspector panel kept an ImGui cursor").not.toMatch(/^url\(/);
  expect(fatalMessages).toEqual([]);
});

test("browser paste callback applies on the GLFW context owner", async ({ page }) => {
  const fatalMessages = await openEditor(page);
  const installObserver = () => {
    const scope = globalThis as typeof globalThis & {
      __donnerApplicationWorker?: boolean;
      __glfwContextCallbackObserver?: (kind: number, text: string | null) => void;
      __glfwObservedCallbacks?: { kind: number; owner: boolean; text: string | null }[];
    };
    scope.__glfwObservedCallbacks = [];
    scope.__glfwContextCallbackObserver = (kind, text) => {
      scope.__glfwObservedCallbacks!.push({
        kind,
        owner: scope.__donnerApplicationWorker === true,
        text,
      });
    };
  };
  await page.evaluate(installObserver);
  for (const worker of page.workers()) await worker.evaluate(installObserver);
  await page.evaluate(() => {
    const clipboardData = new DataTransfer();
    clipboardData.setData("text/plain", "clipboard owner regression \u03bb");
    document.dispatchEvent(new ClipboardEvent("paste", { bubbles: true, clipboardData }));
  });
  const readCallbacks = () =>
    (globalThis as typeof globalThis & {
      __glfwObservedCallbacks?: { kind: number; owner: boolean; text: string | null }[];
    }).__glfwObservedCallbacks ?? [];
  await expect.poll(async () => {
    const observations = await Promise.all([
      page.evaluate(readCallbacks),
      ...page.workers().map((worker) => worker.evaluate(readCallbacks)),
    ]);
    return observations.flat().filter((entry) => entry.kind === 2);
  }, { message: "paste must update clipboard state on the application pthread" })
    .toEqual([{ kind: 2, owner: true, text: "clipboard owner regression \u03bb" }]);
  expect(fatalMessages).toEqual([]);
});

test.describe("Retina Geode splash", () => {
  test.use({ deviceScaleFactor: 2 });
  test("carousel loads Geode and remains responsive after preview completion", async ({ page }) => {
    const fatalMessages = await openEditor(page, { postInitializationDwellMs: 0 });
    const bounds = await page.locator("canvas#canvas").boundingBox();
    expect(bounds).not.toBeNull();
    if (!bounds) return;
    const startedAt = performance.now();
    await page.mouse.click(bounds.x + bounds.width * 0.5, bounds.y + 282);
    await expect(page.locator("canvas#canvas")).toHaveAttribute(
      "data-active-sample-id",
      "geode-splash",
    );
    await expect.poll(() =>
      page.evaluate(() => ({
        count: window.__donnerLayerThumbnailStats?.rowCount,
        pending: window.__donnerLayerThumbnailStats?.deferredCount,
        textures: window.__donnerLayerThumbnailStats?.textureSnapshotCount,
        lost: window.__donnerWorkerStats?.deviceLost,
      })), { message: "Retina Geode completes every layer preview" })
      .toEqual({ count: 15, pending: 0, textures: 15, lost: false });
    await page.mouse.click(bounds.x + 16, bounds.y + 180);
    await expect.poll(() =>
      page.evaluate(() => window.__donnerViewportStats?.paneX ?? 0),
      { message: "Geode source pane opens without blocking the GPU owner" },
    ).toBeGreaterThan(200);
    console.log(`geode-retina-ready-ms=${performance.now() - startedAt}`);
    for (let step = 0; step < 20; ++step) {
      const x = bounds.x + 400 + step * 15;
      const y = bounds.y + 350;
      await page.mouse.move(x, y);
      await expect.poll(() =>
        page.evaluate(() => ({
          x: window.__donnerInteractionStats?.pointerX,
          y: window.__donnerInteractionStats?.pointerY,
        })), { message: "Retina Geode processes pointer input", timeout: 3000 })
        .toEqual({ x, y });
    }
    console.log(`geode-retina-final ${JSON.stringify(await readCarouselFrameDiagnostics(page))}`);
    await page.screenshot({ path: test.info().outputPath("geode-retina.png") });
    expect(fatalMessages).toEqual([]);
  });
});


test("browser File Open uses the native file picker", async ({ page }) => {
  await page.addInitScript(() => {
    const state = window as Window & { __filePickerCalls?: number; showOpenFilePicker?: unknown };
    state.__filePickerCalls = 0;
    state.showOpenFilePicker = async () => {
      state.__filePickerCalls!++;
      if (!navigator.userActivation.isActive) throw new Error("Open lost user activation");
      return [{
        name: "picked-art.svg",
        getFile: async () =>
          new File(
            [
              "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"400\" height=\"260\"><rect id=\"picked\" x=\"40\" y=\"40\" width=\"140\" height=\"90\" fill=\"lime\"/></svg>",
            ],
            "picked-art.svg",
            { type: "image/svg+xml" },
          ),
      }];
    };
  });
  await openEditor(page, { postInitializationDwellMs: 0 });
  await page.locator("canvas#canvas").focus();
  await page.keyboard.press("Control+o");
  await expect.poll(() =>
    page.evaluate(() => (window as Window & { __filePickerCalls?: number }).__filePickerCalls)
  ).toBe(1);
  await expect(page).toHaveTitle(/picked-art\.svg/);
});

test("browser Save As writes through the native save picker", async ({ page }) => {
  await page.addInitScript(() => {
    const state = window as Window & {
      __savedSvg?: string;
      __savePickerCalls?: number;
      showSaveFilePicker?: unknown;
    };
    state.__savePickerCalls = 0;
    state.showSaveFilePicker = async () => {
      state.__savePickerCalls!++;
      if (!navigator.userActivation.isActive) throw new Error("Save lost user activation");
      return {
        name: "saved-art.svg",
        createWritable: async () => ({
          write: async (source: string | Blob) => {
            state.__savedSvg = typeof source === "string" ? source : await source.text();
          },
          close: async () => {},
          abort: async () => {},
        }),
      };
    };
  });
  await openEditor(page, { postInitializationDwellMs: 0 });
  await page.locator("canvas#canvas").focus();
  await page.keyboard.press("Control+Shift+s");
  await expect.poll(() =>
    page.evaluate(() => (window as Window & { __savePickerCalls?: number }).__savePickerCalls)
  ).toBe(1);
  await expect.poll(() =>
    page.evaluate(() => (window as Window & { __savedSvg?: string }).__savedSvg ?? "")
  ).toContain("<svg");
  await expect(page).toHaveTitle(/saved-art\.svg/);
});

test("browser Open supports the native file input chooser", async ({ page }) => {
  await page.addInitScript(() => {
    Object.defineProperty(window, "showOpenFilePicker", { value: undefined, configurable: true });
  });
  await openEditor(page, { postInitializationDwellMs: 0 });
  await page.locator("canvas#canvas").focus();
  const chooserEvent = page.waitForEvent("filechooser");
  await page.keyboard.press("Control+o");
  const chooser = await chooserEvent;
  await chooser.setFiles({
    name: "native-choice.svg",
    mimeType: "image/svg+xml",
    buffer: Buffer.from(
      "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"400\" height=\"260\"><rect x=\"40\" y=\"40\" width=\"140\" height=\"90\" fill=\"lime\"/></svg>",
    ),
  });
  await expect(page).toHaveTitle(/native-choice\.svg/);
  await page.screenshot({ path: test.info().outputPath("native-file-open.png") });
  await page.evaluate(() => {
    Object.defineProperty(window, "showSaveFilePicker", { value: undefined, configurable: true });
  });
  const downloadEvent = page.waitForEvent("download");
  await page.keyboard.press("Control+Shift+s");
  const download = await downloadEvent;
  expect(download.suggestedFilename()).toBe("native-choice.svg");
  const downloadedPath = await download.path();
  expect(downloadedPath).not.toBeNull();
  expect(await readFile(downloadedPath!, "utf8")).toContain("fill=\"lime\"");
});
