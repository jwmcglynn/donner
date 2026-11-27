import { kCarouselSamples, kCarouselViewport } from "./carousel-samples.mjs";
import { completedResultHandoff } from "./completed-result-handoff.mjs";
import { packageHashes } from "./package-identity.mjs";
import { sumPipelineCompilesPending } from "./pipeline-compile-readiness.mjs";
import { dispatchCanvasPointer } from "./canvas-pointer";
import { expect, type Page, test } from "@playwright/test";
import fs from "node:fs";
import {
  endApplicationTeardown,
  startApplicationHeartbeat,
} from "./browser-application-heartbeat.mjs";
import { checkDocumentGpuResidency } from "./browser-gpu-residency";
import { readGpuOwnerDispatchStats } from "./browser-runtime-diagnostics";
import { resourceQueuesAreQuiescent } from "./browser-resource-quiescence.mjs";
import {
  contentMotionFraction,
  installCompositedProbe,
  startCompositedProbe,
} from "./composited-probe";
import { stopCompositedProbe } from "./composited-probe-evidence.mjs";
import {
  completedDragFrames,
  dragInputLatencies,
  heldDragPositionCount,
  physicallyHeldDragPositionCount,
  kPresentationSamplePollMs,
  kReleaseFrameTimeoutMs,
  streamDragInputs,
} from "./drag-input-stream.mjs";
import { firstClickBreakdown } from "./first-click-breakdown.mjs";
import { inspectGpuImageTransfer } from "./gpu-image-transfer-canary";
import { echoGpuSessionConsole } from "./gpu-session-console";
import {
  checkLatencyGate,
  completionCheckMode,
  kCompletionChecks,
  kLatencyGates,
  latencyGateMode,
  runCompletionCheck,
} from "./latency-gates.mjs";
import {
  heldFastPathResultCount,
  heldMaskedDragShapeFailures,
  summarizeMaskedDragResults,
  unpatchedTileCount,
} from "./masked-drag-counters.mjs";
import { pickerPresentationSettled } from "./presentation-baseline.mjs";
import {
  kHeldDragPresentationBounds,
  presentedDragFailures,
  presentedDragSummary,
} from "./presented-frame-samples.mjs";
import { kSplashDocumentSize, showsSplashDocument, splashDPoint } from "./splash-aim.mjs";
import {
  armCanvasGpuDelayFromEnv,
  canvasGpuDelayFromEnv,
  expectSurfaceFrameProbeReady,
  findProbedCanvasApplicationWorker,
} from "./surface-frame-probe";
import { kStallGuardMs } from "./test-timing";

// Every case here opens the editor; echo its GPU acquisition trace into the test log. The previous
// case's page and context have closed by now, so end its application teardown record: the
// watchdog's teardown deadline bounds that close, not the cases that follow it.
test.beforeEach(({ page }) => {
  endApplicationTeardown();
  echoGpuSessionConsole(page);
});

/** The device scale the running project declares; no case may override it (#2470). */
function projectDeviceScale(info: import("@playwright/test").TestInfo) {
  return Number(info.project.use.deviceScaleFactor ?? 1);
}

// Every passing case must have rendered at its project's device scale, as the page itself reports,
// so that a project's name keeps saying what it measured.
test.afterEach(async ({ page }, info) => {
  if (info.status !== "passed") return;
  const scale = projectDeviceScale(info);
  const raster = await rasterIdentity(page);
  console.log(
    `device-scale ${JSON.stringify({ project: info.project.name, case: info.title, raster })}`,
  );
  expect(raster, `${info.project.name} must render at device scale ${scale}`).toEqual({
    ...raster,
    dpr: scale,
    backingWidth: raster.cssWidth * scale,
    backingHeight: raster.cssHeight * scale,
  });
});

interface Diagnostics extends Window {
  __donnerBackend?: string;
  __donnerFirstFramePresented?: boolean;
  __donnerPresentedDocumentStats?: {
    count: number;
    documentGeneration: number;
    frameVersion: number;
    lastPresentedAtMs: number;
    lastPublicationDelayMs: number;
    samples: [number, number, number, number][];
  };
  __donnerFrameLoopStats?: {
    renderedFrames: number;
    uiFrameMsSamples: number[];
    lastFrameAtMs: number;
    phaseTotalsMs?: Record<string, number>;
  };
  __donnerHostFrameTiming?: { frames: number; sums: Record<string, number> };
  __donnerAsyncifySuspendStats?: {
    frames: number;
    suspends: number;
    totalMs: number;
    tileYieldMs: number;
    gpuReadbackMs: number;
    deviceWaitMs: number;
    startupMs: number;
  };
  __donnerInteractionStats?: {
    pointerX: number;
    pointerY: number;
    pendingClick: boolean;
    workerBusy: boolean;
    renderRequestPending?: boolean;
    viewportSettlePending?: boolean;
    selectedCount: number;
    dragging: boolean;
    moved: boolean;
  };
  __donnerViewportStats?: {
    paneX: number;
    paneY: number;
    paneWidth: number;
    paneHeight: number;
    zoom: number;
    documentX: number;
    documentY: number;
    documentWidth: number;
    documentHeight: number;
  };
  __donnerWorkerStats?: {
    completedResults: number;
    resultFrameCompletedAtMs?: number;
    publishedAtMs: number;
    presentedAtMs?: number;
    workerMs: number;
    queueWaitMs: number;
    dequeueToStartMs: number;
    pollDelayMs: number;
    deviceLost: boolean;
    [name: string]: unknown;
  };
  __donnerSampleThumbnailStats?: {
    pending: boolean;
    active: boolean;
    resultReady: boolean;
    ready?: number;
    rendered?: number;
    drained?: boolean;
    publishedAtMs?: number;
  };
  __donnerLayerThumbnailStats?: {
    rowCount: number;
    renderedCount: number;
    deferredCount: number;
    snapshotRebuildCount: number;
  };
  __donnerFrameTickStats?: unknown;
  __donnerMemoryStats?: unknown;
  __donnerImGuiDrawStats?: unknown;
  __donnerResponsivenessInput?: { type: string; atMs: number; x: number; y: number };
  __donnerResponsivenessEvents?: unknown[];
  __donnerPresentationQueueStats?: {
    completedSerial: number;
    submittedSerial: number;
    framesInFlight: number;
    coalescedFrames: number;
    frameId: number;
    captureId: number;
    pointerX: number;
    pointerY: number;
    mouseDown: boolean;
    inputRepresented: boolean;
    viewportZoom: number;
    completedAtMs: number;
  };
  __donnerGpuObjectStats?: unknown;
  __donnerOverlayStats?: unknown;
  __donnerPresentationRepairStats?: unknown;
  __donnerLastDragStream?: unknown;
  __donnerWorkerResultCounters?: WorkerResultCounters[];
}

/**
 * One published worker result's compositor and browser GPU owner counters. A bundle that predates
 * them publishes none: `countersPublished` is then false and the counts are null.
 */
interface WorkerResultCounters {
  completedResults: number;
  countersPublished: boolean;
  activeDragResult: boolean | null;
  fastPathFrames: number;
  slowPathFramesWithDirty: number;
  noDirtyFrames: number;
  damagePatchTileCount: number;
  maskedChildComposeTileCount: number;
  cachedTileCount: number;
  immediateTileCount: number;
  offscreenRecycleCount: number;
  mainComposeCount: number;
  gpuOwnerDispatchCount: number;
  gpuOwnerWaitSliceTimeouts: number;
  gpuOwnerDispatchWaitMs: number;
  /** Origin, kinds and blocked-time phases of this result's owner dispatches, as published. */
  gpuOwnerDispatch: unknown;
  compositorRenderCount: number;
  workerMs: number;
  renderFrameMs: number;
  /** The result's most recent interaction-layer promotion request, as published, or null. */
  interactionPromotion: InteractionPromotion | null;
}

/** One worker result's interaction-layer promotion request and its admission inputs. */
interface InteractionPromotion {
  requested: boolean;
  result: string | null;
  retainedBitmapBytes: number;
  memoryBudgetBytes: number;
}

async function attachJson(info: import("@playwright/test").TestInfo, name: string, data: unknown) {
  const output = info.outputPath(name);
  fs.writeFileSync(output, JSON.stringify(data));
  await info.attach(name, { path: output, contentType: "application/json" });
}

async function rasterIdentity(page: Page) {
  return page.evaluate(() => {
    const canvas = document.querySelector<HTMLCanvasElement>("canvas#canvas")!;
    return {
      dpr: devicePixelRatio,
      backingWidth: canvas.width,
      backingHeight: canvas.height,
      cssWidth: canvas.clientWidth,
      cssHeight: canvas.clientHeight,
    };
  });
}

async function snapshot(page: Page, detailed = false) {
  return page.evaluate((detailed) => {
    const state = window as Diagnostics;
    return {
      backend: state.__donnerBackend,
      frameLoop: state.__donnerFrameLoopStats,
      host: state.__donnerHostFrameTiming,
      suspend: state.__donnerAsyncifySuspendStats,
      interaction: state.__donnerInteractionStats,
      viewport: state.__donnerViewportStats,
      worker: state.__donnerWorkerStats,
      documentPresentation: state.__donnerPresentedDocumentStats,
      presentation: state.__donnerPresentationQueueStats,
      gpuObjects: state.__donnerGpuObjectStats,
      overlay: state.__donnerOverlayStats,
      repair: state.__donnerPresentationRepairStats,
      lastDragStream: detailed ? state.__donnerLastDragStream : undefined,
      thumbnails: state.__donnerSampleThumbnailStats,
      layerThumbnails: state.__donnerLayerThumbnailStats,
      ticks: state.__donnerFrameTickStats,
      memory: detailed ? state.__donnerMemoryStats : undefined,
      draw: state.__donnerImGuiDrawStats,
      input: state.__donnerResponsivenessInput,
      inputEvents: detailed ? state.__donnerResponsivenessEvents : undefined,
    };
  }, detailed);
}

type Snapshot = Awaited<ReturnType<typeof snapshot>>;

function distribution(values: number[]) {
  const sorted = [...values].sort((a, b) => a - b);
  const percentile = (fraction: number) =>
    sorted[
      Math.min(
        sorted.length - 1,
        Math.floor(sorted.length * fraction),
      )
    ] ?? null;
  return { count: sorted.length, p50: percentile(0.5), p95: percentile(0.95), max: sorted.at(-1) };
}

function phaseReport(before: Snapshot, after: Snapshot, inputToFrameMs: number[]) {
  const frames = (after.host?.frames ?? 0) - (before.host?.frames ?? 0);
  return {
    inputToFrameMs: distribution(inputToFrameMs),
    uiFrameMs: distribution(
      after.frameLoop?.uiFrameMsSamples.slice(before.frameLoop?.uiFrameMsSamples.length ?? 0) ?? [],
    ),
    uiPhaseMeanMs: Object.fromEntries(
      Object.entries(after.frameLoop?.phaseTotalsMs ?? {}).map(([name, total]) => [
        name,
        (total - (before.frameLoop?.phaseTotalsMs?.[name] ?? 0)) / Math.max(frames, 1),
      ]),
    ),
    layerThumbnails: after.layerThumbnails,
    hostMeanMs: Object.fromEntries(
      Object.entries(after.host?.sums ?? {}).map(([name, total]) => [
        name,
        (total - (before.host?.sums[name] ?? 0)) / Math.max(frames, 1),
      ]),
    ),
    suspendDelta: Object.fromEntries(
      ([
        "frames",
        "suspends",
        "totalMs",
        "tileYieldMs",
        "gpuReadbackMs",
        "deviceWaitMs",
        "startupMs",
      ] as const).flatMap((name) => {
        const total = after.suspend?.[name];
        return typeof total === "number" ? [[name, total - (before.suspend?.[name] ?? 0)]] : [];
      }),
    ),
    frames,
    workerResults: (after.worker?.completedResults ?? 0) - (before.worker?.completedResults ?? 0),
    lastWorker: after.worker,
    ticks: after.ticks,
  };
}

async function waitForIdle(page: Page) {
  // Historical baseline bundles predate queued-work telemetry. Candidates must explicitly report
  // both flags false: an idle worker alone can still be waiting for the zoom debounce deadline.
  const expected = process.env.DONNER_BROWSER_BASELINE === "1"
    ? { pendingClick: false, workerBusy: false }
    : {
      pendingClick: false,
      workerBusy: false,
      renderRequestPending: false,
      viewportSettlePending: false,
    };
  await expect.poll(async () => (await snapshot(page)).interaction, {
    message: "the editor must consume pending input and finish queued and active viewport work",
    timeout: 15000,
  }).toEqual(expect.objectContaining(expected));
}

// Opening a sample is observable before the editor displays it: the sample attribute changes, and
// the editor can report idle while it still publishes the previous document's viewport and has not
// started the new document's first render. A click then races that render and can select nothing
// (#1683). So this waits until the published viewport is the splash, and then until the editor is
// idle on it.
async function waitForSplashDisplayed(page: Page) {
  await expect.poll(async () => showsSplashDocument((await snapshot(page)).viewport), {
    timeout: 15000,
    message: "the editor must display the Donner splash",
  }).toBe(true);
  await waitForIdle(page);
}

// The aim maps a document point through the published viewport, so it waits until that viewport
// describes the splash (see splash-aim.mjs), and fails rather than clicks if applying the move
// changed the layout under it.
async function aimAtSplashD(page: Page) {
  await expect.poll(async () => showsSplashDocument((await snapshot(page)).viewport), {
    timeout: 15000,
    message: "the editor must display the Donner splash before the D can be aimed at",
  }).toBe(true);
  const viewport = (await snapshot(page)).viewport!;
  const point = splashDPoint(viewport);
  expect(point.x).toBeGreaterThan(viewport.paneX);
  expect(point.x).toBeLessThan(viewport.paneX + viewport.paneWidth);
  expect(point.y).toBeGreaterThan(viewport.paneY);
  expect(point.y).toBeLessThan(viewport.paneY + viewport.paneHeight);
  await dispatchCanvasPointer(page, point);
  await expect.poll(async () => (await snapshot(page)).interaction, {
    timeout: 10000,
    message: "the D aiming move must be applied before a press or zoom",
  }).toEqual(expect.objectContaining({ pointerX: point.x, pointerY: point.y }));
  const applied = (await snapshot(page)).viewport!;
  expect(
    splashDPoint(applied),
    `the layout moved under the D aim: ${JSON.stringify({ aimedAt: viewport, applied })}`,
  ).toEqual(point);
  return point;
}

async function zoomSplashForDrag(page: Page, targetZoom = 1.93) {
  for (let step = 0; step < 12; ++step) {
    const viewport = (await snapshot(page)).viewport!;
    if (Math.abs(viewport.zoom - targetZoom) < 0.001) break;
    const point = await aimAtSplashD(page);
    const deltaY = -100 * Math.max(-0.25, Math.min(0.25, Math.log(targetZoom / viewport.zoom)));
    await page.evaluate(({ x, y, deltaY }) => {
      document.querySelector("canvas#canvas")!.dispatchEvent(
        new WheelEvent("wheel", {
          bubbles: true,
          cancelable: true,
          clientX: x,
          clientY: y,
          ctrlKey: true,
          deltaMode: 0,
          deltaY,
        }),
      );
    }, { ...point, deltaY });
    await expect.poll(async () => (await snapshot(page)).viewport?.zoom, {
      timeout: 10000,
      message: "the normal pinch input must update the viewport",
    }).not.toBe(viewport.zoom);
  }
  expect((await snapshot(page)).viewport!.zoom).toBeCloseTo(targetZoom, 2);
  await waitForIdle(page);
  return aimAtSplashD(page);
}

async function zoomAtDraggedShape(page: Page, point: { x: number; y: number }, targetZoom: number) {
  const before = (await snapshot(page)).viewport!;
  await page.evaluate(({ point, deltaY }) => {
    document.querySelector("canvas#canvas")!.dispatchEvent(
      new WheelEvent("wheel", {
        bubbles: true,
        cancelable: true,
        clientX: point.x,
        clientY: point.y,
        ctrlKey: true,
        deltaMode: 0,
        deltaY,
      }),
    );
  }, { point, deltaY: -100 * Math.log(targetZoom / before.zoom) });
  await expect.poll(async () => (await snapshot(page)).viewport?.zoom, {
    timeout: 5000,
    message: "zoom must be applied before the immediate second press",
  }).toBeCloseTo(targetZoom, 2);
}

const latencyGateEnforcement = latencyGateMode();

/** Record one interaction latency gate, failing the test on a miss unless the run only reports. */
function latencyGate(
  gate: keyof typeof kLatencyGates,
  phase: string,
  valueMs: number | null | undefined,
) {
  checkLatencyGate(gate, valueMs, {
    browser: test.info().project.name,
    phase,
    mode: latencyGateEnforcement,
    enforce: (value, limitMs, message) => expect.soft(value, message).toBeLessThanOrEqual(limitMs),
    log: (line) => console.log(line),
  });
}

const completionCheckEnforcement = completionCheckMode();

/**
 * Run one tracked completion check's assertion and record whether it held. A miss fails the test
 * unless the run only reports completion checks and the miss is in the check's tracked browser
 * and, when the check names them, its tracked phases.
 */
async function completionCheck(
  check: keyof typeof kCompletionChecks,
  phase: string,
  detail: string,
  assertion: () => unknown,
) {
  const project = test.info().project;
  await runCompletionCheck(check, assertion, {
    browserName: project.use.browserName ?? "",
    project: project.name,
    phase,
    detail,
    mode: completionCheckEnforcement,
    log: (line) => console.log(line),
  });
}

/** Stream one drag from \p start and wait until its release reaches an idle editor. */
async function streamDragToIdle(page: Page, start: { x: number; y: number }, direction: number) {
  const stream = await page.evaluate(streamDragInputs, {
    start,
    direction,
    presentationSamplePollMs: kPresentationSamplePollMs,
    releaseFrameTimeoutMs: kReleaseFrameTimeoutMs,
  });
  await expect.poll(async () => (await snapshot(page)).interaction, {
    timeout: 15000,
    message: "the final pointer move and mouse release must reach the editor",
  }).toEqual(expect.objectContaining({
    pointerX: stream.end.x,
    pointerY: stream.end.y,
    dragging: false,
    pendingClick: false,
  }));
  await waitForIdle(page);
  return stream;
}

async function continuousDrag(
  page: Page,
  start: { x: number; y: number },
  direction: number,
  phase: string,
) {
  const before = await snapshot(page);
  const stream = await streamDragToIdle(page, start, direction);
  const after = await snapshot(page);
  const baselineOnly = process.env.DONNER_BROWSER_BASELINE === "1";
  if (!baselineOnly) {
    expect(after.presentation, "candidate completion telemetry is required").toBeTruthy();
  }
  if (!baselineOnly && after.presentation) {
    // The release has one stall guard, measured from the mouse up, and the sampler observed the
    // drag until it. A release frame later than that fails here, before the checks below read
    // which inputs the stream completed.
    expect(stream.releaseFrame, "release must reach a current paired frame at the requested camera")
      .toBeTruthy();
    // Once idle, the presented frame must still be that release at the current camera: a later
    // frame must not have reverted the pointer or the camera. Frames submitted by then may still
    // be in flight, and the last one to complete can be a frame that kept showing the previous
    // presentation while a new one was prepared, which does not claim the current input. Every
    // such frame must be superseded by one that does, so the check reads the presented frame
    // once all of them have completed.
    await expect.poll(async () => (await snapshot(page)).presentation?.completedSerial ?? 0, {
      timeout: 15000,
      message: "every frame submitted by idle must complete",
    }).toBeGreaterThanOrEqual(after.presentation.submittedSerial);
    const settled = (await snapshot(page)).presentation!;
    expect(
      settled.inputRepresented && !settled.mouseDown && settled.pointerX === stream.end.x
        && settled.pointerY === stream.end.y
        && Math.abs(settled.viewportZoom - after.viewport!.zoom) < 0.001,
      "the presented frame after idle must still be the release at the requested camera",
    ).toBe(true);
  }
  expect(after.interaction?.selectedCount, "the D drag must leave one shape selected").toBe(1);
  expect(stream.frames.some((frame) => frame.dragging), "the stream must enter drag mode").toBe(
    true,
  );
  const gaps = (times: number[]) => times.slice(1).map((time, index) => time - times[index]);
  const baselineSerial = before.presentation?.completedSerial ?? 0;
  const latencies = dragInputLatencies(stream, baselineSerial);
  const completedFrames = completedDragFrames(stream, baselineSerial);
  if (!baselineOnly) {
    const stampedHeldPositions = heldDragPositionCount(completedFrames);
    const crown = phase === "geode-crown";
    const heldPositions = crown
      ? physicallyHeldDragPositionCount(stream, completedFrames)
      : stampedHeldPositions;
    const heldDetail = crown
      ? `${heldPositions} represented positions completed before release (${stampedHeldPositions} stamped held)`
      : `${heldPositions} held positions completed`;
    // The hosted macOS Perf job reports, rather than fails, a miss in Chromium in the masked crown
    // drag (#2106); it enforces Firefox and every other phase, as does every run that does not set
    // report mode.
    await completionCheck(
      "heldDragPositions",
      phase,
      heldDetail,
      () =>
        expect(heldPositions, "held drag must complete multiple represented positions")
          .toBeGreaterThan(1),
    );
    await completionCheck(
      "dragInputsComplete",
      phase,
      `${latencies.length} of ${stream.dispatchPoints.length} inputs completed`,
      () =>
        expect(
          latencies.length,
          "every dispatched input, including the final one, must complete in the measured stream",
        ).toBe(stream.dispatchPoints.length),
    );
    expect(after.worker?.compositorReadbackCount, "composition must not read GPU tiles back")
      .toBe(0);
    latencyGate("inputP95", phase, distribution(latencies).p95);
    latencyGate("inputStall", phase, distribution(latencies).max);
    const cadence = gaps(completedFrames.map((frame) => frame.presentation!.completedAtMs));
    latencyGate("completedStall", phase, distribution(cadence).max);
  }
  return {
    ...phaseReport(before, after, []),
    // These are observed frame gaps, not a latched input-to-presentation latency measurement.
    observedFrameGapsMs: distribution(gaps(stream.frames.map((frame) => frame.atMs))),
    inputDispatchGapsMs: distribution(gaps(stream.dispatchTimes)),
    observedFrames: stream.frames.length,
    presentedInputCount: latencies.length,
    inputToGpuCompletedFrameMs: distribution(latencies),
    completedFrameGapsMs: distribution(
      gaps(completedFrames.map((frame) => frame.presentation!.completedAtMs)),
    ),
    stream,
  };
}

// This manual lane measures production frame telemetry. Pixel reads, screenshots and traces
// serialize the GPU in Firefox and must stay outside the measured interaction windows.
test(
  "reports input-to-frame latency with matching raster load",
  async ({ page, browser }, info) => {
    const failures: string[] = [];
    page.on("pageerror", (error) => failures.push(error.message));
    page.on("console", (message) => {
      if (/Aborted|RuntimeError|UTILS_RELEASE_ASSERT|device lost/i.test(message.text())) {
        failures.push(message.text());
      }
    });
    await page.addInitScript(() => {
      (window as Diagnostics).__donnerResponsivenessEvents = [];
      for (const type of ["mousemove", "mousedown", "mouseup", "wheel"]) {
        window.addEventListener(type, (event) => {
          const pointer = event as MouseEvent;
          const state = window as Diagnostics;
          if (type !== "mouseup") {
            state.__donnerResponsivenessInput = {
              type,
              atMs: performance.now(),
              x: pointer.clientX,
              y: pointer.clientY,
            };
          }
          const events = state.__donnerResponsivenessEvents;
          if (events && events.length < 32) {
            events.push({
              type,
              clientX: pointer.clientX,
              clientY: pointer.clientY,
              screenX: pointer.screenX,
              screenY: pointer.screenY,
              target: (event.target as Element | null)?.id,
              applied: state.__donnerInteractionStats,
              atMs: performance.now(),
            });
          }
        }, { capture: true, passive: true });
      }
    });
    let stopHeartbeat: (() => void) | undefined;
    const report: Record<string, unknown> = {
      browser: info.project.name,
      version: browser.version(),
      ...packageHashes(),
      headless: info.project.use.headless,
    };
    try {
      await page.goto(process.env.DONNER_WASM_BASE_URL!, { waitUntil: "domcontentloaded" });
      await expect.poll(
        () => page.evaluate(() => (window as Diagnostics).__donnerFirstFramePresented),
        {
          timeout: 30000,
          message: "WebGPU must present the production editor before timing interactions",
        },
      ).toBe(true);
      stopHeartbeat = await startApplicationHeartbeat(page);
      report.clock = await page.evaluate(async () => {
        const samples: number[] = [];
        let previous = performance.now();
        for (let i = 0; i < 12; ++i) {
          const now = await new Promise<number>((resolve) => requestAnimationFrame(resolve));
          samples.push(now - previous);
          previous = now;
        }
        return {
          visibility: document.visibilityState,
          driver: (window as unknown as { __donnerFrameDriver?: string }).__donnerFrameDriver,
          mainAnimationFrameIntervalsMs: samples,
        };
      });
      const raster = await page.evaluate(() => {
        const canvas = document.querySelector<HTMLCanvasElement>("canvas#canvas")!;
        return {
          dpr: devicePixelRatio,
          backingWidth: canvas.width,
          backingHeight: canvas.height,
          cssWidth: canvas.clientWidth,
          cssHeight: canvas.clientHeight,
        };
      });
      report.raster = raster;
      console.log(`responsiveness raster ${info.project.name} ${JSON.stringify(raster)}`);
      const expectedDpr = Number(info.project.use.deviceScaleFactor ?? 1);
      expect(raster).toEqual({
        dpr: expectedDpr,
        backingWidth: 1280 * expectedDpr,
        backingHeight: 900 * expectedDpr,
        cssWidth: 1280,
        cssHeight: 900,
      });
      const beforeSample = await snapshot(page);
      const canvas = page.locator("canvas#canvas");
      const bounds = await canvas.boundingBox();
      expect(bounds).not.toBeNull();
      report.sampleBounds = bounds;
      await dispatchCanvasPointer(
        page,
        { x: bounds!.x + bounds!.width * 0.24, y: bounds!.y + 282 },
        true,
      );
      await expect(canvas).toHaveAttribute("data-active-sample-id", "donner-splash");
      await expect.poll(async () => {
        const state = await snapshot(page);
        return (state.worker?.completedResults ?? 0) > (beforeSample.worker?.completedResults ?? 0)
          && state.worker?.presentedAtMs !== undefined;
      }, { timeout: 20000, message: "the clicked sample must reach a presenting frame" }).toBe(
        true,
      );
      report.sampleInputEvents = await page.evaluate(() => {
        const state = window as Diagnostics;
        const events = state.__donnerResponsivenessEvents;
        delete state.__donnerResponsivenessEvents;
        return events;
      });
      const loaded = await snapshot(page);
      const worker = loaded.worker!;
      const inputAtMs = loaded.input!.atMs;
      report.sample = {
        ...phaseReport(beforeSample, loaded, [worker.presentedAtMs! - inputAtMs]),
        workerQueueMs: worker.queueWaitMs,
        dequeueToStartMs: worker.dequeueToStartMs,
        workerMs: worker.workerMs,
        completionToPollMs: worker.pollDelayMs,
        pollToFrameMs: worker.presentedAtMs! - worker.publishedAtMs,
      };
      await waitForIdle(page);
      const viewport = (await snapshot(page)).viewport!;
      const center = {
        x: Math.round(viewport.paneX + viewport.paneWidth * 0.5),
        y: Math.round(viewport.paneY + viewport.paneHeight * 0.5),
      };
      for (
        const { phase, kind } of [
          { phase: "cold-pointer", kind: "pointer" },
          { phase: "cold-zoom", kind: "zoom" },
          { phase: "warm-pointer", kind: "pointer" },
          { phase: "warm-zoom", kind: "zoom" },
        ] as const
      ) {
        if (process.env.DONNER_BROWSER_REPAIR_PROBE === "1") break;
        if (phase.startsWith("warm-")) {
          await expect.poll(async () => (await snapshot(page)).thumbnails, {
            timeout: 15000,
            message: "warm input must start after thumbnail work settles",
          }).toEqual(
            expect.objectContaining({ pending: false, active: false, resultReady: false }),
          );
          await expect.poll(async () => (await snapshot(page)).layerThumbnails?.deferredCount, {
            timeout: 15000,
            message: "warm input must start after layer previews settle",
          }).toBe(0);
        }
        const before = await snapshot(page);
        const latencies: number[] = [];
        for (let step = 0; step < 12; ++step) {
          const prior = await snapshot(page);
          const point = { x: center.x + step * 3, y: center.y + step * 2 };
          if (kind === "pointer") {
            await dispatchCanvasPointer(page, point);
          } else {
            await page.evaluate(({ x, y, deltaY }) => {
              document.querySelector("canvas#canvas")!.dispatchEvent(
                new WheelEvent("wheel", {
                  bubbles: true,
                  cancelable: true,
                  clientX: x,
                  clientY: y,
                  ctrlKey: true,
                  deltaMode: 0,
                  deltaY,
                }),
              );
            }, { ...center, deltaY: step % 2 === 0 ? -42 : 42 });
          }
          let presented: Snapshot | undefined;
          const stepPresents = () =>
            expect.poll(async () => {
              const current = await snapshot(page);
              const changed = kind === "pointer"
                ? current.interaction?.pointerX === point.x
                  && current.interaction?.pointerY === point.y
                : current.viewport?.zoom !== prior.viewport?.zoom;
              const ready = changed
                && (current.frameLoop?.renderedFrames ?? 0)
                  > (prior.frameLoop?.renderedFrames ?? 0)
                && (current.frameLoop?.lastFrameAtMs ?? 0) >= (current.input?.atMs ?? Infinity);
              if (ready) presented = current;
              return ready;
            }, {
              timeout: 10000,
              intervals: [8, 16, 32],
              message: `${phase} step ${step} must present`,
            })
              .toBe(true);
          if (phase === "cold-zoom") {
            await completionCheck("coldZoomPresents", phase, `step ${step}`, stepPresents);
          } else {
            await stepPresents();
          }
          // A reported miss leaves this step without a presented frame to measure.
          if (presented) latencies.push(presented.frameLoop!.lastFrameAtMs - presented.input!.atMs);
        }
        report[phase] = phaseReport(before, await snapshot(page), latencies);
        console.log(
          `responsiveness ${phase} ${info.project.name} ${JSON.stringify(report[phase])}`,
        );
        await waitForIdle(page);
      }
      const dragStart = await zoomSplashForDrag(page);
      const beforeClick = await snapshot(page);
      await dispatchCanvasPointer(page, dragStart, true);
      let clicked: Snapshot | undefined;
      await expect.poll(async () => {
        const current = await snapshot(page);
        const candidate = process.env.DONNER_BROWSER_BASELINE !== "1";
        const completion = current.presentation;
        const ready = current.interaction?.selectedCount === 1
          && (!candidate || (completion?.inputRepresented
            && completion.frameId > (beforeClick.presentation?.frameId ?? 0)
            && completion.pointerX === dragStart.x && completion.pointerY === dragStart.y));
        if (ready) clicked = current;
        return Boolean(ready);
      }, { timeout: 5000, message: "settled D click must reach a paired presented selection" })
        .toBe(true);
      if (clicked?.presentation && process.env.DONNER_BROWSER_BASELINE !== "1") {
        report.settledClickFeedbackMs = clicked.presentation.completedAtMs - clicked.input!.atMs;
        latencyGate("settledClick", "settled-click", report.settledClickFeedbackMs as number);
      }
      // Separate the selection click from the drag so it cannot become a group-isolation double click.
      await page.waitForTimeout(400);
      const firstDrag = await continuousDrag(page, dragStart, -1, "first-drag-193");
      report["first-drag-193"] = firstDrag;
      console.log(
        `responsiveness first-drag-193 ${info.project.name} ${JSON.stringify(firstDrag)}`,
      );
      const firstCapture = info.outputPath("first-drag-193.png");
      await page.screenshot({ path: firstCapture });
      await info.attach("first-drag-193.png", {
        path: firstCapture,
        contentType: "image/png",
      });
      await zoomAtDraggedShape(page, firstDrag.stream.end, 2.17);
      report.zoomBeforeSecondDrag = await snapshot(page);
      const secondDrag = await continuousDrag(page, firstDrag.stream.end, 1, "second-drag-193");
      report["second-drag-193"] = secondDrag;
      console.log(
        `responsiveness second-drag-193 ${info.project.name} ${JSON.stringify(secondDrag)}`,
      );
      const secondCapture = info.outputPath("second-drag-193.png");
      await page.screenshot({ path: secondCapture });
      await info.attach("second-drag-193.png", {
        path: secondCapture,
        contentType: "image/png",
      });
      await zoomAtDraggedShape(page, secondDrag.stream.end, 1.93);
      await waitForIdle(page);
      report["settled-zoom-third-drag"] = await continuousDrag(
        page,
        secondDrag.stream.end,
        -1,
        "settled-zoom-third-drag",
      );
      expect(failures).toEqual([]);
    } finally {
      stopHeartbeat?.();
      let deadline: ReturnType<typeof setTimeout> | undefined;
      report.last = await Promise.race([
        snapshot(page, true),
        new Promise((resolve) => {
          deadline = setTimeout(() => resolve({ diagnosticTimedOut: true }), 5000);
        }),
      ]).catch(() => null);
      clearTimeout(deadline);
      report.failures = failures;
      console.log(`responsiveness ${JSON.stringify(report)}`);
      await attachJson(info, "responsiveness.json", report);
    }
  },
);

interface GpuCounts {
  objects: number;
  textures: number;
  textureBytes: number;
  bufferBytes: number;
  pendingSubmissions: number;
  shares: number;
  sharedTextureTailBytes: number;
}
function monitoredMemory() {
  const report = JSON.parse(
    fs.readFileSync(`${process.env.TEST_UNDECLARED_OUTPUTS_DIR}/browser-watchdog.json`, "utf8"),
  );
  const rssBytes = report.samples.at(-1)?.rssBytes;
  expect(typeof rssBytes).toBe("number");
  const heartbeat = JSON.parse(fs.readFileSync(process.env.DONNER_WATCHDOG_HEARTBEAT!, "utf8"));
  return {
    rssBytes: rssBytes as number,
    gpu: heartbeat.gpu as GpuCounts,
    workers: heartbeat.workers as {
      index: number;
      gpu: GpuCounts | null;
      ageMs?: number;
      unavailable?: boolean;
    }[],
  };
}

async function waitForResourceIdle(page: Page) {
  let previousSignature = "";
  let unchangedSince = Date.now();
  await expect.poll(async () => {
    const state = await snapshot(page);
    const memory = monitoredMemory();
    const known = memory.workers.filter((owner) => owner.gpu);
    const quiescent = !state.interaction?.workerBusy
      && resourceQueuesAreQuiescent(memory.workers, state.presentation?.framesInFlight);
    const signature = JSON.stringify({
      renderedFrames: state.frameLoop!.renderedFrames,
      workers: known.map(({ index, gpu }) => ({ index, gpu })),
    });
    if (!quiescent || signature !== previousSignature) {
      previousSignature = signature;
      unchangedSince = Date.now();
      return false;
    }
    return Date.now() - unchangedSince >= 2000;
  }, {
    timeout: 15000,
    intervals: [1000],
    message: "all owned resource queues and idle frames must settle before the dwell",
  }).toBe(true);
}
function expectGpuPlateau(current: GpuCounts, baseline: GpuCounts) {
  for (
    const field of [
      "objects",
      "textures",
      "textureBytes",
      "bufferBytes",
      "shares",
      "sharedTextureTailBytes",
    ] as const
  ) {
    expect(current[field], `${field} must remain bounded after warm-up`).toBeLessThanOrEqual(
      baseline[field],
    );
  }
  expect(current.pendingSubmissions).toBe(0);
}

test(
  "browser resources stay bounded after repeated drag zoom drag",
  async ({ page, browser }, info) => {
    test.skip(
      process.env.DONNER_BROWSER_SOAK !== "1",
      "five-minute qualification is an explicit manual workload",
    );
    test.setTimeout(420000);
    await page.goto(process.env.DONNER_WASM_BASE_URL!, { waitUntil: "domcontentloaded" });
    await expect.poll(
      () => page.evaluate(() => (window as Diagnostics).__donnerFirstFramePresented),
      { timeout: 30000 },
    ).toBe(true);
    const stopHeartbeat = await startApplicationHeartbeat(page);
    const identity = {
      browser: info.project.name,
      version: browser.version(),
      headless: info.project.use.headless,
      ...packageHashes(),
      raster: await rasterIdentity(page),
      backend: (await snapshot(page)).backend,
    };
    const samples: unknown[] = [];
    try {
      await dispatchCanvasPointer(page, { x: 1280 * 0.24, y: 282 }, true);
      await expect(page.locator("canvas#canvas")).toHaveAttribute(
        "data-active-sample-id",
        "donner-splash",
      );
      await waitForSplashDisplayed(page);
      let point = await zoomSplashForDrag(page);
      const beforeSelection = await snapshot(page);
      await dispatchCanvasPointer(page, point, true);
      await expect.poll(async () => {
        const state = await snapshot(page);
        return state.interaction?.selectedCount === 1 && state.presentation?.inputRepresented
          && state.presentation.frameId > (beforeSelection.presentation?.frameId ?? 0)
          && state.presentation.pointerX === point.x && state.presentation.pointerY === point.y;
      }, { timeout: 5000, message: "select D before the warmed drag-zoom-drag sequence" }).toBe(
        true,
      );
      await waitForIdle(page);
      // Keep the selection click separate from the drag press, outside measured intervals.
      await page.waitForTimeout(400);
      for (let cycle = 0; cycle < 10; ++cycle) {
        const first = await continuousDrag(page, point, -1, `cycle-${cycle}-first-drag`);
        await zoomAtDraggedShape(page, first.stream.end, 2.17);
        const second = await continuousDrag(
          page,
          first.stream.end,
          1,
          `cycle-${cycle}-second-drag`,
        );
        point = second.stream.end;
        await zoomAtDraggedShape(page, point, 1.93);
        await waitForIdle(page);
        const completion = (await snapshot(page)).presentation;
        if (completion) expect(completion.framesInFlight).toBeLessThanOrEqual(3);
      }
      await waitForResourceIdle(page);
      const baseline = monitoredMemory();
      expect(baseline.gpu).not.toBeNull();
      const idleFrames = (await snapshot(page)).frameLoop!.renderedFrames;
      for (let second = 5; second <= 300; second += 5) {
        await page.waitForTimeout(5000);
        const memory = monitoredMemory();
        samples.push({ second, ...memory });
        expect(memory.rssBytes - baseline.rssBytes, "post-warm-up process RSS must plateau")
          .toBeLessThan(64 * 1024 * 1024);
        expect(
          memory.gpu.textureBytes + memory.gpu.sharedTextureTailBytes,
          "texture allocations and exported tails must remain bounded",
        ).toBeLessThanOrEqual(
          baseline.gpu.textureBytes + baseline.gpu.sharedTextureTailBytes,
        );
        expectGpuPlateau(memory.gpu, baseline.gpu);
        expect(memory.workers.length, "worker ownership samples are required").toBeGreaterThan(0);
        for (const known of baseline.workers.filter((owner) => owner.gpu)) {
          const current = memory.workers.find((owner) => owner.index === known.index);
          expect(current?.gpu, "a known GPU owner must retain diagnostic visibility").toBeTruthy();
        }
        for (const owner of memory.workers) {
          expect(owner.unavailable, "an unavailable owner cannot establish a GPU plateau").not.toBe(
            true,
          );
          expect(owner.ageMs, "owner statistics must remain fresh").toBeLessThan(5000);
          if (!owner.gpu) continue;
          const prior = baseline.workers.find((worker) => worker.index === owner.index)?.gpu;
          expect(prior, "the baseline must identify every GPU owner").toBeTruthy();
          expect(owner.gpu.textureBytes + owner.gpu.sharedTextureTailBytes)
            .toBeLessThanOrEqual(prior!.textureBytes + prior!.sharedTextureTailBytes);
          expectGpuPlateau(owner.gpu, prior!);
        }
      }
      expect(
        (await snapshot(page)).frameLoop!.renderedFrames - idleFrames,
        "the idle editor must park",
      ).toBeLessThanOrEqual(2);
    } finally {
      stopHeartbeat();
      await attachJson(info, "memory-soak.json", { identity, samples });
      let diagnosticTimer: ReturnType<typeof setTimeout> | undefined;
      let last: unknown;
      try {
        last = await Promise.race([
          snapshot(page, true),
          new Promise((resolve) => {
            diagnosticTimer = setTimeout(
              () => resolve({ unavailable: true, reason: "snapshot deadline" }),
              1000,
            );
          }),
        ]);
      } catch {
        last = { unavailable: true, reason: "page closed or snapshot failed" };
      } finally {
        clearTimeout(diagnosticTimer);
      }
      await attachJson(info, "memory-soak-final-state.json", last);
    }
  },
);

test(
  "parked renderer keeps resource completion and ownership observable",
  async ({ page, browser }, info) => {
    test.setTimeout(60000);
    await page.goto(process.env.DONNER_WASM_BASE_URL!, { waitUntil: "domcontentloaded" });
    await expect.poll(
      () => page.evaluate(() => (window as Diagnostics).__donnerFirstFramePresented),
      { timeout: 30000 },
    ).toBe(true);
    const stopHeartbeat = await startApplicationHeartbeat(page);
    const identity = {
      browser: info.project.name,
      version: browser.version(),
      headless: info.project.use.headless,
      ...packageHashes(),
      raster: await rasterIdentity(page),
      backend: (await snapshot(page)).backend,
    };
    const samples = [];
    try {
      await dispatchCanvasPointer(page, { x: 1280 * 0.24, y: 282 }, true);
      await expect(page.locator("canvas#canvas")).toHaveAttribute(
        "data-active-sample-id",
        "donner-splash",
      );
      await waitForSplashDisplayed(page);
      await waitForResourceIdle(page);
      const baseline = monitoredMemory();
      const idleFrames = (await snapshot(page)).frameLoop!.renderedFrames;
      expect(baseline.workers.some((owner) => owner.gpu)).toBe(true);
      for (let second = 2; second <= 12; second += 2) {
        await page.waitForTimeout(2000);
        const memory = monitoredMemory();
        samples.push({ second, ...memory });
        for (const known of baseline.workers.filter((owner) => owner.gpu)) {
          const owner = memory.workers.find((worker) => worker.index === known.index);
          expect.soft(owner?.unavailable, "an idle GPU owner must remain observable").not.toBe(
            true,
          );
          expect.soft(owner?.ageMs, "resource completion must run while the renderer is parked")
            .toBeLessThan(5000);
          expect.soft(owner?.gpu?.pendingSubmissions, "idle submissions must complete").toBe(0);
        }
      }
      expect(
        (await snapshot(page)).frameLoop!.renderedFrames - idleFrames,
        "idle maintenance must not render frames",
      ).toBeLessThanOrEqual(2);
    } finally {
      stopHeartbeat();
      await attachJson(info, "parked-resource-owners.json", { identity, samples });
    }
  },
);

test("GPU image transfer preserves pixels between workers", async ({ page, browser }, info) => {
  await page.goto(process.env.DONNER_WASM_BASE_URL!, { waitUntil: "domcontentloaded" });
  await expect.poll(
    () => page.evaluate(() => (window as Diagnostics).__donnerFirstFramePresented),
    { timeout: 30000 },
  ).toBe(true);
  const stopHeartbeat = await startApplicationHeartbeat(page);
  try {
    const completionProgress = process.env.DONNER_GPU_COMPLETION_PROGRESS === "1";
    const samples = await inspectGpuImageTransfer(
      page,
      info.outputPath("gpu-image-transfer"),
      completionProgress,
    );
    await attachJson(info, "gpu-image-transfer.json", {
      browser: info.project.name,
      version: browser.version(),
      ...packageHashes(),
      completionProgress,
      samples,
    });
  } finally {
    stopHeartbeat();
  }
});

type DelayedCompletionState = {
  calls: number;
  realCompletions: number;
  released: boolean;
  firstHoldAtMs: number | null;
  lastRealCompletionAtMs: number | null;
};
type CompletionWorker = typeof globalThis & {
  __donnerApplicationGpuSurfaceWorker?: boolean;
  __donnerDelayedUiCompletion?: DelayedCompletionState;
  __donnerReleaseUiCompletion?: () => void;
};

test(
  "delayed UI completion callbacks do not permanently disable presentation",
  async ({ page, browser }, info) => {
    await page.goto(process.env.DONNER_WASM_BASE_URL!, { waitUntil: "domcontentloaded" });
    await expect.poll(
      () => page.evaluate(() => (window as Diagnostics).__donnerFirstFramePresented),
      { timeout: 30000 },
    ).toBe(true);
    const stopHeartbeat = await startApplicationHeartbeat(page);
    let application: ReturnType<Page["workers"]>[number] | undefined;
    const report: Record<string, unknown> = {
      browser: info.project.name,
      version: browser.version(),
      ...packageHashes(),
    };
    try {
      await expect.poll(async () => pickerPresentationSettled(await snapshot(page)), {
        timeout: 20000,
        message: "sample thumbnails must be published and presentation idle",
      }).toBe(true);
      application = await Promise.any(
        page.workers().map(async (worker) => {
          let timer: ReturnType<typeof setTimeout> | undefined;
          try {
            const ownsSurface = await Promise.race([
              worker.evaluate(() =>
                Boolean((globalThis as CompletionWorker).__donnerApplicationGpuSurfaceWorker)
              ),
              new Promise((resolve) => {
                timer = setTimeout(() => resolve(false), 1000);
              }),
            ]);
            if (!ownsSurface) throw new Error("not the surface worker");
            return worker;
          } finally {
            clearTimeout(timer);
          }
        }),
      );
      // Finding the worker takes up to a second; read the baseline from a settled state after it.
      await expect.poll(async () => {
        const state = await snapshot(page);
        report.before = state;
        return pickerPresentationSettled(state);
      }, {
        timeout: 10000,
        message: "presentation must still be idle once the surface worker is found",
      }).toBe(true);
      const before = (report.before as Snapshot).presentation!;
      expect(before).toEqual(
        expect.objectContaining({ deviceLost: false, framesInFlight: 0, oldestSerial: 0 }),
      );
      expect(before.completedSerial).toBe(before.submittedSerial);
      await application.evaluate(() => {
        const queue = GPUQueue.prototype.onSubmittedWorkDone;
        let release: (() => void) | undefined;
        const hold = new Promise<void>((resolve) => {
          release = resolve;
        });
        const state: DelayedCompletionState = {
          calls: 0,
          realCompletions: 0,
          released: false,
          firstHoldAtMs: null,
          lastRealCompletionAtMs: null,
        };
        let timer: ReturnType<typeof setTimeout> | undefined;
        const restore = () => {
          clearTimeout(timer);
          if (!state.released) {
            state.released = true;
            release!();
          }
          if (GPUQueue.prototype.onSubmittedWorkDone === heldCompletion) {
            GPUQueue.prototype.onSubmittedWorkDone = queue;
          }
        };
        const heldCompletion = function(this: GPUQueue) {
          const completion = queue.call(this);
          if (state.released) return completion;
          if (++state.calls === 1) {
            state.firstHoldAtMs = performance.now();
            timer = setTimeout(restore, 5100);
          }
          return completion.then(() => {
            ++state.realCompletions;
            state.lastRealCompletionAtMs = performance.now();
            return hold;
          });
        };
        const worker = globalThis as CompletionWorker;
        worker.__donnerDelayedUiCompletion = state;
        worker.__donnerReleaseUiCompletion = restore;
        GPUQueue.prototype.onSubmittedWorkDone = heldCompletion;
      });
      const readCompletion = () =>
        application!.evaluate(() => (globalThis as CompletionWorker).__donnerDelayedUiCompletion);
      await page.evaluate(() => {
        window.__donnerEditorFrameRequested = true;
      });
      await expect.poll(async () => (await snapshot(page)).presentation?.submittedSerial, {
        timeout: 3000,
      })
        .toBeGreaterThan(before.submittedSerial);
      await expect.poll(async () => {
        const state = await readCompletion();
        report.completedBeforeRelease = state;
        return state !== undefined && !state.released && state.calls > 0
          && state.realCompletions === state.calls && state.firstHoldAtMs !== null
          && state.lastRealCompletionAtMs !== null
          && state.lastRealCompletionAtMs - state.firstHoldAtMs < 3000;
      }, { timeout: 3000 }).toBe(true);
      await attachJson(info, "ui-gpu-completed-before-callback-release", report);
      await expect.poll(readCompletion, { timeout: 10000 })
        .toEqual(expect.objectContaining({ released: true, realCompletions: expect.any(Number) }));
      await expect.poll(async () => (await snapshot(page)).presentation?.framesInFlight, {
        timeout: 3000,
      }).toBe(0);
      report.after = await snapshot(page);
      report.completion = await readCompletion();
      await attachJson(info, "delayed-ui-completion", report);
      expect((report.completion as DelayedCompletionState).realCompletions).toBeGreaterThan(0);
      expect((report.after as Snapshot).presentation).toEqual(
        expect.objectContaining({ deviceLost: false }),
      );
      const presented = (report.after as Snapshot).presentation!.submittedSerial;
      await page.evaluate(() => {
        window.__donnerEditorFrameRequested = true;
      });
      await expect.poll(async () => (await snapshot(page)).presentation?.completedSerial, {
        timeout: 3000,
      })
        .toBeGreaterThan(presented);
    } finally {
      let timer: ReturnType<typeof setTimeout> | undefined;
      try {
        if (application) {
          report.cleanup = await Promise.race([
            application.evaluate(() => {
              (globalThis as CompletionWorker).__donnerReleaseUiCompletion?.();
              return "restored";
            }),
            new Promise((resolve) => {
              timer = setTimeout(() => resolve("release RPC deadline"), 1000);
            }),
          ]);
        }
      } catch {
        report.cleanup = "release RPC unavailable";
      } finally {
        clearTimeout(timer);
      }
      stopHeartbeat();
      await attachJson(info, "delayed-ui-completion-final", report);
    }
  },
);

// Samples, once per page animation frame, what the editor publishes after each presented frame. It
// reads no pixels: see presented-frame-samples.mjs.
async function startPresentedFrameSampler(page: Page) {
  await page.evaluate(() => {
    const w = window as unknown as {
      __donnerPresentedFrameSampler?: { running: boolean; samples: unknown[] };
      __donnerPresentationQueueStats?: Record<string, unknown>;
    };
    const sampler = { running: true, samples: [] as unknown[] };
    w.__donnerPresentedFrameSampler = sampler;
    const tick = () => {
      if (!sampler.running) return;
      const queue = w.__donnerPresentationQueueStats;
      if (queue && sampler.samples.length < 4096) {
        sampler.samples.push({
          t: performance.now(),
          frameId: Number(queue.frameId),
          completedSerial: Number(queue.completedSerial),
          submittedSerial: Number(queue.submittedSerial),
          pointerX: Number(queue.pointerX),
          pointerY: Number(queue.pointerY),
          inputRepresented: queue.inputRepresented === true,
        });
      }
      requestAnimationFrame(tick);
    };
    requestAnimationFrame(tick);
  });
}

async function stopPresentedFrameSampler(page: Page) {
  return await page.evaluate(() => {
    const sampler = (window as unknown as {
      __donnerPresentedFrameSampler?: { running: boolean; samples: unknown[] };
    }).__donnerPresentedFrameSampler;
    if (!sampler) return [];
    sampler.running = false;
    return sampler.samples;
  }) as Parameters<typeof presentedDragSummary>[0];
}

test.describe("UI presentation diagnosis", () => {
  test.use({ viewport: { width: 1600, height: 900 } });
  test(
    "held drag advances UI submissions while composited pixels are sampled",
    async ({ page, browser, browserName }, info) => {
      test.skip(projectDeviceScale(info) !== 1, "its sample bounds were set at device scale 1");
      // Firefox observes the drag through the editor's presented-frame statistics rather than by
      // reading the canvas back every frame, which stalled it on the hosted runner (#1668).
      const pixelProbe = browserName !== "firefox";
      const report: Record<string, unknown> = {
        browser: info.project.name,
        version: browser.version(),
        ...packageHashes(),
      };
      let stopHeartbeat: (() => void) | undefined;
      const checkpoint = (stage: string) => {
        report.stage = stage;
        fs.writeFileSync(
          info.outputPath("ui-presentation-checkpoint.json"),
          JSON.stringify(report),
        );
      };
      try {
        await page.goto(process.env.DONNER_WASM_BASE_URL!, { waitUntil: "domcontentloaded" });
        checkpoint("waiting for first frame");
        await expect.poll(
          () => page.evaluate(() => (window as Diagnostics).__donnerFirstFramePresented),
          {
            timeout: 30000,
          },
        ).toBe(true);
        stopHeartbeat = await startApplicationHeartbeat(page);
        checkpoint("waiting for thumbnails");
        await expect.poll(async () => {
          const thumbnails = (await snapshot(page)).thumbnails;
          return thumbnails !== undefined && !thumbnails.pending && !thumbnails.active
            && (thumbnails.ready ?? 0) > 0;
        }, { timeout: 20000 }).toBe(true);
        checkpoint("opening Donner sample");
        const canvas = await page.locator("canvas#canvas").boundingBox();
        expect(canvas).not.toBeNull();
        if (canvas === null) throw new Error("missing editor canvas");
        await dispatchCanvasPointer(page, { x: canvas.x + canvas.width * 0.24, y: canvas.y + 282 }, true);
        await expect(page.locator("canvas#canvas")).toHaveAttribute(
          "data-active-sample-id",
          "donner-splash",
        );
        checkpoint("waiting for Donner document");
        await waitForSplashDisplayed(page);
        const start = await aimAtSplashD(page);
        checkpoint("selecting D");
        await dispatchCanvasPointer(page, start, true);
        await expect.poll(async () => (await snapshot(page)).interaction, { timeout: 15000 })
          .toEqual(
            expect.objectContaining({ selectedCount: 1, pendingClick: false, workerBusy: false }),
          );
        await page.waitForTimeout(800);
        report.before = await snapshot(page, true);
        checkpoint("sampling held drag");
        if (pixelProbe) {
          await installCompositedProbe(page, {
            sampleRegionCss: { x: start.x - 200, y: start.y - 130, width: 245, height: 175 },
            sampleWidth: 96,
            sampleHeight: 96,
            minColorAlpha: 64,
            minColorSpread: 60,
          });
          await startCompositedProbe(page);
        } else {
          await startPresentedFrameSampler(page);
        }
        let stream:
          | { dispatchTimes: number[]; dispatchPoints: { x: number; y: number }[] }
          | undefined;
        let usableSamples = 0;
        let drawFraction = 0;
        let presented: ReturnType<typeof presentedDragSummary> | undefined;
        try {
          stream = await page.evaluate(async (start) => {
            const canvas = document.querySelector("canvas#canvas")!;
            const dispatch = (type: string, point: { x: number; y: number }, buttons: number) =>
              canvas.dispatchEvent(
                new MouseEvent(type, {
                  bubbles: true,
                  cancelable: true,
                  view: window,
                  clientX: point.x,
                  clientY: point.y,
                  button: 0,
                  buttons,
                }),
              );
            const dispatchTimes: number[] = [];
            const dispatchPoints: { x: number; y: number }[] = [];
            let point = start;
            dispatch("mousedown", point, 1);
            try {
              for (let i = 1; i <= 60; ++i) {
                await new Promise((resolve) => setTimeout(resolve, 16));
                point = {
                  x: Math.round(start.x - 120 * i / 60),
                  y: Math.round(start.y - 72 * i / 60),
                };
                dispatchTimes.push(performance.now());
                dispatchPoints.push(point);
                dispatch("mousemove", point, 1);
              }
            } finally {
              dispatch("mouseup", point, 0);
            }
            return { dispatchTimes, dispatchPoints };
          }, start);
        } finally {
          if (pixelProbe) {
            const result = await stopCompositedProbe(page, info, stream ?? null);
            if (stream !== undefined) {
              const firstInput = stream.dispatchTimes[0];
              const lastInput = stream.dispatchTimes.at(-1)!;
              const active = result.samples.filter((sample) =>
                sample.t >= firstInput && sample.t <= lastInput
              );
              usableSamples = active.length;
              drawFraction = active.filter((sample) => sample.drawOk).length
                / Math.max(1, active.length);
              report.motion = contentMotionFraction(active);
              report.activeSamples = usableSamples;
              report.drawFraction = drawFraction;
            }
          } else {
            const samples = await stopPresentedFrameSampler(page);
            await attachJson(info, "presented-frame-samples", { stream: stream ?? null, samples });
            if (stream !== undefined) {
              presented = presentedDragSummary(samples, {
                firstInputAt: stream.dispatchTimes[0],
                lastInputAt: stream.dispatchTimes.at(-1)!,
                start,
                end: stream.dispatchPoints.at(-1)!,
              });
              report.presented = presented;
            }
          }
          await attachJson(info, "ui-presentation-collected", report);
          let timer: ReturnType<typeof setTimeout> | undefined;
          try {
            report.after = await Promise.race([
              snapshot(page, true).catch(() => ({ unavailable: "final snapshot rejected" })),
              new Promise((resolve) => {
                timer = setTimeout(() => resolve({ unavailable: "final snapshot deadline" }), 1000);
              }),
            ]);
          } finally {
            clearTimeout(timer);
          }
          try {
            report.memory = monitoredMemory();
          } catch {
            report.memory = { unavailable: "watchdog resource sample unavailable" };
          }
        }
        if (pixelProbe) {
          expect(usableSamples).toBeGreaterThanOrEqual(process.env.CI ? 12 : 16);
          expect(drawFraction).toBeGreaterThan(0.8);
          const motion = report.motion as ReturnType<typeof contentMotionFraction>;
          expect(motion.fraction).toBeGreaterThanOrEqual(0.15);
        } else {
          expect(presented, "the held drag must finish its input stream").toBeDefined();
          expect(presented!.activeSamples, "frames sampled during the drag")
            .toBeGreaterThanOrEqual(process.env.CI ? 12 : 16);
          expect(
            presentedDragFailures(presented!, kHeldDragPresentationBounds),
            "presented frames must keep completing and follow the dragged pointer",
          ).toEqual([]);
        }
      } finally {
        stopHeartbeat?.();
        await attachJson(info, "ui-presentation-diagnosis", report);
      }
    },
  );
});

test("ordinary document GPU residency", async ({ page }, info) => {
  let stopHeartbeat = () => {};
  try {
    await checkDocumentGpuResidency(page, info, async () => {
      stopHeartbeat = await startApplicationHeartbeat(page);
    });
  } finally {
    stopHeartbeat();
  }
});

// The bridge's GPU queue timeline, on the page clock, and its object statistics, from whichever
// worker owns the browser device.
async function readGpuTimelineOnPageClock(page: Page) {
  const timeOrigin = await page.evaluate(() => performance.timeOrigin);
  for (const worker of page.workers()) {
    const read = await worker.evaluate(() => {
      const state = globalThis as unknown as Record<string, unknown>;
      const timeline = state.__donnerReadGpuTimeline;
      const statistics = state.__donnerReadGpuObjectStats;
      return typeof timeline === "function" && typeof statistics === "function"
        ? {
          entries: (timeline as () => (string | number)[][])(),
          statistics: (statistics as () => unknown)(),
        }
        : null;
    }).catch(() => null);
    if (read === null || read.entries.length === 0) continue;
    const { entries, statistics } = read;
    const toPage = (epochMs: unknown) =>
      Number(epochMs) > 0 ? Math.round((Number(epochMs) - timeOrigin) * 10) / 10 : 0;
    const timeline = entries.map((fields) => {
      switch (fields[0]) {
        case "s":
          return [...fields.slice(0, 5), toPage(fields[5]), toPage(fields[6])];
        case "m":
          return [...fields.slice(0, 3), toPage(fields[3]), toPage(fields[4])];
        case "p":
          return fields.length >= 5
            ? [fields[0], fields[1], toPage(fields[2]), toPage(fields[3]), fields[4]]
            : [fields[0], fields[1], toPage(fields[2])];
        default:
          return [fields[0], fields[1], toPage(fields[2]), ...fields.slice(3)];
      }
    });
    return { timeline, statistics };
  }
  return null;
}

// Clicks a sample the moment the editor has presented its first frame, while boot work such as the
// sample thumbnails is still on the GPU, and splits the wait for the clicked document into the GPU
// queue backlog left from before the click and the presentation handoff after it. Report only: the
// numbers depend on the GPU. Run with DONNER_BROWSER_WEBGPU_ADAPTER=swiftshader to measure a slow
// GPU, where a pipeline's first use used to hold the queue for seconds.
test("first sample click after boot", async ({ page, browser }, info) => {
  const failures: string[] = [];
  page.on("pageerror", (error) => failures.push(error.message));
  page.on("console", (message) => {
    if (/Aborted|RuntimeError|UTILS_RELEASE_ASSERT|device lost/i.test(message.text())) {
      failures.push(message.text());
    }
  });
  await page.goto(process.env.DONNER_WASM_BASE_URL!, { waitUntil: "domcontentloaded" });
  await expect.poll(
    () => page.evaluate(() => (window as Diagnostics).__donnerFirstFramePresented),
    { timeout: 30000, message: "WebGPU must present the production editor before the click" },
  ).toBe(true);
  const canvas = await page.locator("canvas#canvas").boundingBox();
  expect(canvas).not.toBeNull();
  if (canvas === null) throw new Error("missing editor canvas");
  const beforeResults = (await snapshot(page)).worker?.completedResults ?? 0;
  const clickAt = await dispatchCanvasPointer(
    page, { x: canvas.x + canvas.width * 0.76, y: canvas.y + 282 }, true,
  );
  await expect(page.locator("canvas#canvas")).toHaveAttribute(
    "data-active-sample-id",
    "basic-shapes",
  );
  let worker: Diagnostics["__donnerWorkerStats"];
  let presentation: Diagnostics["__donnerPresentedDocumentStats"];
  await expect.poll(async () => {
    const state = await snapshot(page);
    worker = state.worker;
    presentation = state.documentPresentation;
    return (worker?.completedResults ?? 0) > beforeResults && worker?.presentedAtMs !== undefined
      && worker?.resultFramePresented === true
      && presentation?.documentGeneration === worker.documentGeneration
      && presentation?.frameVersion === worker.frameVersion
      && Number.isFinite(worker?.resultFrameCompletedAtMs);
  }, { timeout: 60000, message: "the clicked document must reach the canvas" }).toBe(true);
  // The breakdown needs every pre-click submission retired, which presentation implies.
  const gpu = await readGpuTimelineOnPageClock(page);
  expect(gpu, "the bridge must publish its GPU queue timeline").not.toBeNull();
  const completedAt = worker!.publishedAtMs - worker!.pollDelayMs;
  const breakdown = firstClickBreakdown(gpu!.timeline, {
    clickAt,
    completedAt,
    presentedAt: worker!.resultFrameCompletedAtMs!,
  });
  const report = {
    browser: info.project.name,
    version: browser.version(),
    adapter: process.env.DONNER_BROWSER_WEBGPU_ADAPTER ?? "default",
    backend: (await snapshot(page)).backend,
    viewport: await page.evaluate(() => ({ width: innerWidth, height: innerHeight })),
    raster: await rasterIdentity(page),
    ...packageHashes(),
    clickAt,
    completedAt,
    cpuFrameFinishedAt: worker!.presentedAtMs!,
    presentedAt: worker!.resultFrameCompletedAtMs!,
    ...breakdown,
    gpuObjects: gpu!.statistics,
  };
  console.log(`first-click-after-boot ${JSON.stringify(report)}`);
  await attachJson(info, "first-click-after-boot.json", { ...report, timeline: gpu!.timeline });
  expect(failures).toEqual([]);
});

// The nominal completed-result handoff of each carousel sample: from the raster worker finishing
// the clicked document to GPU retirement of the frame that presents it, on the smoke suite's
// carousel layout. Only the raw handoff is gated; GPU work already queued ahead of the presenting
// frame is part of it. The smoke suite asserts the ordering of the same interaction without a
// time bound, because a shared runner's GPU can retire the presenting frame behind other work.
// DONNER_CAROUSEL_CANVAS_GPU_DELAY=<everyFrames>:<iterations> queues fixed GPU work after the first
// canvas write of every Nth frame from the click on, as the negative control of this gate.
test.describe("nominal completed-result handoff", () => {
  test.use({ viewport: kCarouselViewport });
  for (const sample of kCarouselSamples) {
    test(`carousel ${sample.id}`, async ({ page, browserName }, info) => {
      test.skip(browserName !== "chromium", "the bound is calibrated for Chromium");
      test.skip(projectDeviceScale(info) !== 1, "the bound is calibrated at device scale 1");
      const failures: string[] = [];
      page.on("pageerror", (error) => failures.push(error.message));
      await page.goto(process.env.DONNER_WASM_BASE_URL!, { waitUntil: "domcontentloaded" });
      await expect.poll(
        () => page.evaluate(() => (window as Diagnostics).__donnerFirstFramePresented),
        { timeout: kStallGuardMs, message: "WebGPU must present the production editor" },
      ).toBe(true);
      // A cold shader cache holds later GPU work behind asynchronous pipeline compiles, so the
      // measured click starts once the GPU owner reports none in flight.
      await expect.poll(async () => {
        const statistics = await Promise.all(page.workers().map((worker) =>
          worker.evaluate(() => {
            const scope = globalThis as typeof globalThis & {
              __donnerGpuOwner?: boolean;
              __donnerReadGpuObjectStats?: () => Record<string, unknown>;
            };
            return scope.__donnerGpuOwner ? scope.__donnerReadGpuObjectStats?.() ?? null : null;
          }).catch(() => null)
        ));
        const { pending, reporting } = sumPipelineCompilesPending(statistics);
        return reporting > 0 && pending === 0;
      }, {
        timeout: kStallGuardMs,
        message: "the GPU owner's pipeline compiles settle before the measured click",
      }).toBe(true);
      let gpuDelay: Awaited<ReturnType<typeof armCanvasGpuDelayFromEnv>> = null;
      if (canvasGpuDelayFromEnv("DONNER_CAROUSEL_CANVAS_GPU_DELAY") !== null) {
        await expectSurfaceFrameProbeReady(page);
        const owner = await findProbedCanvasApplicationWorker(page);
        if (owner === null) throw new Error("the application canvas owner is unavailable");
        gpuDelay = await armCanvasGpuDelayFromEnv(owner, "DONNER_CAROUSEL_CANVAS_GPU_DELAY");
      }
      const canvas = await page.locator("canvas#canvas").boundingBox();
      expect(canvas).not.toBeNull();
      if (canvas === null) throw new Error("missing editor canvas");
      let injected: Awaited<ReturnType<NonNullable<typeof gpuDelay>["finish"]>> | undefined;
      let worker: Diagnostics["__donnerWorkerStats"];
      try {
        const before = await page.evaluate(() => ({
          results: (window as Diagnostics).__donnerWorkerStats?.completedResults ?? 0,
          frames: (window as Diagnostics & { __donnerMainLoopRenderedFrames?: number })
            .__donnerMainLoopRenderedFrames ?? 0,
        }));
        await page.mouse.click(canvas.x + canvas.width * sample.xFraction, canvas.y + sample.y);
        await expect(page.locator("canvas#canvas")).toHaveAttribute(
          "data-active-sample-id",
          sample.id,
          { timeout: kStallGuardMs },
        );
        await expect.poll(async () => {
          const state = await page.evaluate(() => ({
            frames: (window as Diagnostics & { __donnerMainLoopRenderedFrames?: number })
              .__donnerMainLoopRenderedFrames ?? 0,
            worker: (window as Diagnostics).__donnerWorkerStats,
            presentation: (window as Diagnostics).__donnerPresentedDocumentStats,
          }));
          worker = state.worker;
          return (worker?.completedResults ?? 0) > before.results && state.frames > before.frames
            && worker?.resultFramePresented === true
            && state.presentation?.documentGeneration === worker.documentGeneration
            && state.presentation?.frameVersion === worker.frameVersion
            && Number.isFinite(worker?.resultFrameCompletedAtMs);
        }, {
          timeout: kStallGuardMs,
          intervals: [8, 16, 25, 50],
          message: "the clicked document must reach the canvas",
        }).toBe(true);
      } finally {
        // Retain what was injected even when the click never presented.
        injected = await gpuDelay?.finish()
          .catch((error: unknown) => ({ spec: "", disarmError: String(error) }));
        if (injected) console.log(`carousel-canvas-gpu-delay ${JSON.stringify(injected)}`);
      }
      const handoff = completedResultHandoff(
        worker!.publishedAtMs - worker!.pollDelayMs,
        worker!.resultFrameCompletedAtMs!,
      );
      const report = {
        workload: sample.id,
        browser: info.project.name,
        backend: await page.evaluate(() => (window as Diagnostics).__donnerBackend),
        ...packageHashes(),
        ...handoff,
        canvasGpuDelay: injected,
        worker,
      };
      console.log(`nominal-completed-result-handoff ${JSON.stringify(report)}`);
      await attachJson(info, `nominal-completed-result-handoff-${sample.id}.json`, report);
      expect(failures).toEqual([]);
      expect(Number.isFinite(handoff.handoffMs)).toBe(true);
      expect(handoff.handoffMs).toBeGreaterThanOrEqual(0);
      latencyGate("completedResultHandoff", `carousel-${sample.id}`, handoff.handoffMs);
    });
  }
});

/**
 * Record the counters of every worker result published from now on. Frame sampling can miss a
 * result that is replaced before the next animation frame, so this observes each publication.
 */
async function startWorkerResultCounters(page: Page) {
  await page.evaluate(() => {
    const state = window as Diagnostics;
    const records: WorkerResultCounters[] = [];
    let current = state.__donnerWorkerStats;
    Object.defineProperty(window, "__donnerWorkerStats", {
      configurable: true,
      enumerable: true,
      get: () => current,
      set: (stats: Diagnostics["__donnerWorkerStats"]) => {
        current = stats;
        if (stats?.publishReason !== "render-result") return;
        const countersPublished = Object.hasOwn(stats, "activeDragResult");
        const count = (name: string) => (countersPublished ? Number(stats[name]) : null) as number;
        records.push({
          completedResults: stats.completedResults,
          countersPublished,
          activeDragResult: countersPublished ? stats.activeDragResult === true : null,
          fastPathFrames: count("fastPathFrames"),
          slowPathFramesWithDirty: count("slowPathFramesWithDirty"),
          noDirtyFrames: count("noDirtyFrames"),
          damagePatchTileCount: count("damagePatchTileCount"),
          maskedChildComposeTileCount: count("maskedChildComposeTileCount"),
          cachedTileCount: count("cachedTileCount"),
          immediateTileCount: count("immediateTileCount"),
          offscreenRecycleCount: count("offscreenRecycleCount"),
          mainComposeCount: count("mainComposeCount"),
          gpuOwnerDispatchCount: count("gpuOwnerDispatchCount"),
          gpuOwnerWaitSliceTimeouts: count("gpuOwnerWaitSliceTimeouts"),
          gpuOwnerDispatchWaitMs: count("gpuOwnerDispatchWaitMs"),
          gpuOwnerDispatch: countersPublished ? (stats.gpuOwnerDispatch ?? null) : null,
          compositorRenderCount: count("compositorRenderCount"),
          workerMs: stats.workerMs,
          renderFrameMs: count("renderFrameMs"),
          interactionPromotion: (stats.interactionPromotion ?? null) as InteractionPromotion | null,
        });
      },
    });
    state.__donnerWorkerResultCounters = records;
  });
}

/** Stop recording and return the counters of the results published since the start. */
async function stopWorkerResultCounters(page: Page) {
  return page.evaluate(() => {
    const state = window as Diagnostics;
    const current = state.__donnerWorkerStats;
    delete (window as { __donnerWorkerStats?: unknown }).__donnerWorkerStats;
    state.__donnerWorkerStats = current;
    const records = state.__donnerWorkerResultCounters ?? [];
    delete state.__donnerWorkerResultCounters;
    return records;
  });
}

/**
 * As in the native compositor, every result rendered while the masked child is held takes the
 * fast path (or finds nothing dirty) without a slow-path frame, and rasterizes at most one tile,
 * by patching only the damaged part of its owner. A drag-start result finds nothing dirty and
 * rasterizes no tile, so it needs no exception, and a drag that completes only that result while
 * held passes: how far the held drag gets is `heldDragFastPathResults`.
 */
function expectHeldMaskedDragDamagePatch(summary: MaskedDragSummary) {
  expect(
    heldMaskedDragShapeFailures(summary),
    "every held masked-child result must take the fast path and only patch its owning tile",
  ).toEqual([]);
}

type MaskedDragSummary = {
  countersPublished: boolean;
  resultCount: number;
  held: WorkerResultCounters[];
  release: WorkerResultCounters[];
  checkHeldResults: boolean;
};

/**
 * Drag the selected masked crown face while recording every worker result's counters. The counters
 * are printed as the held-drag and release baseline, or reported absent for a bundle that predates
 * them. A candidate run checks the held results even when the drag itself misses a check, so it
 * reports both; a baseline run only measures.
 */
async function maskedCrownDragWithCounters(
  page: Page,
  point: { x: number; y: number },
  sourceVisible: boolean,
) {
  const phase = "geode-crown";
  await startWorkerResultCounters(page);
  let drag: Awaited<ReturnType<typeof continuousDrag>> | undefined;
  let dragFailure: unknown;
  try {
    drag = await continuousDrag(page, point, 0.25, phase);
  } catch (error) {
    dragFailure = error;
  }
  const results = summarizeMaskedDragResults(await stopWorkerResultCounters(page), {
    baselineOnly: process.env.DONNER_BROWSER_BASELINE === "1",
  }) as MaskedDragSummary;
  const baseline = {
    project: test.info().project.name,
    raster: await rasterIdentity(page),
    sourceVisible,
    ...results,
  };
  console.log(`masked-drag-counters ${JSON.stringify(baseline)}`);
  const unpatched = results.held.filter((result) => unpatchedTileCount(result) > 0).length;
  const slowPath = results.held.filter((result) => result.slowPathFramesWithDirty > 0).length;
  const fastPath = heldFastPathResultCount(results);
  const checkFailures: unknown[] = [];
  if (results.checkHeldResults) {
    await completionCheck(
      "heldMaskedDragDamagePatch",
      phase,
      `${results.held.length} held results, ${slowPath} slow path, ${unpatched} rasterized unpatched`,
      () => expectHeldMaskedDragDamagePatch(results),
    ).catch((error) => checkFailures.push(error));
    // Enforced on every project and run: a drag slower than its hold fails here, as a held drag
    // that rendered no move result, not as a fast-path defect (#2106).
    await completionCheck(
      "heldDragFastPathResults",
      phase,
      `${results.held.length} held results, ${fastPath} fast path`,
      () =>
        expect(
          fastPath,
          "the held drag must render a fast-path result for a move before release, not only "
            + "its drag-start result",
        ).toBeGreaterThan(0),
    ).catch((error) => checkFailures.push(error));
  }
  if (dragFailure !== undefined) throw dragFailure;
  if (checkFailures.length > 0) throw checkFailures[0];
  return { drag, workerResults: results };
}

test.describe("Geode detailed interactions", () => {
  test.use({ viewport: { width: 1600, height: 900 } });
  for (const sourceVisible of [false, true]) {
    test(
      `masked crown drag with source ${sourceVisible ? "open" : "closed"}`,
      async ({ page, browser }, info) => {
        const report: Record<string, unknown> = {
          browser: browser.version(),
          sourceVisible,
          ...packageHashes(),
        };
        let stopHeartbeat: (() => void) | undefined;
        const save = (stage: string) => {
          report.stage = stage;
          fs.writeFileSync(info.outputPath("geode-interaction.json"), JSON.stringify(report));
        };
        try {
          const url = new URL(process.env.DONNER_WASM_BASE_URL!);
          url.searchParams.set("testControl", "eyedropper");
          await page.goto(url.href, { waitUntil: "domcontentloaded" });
          await expect.poll(() =>
            page.evaluate(() => (window as Diagnostics).__donnerFirstFramePresented)
          ).toBe(true);
          stopHeartbeat = await startApplicationHeartbeat(page);
          const bounds = await page.locator("canvas#canvas").boundingBox();
          if (!bounds) throw new Error("missing canvas");
          await dispatchCanvasPointer(
            page,
            { x: bounds.x + bounds.width * 0.5, y: bounds.y + 282 },
            true,
          );
          await expect(page.locator("canvas#canvas")).toHaveAttribute(
            "data-active-sample-id",
            "geode-splash",
          );
          await expect.poll(async () => (await snapshot(page)).worker?.acceptedForPresentation)
            .toBe(true);
          await waitForIdle(page);
          report.loaded = await snapshot(page, true);
          save("loaded");
          if (sourceVisible) {
            const before = await snapshot(page);
            const began = performance.now();
            await dispatchCanvasPointer(page, { x: 16, y: 180 }, true);
            await expect.poll(async () => (await snapshot(page)).viewport?.paneX ?? 0)
              .toBeGreaterThan(200);
            report.sourceOpenWallMs = performance.now() - began;
            save("source opened");
            for (let i = 0; i < 12; ++i) {
              const x = 100 + i * 5;
              await dispatchCanvasPointer(page, { x, y: 300 });
              await expect.poll(async () => (await snapshot(page)).interaction?.pointerX, {
                timeout: 10000,
              }).toBe(x);
            }
            report.sourceHover = phaseReport(before, await snapshot(page), []);
            save("source hovered");
          }
          await waitForIdle(page);
          for (let step = 0; step < 6; ++step) {
            const v = (await snapshot(page)).viewport!;
            if (Math.abs(v.zoom - 1.5) < 0.01) break;
            const aim = {
              x: v.documentX + v.documentWidth * (1589 / 3) / 1536,
              y: v.documentY + v.documentHeight * (1558 / 3) / 1024,
            };
            await dispatchCanvasPointer(page, { x: Math.round(aim.x), y: Math.round(aim.y) });
            await page.evaluate(
              ({ aim, deltaY }) =>
                document.querySelector("canvas#canvas")!.dispatchEvent(
                  new WheelEvent("wheel", {
                    bubbles: true,
                    cancelable: true,
                    clientX: aim.x,
                    clientY: aim.y,
                    ctrlKey: true,
                    deltaY,
                  }),
                ),
              { aim, deltaY: -100 * Math.max(-0.25, Math.min(0.25, Math.log(1.5 / v.zoom))) },
            );
            await expect.poll(async () => (await snapshot(page)).viewport?.zoom).not.toBe(v.zoom);
            await waitForIdle(page);
          }
          const viewport = (await snapshot(page)).viewport!;
          const point = {
            x: Math.round(viewport.documentX + viewport.documentWidth * (1589 / 3) / 1536),
            y: Math.round(viewport.documentY + viewport.documentHeight * (1558 / 3) / 1024),
          };
          await dispatchCanvasPointer(page, point, true);
          await expect.poll(() =>
            page.evaluate(() =>
              (window as Window & {
                __donnerEyedropperTestState?: { selectedId: string };
              }).__donnerEyedropperTestState?.selectedId
            ), { message: "select the actual masked crown face" })
            .toBe("central-crown-face-3");
          await waitForIdle(page);
          report.selected = await snapshot(page, true);
          save("crown selected");
          // Separate selection from the drag press so this does not enter double-click editing.
          await page.waitForTimeout(500);
          report.ownerBeforeDrag = await readGpuOwnerDispatchStats(page);
          Object.assign(report, await maskedCrownDragWithCounters(page, point, sourceVisible));
          report.ownerAfterDrag = await readGpuOwnerDispatchStats(page);
          save("drag finished");
          await page.screenshot({ path: info.outputPath("geode-interaction.png") });
        } finally {
          report.final = await snapshot(page, true).catch(() => null);
          save("finished");
          stopHeartbeat?.();
          console.log(`geode-interaction ${JSON.stringify(report)}`);
        }
      },
    );
  }
});

// A shape first selected after zooming in must still get its own layer (#2575). Retained textures
// grow with the square of the device scale, so `chromium` and `firefox` (3840x2160) are where a
// budget that counts them refuses it; `chromium-dpr1` and `firefox-dpr1` are the control.
test.describe("fresh selection after zoom", () => {
  test.use({ viewport: { width: 1920, height: 1080 } });

  test("a letter selected after zooming in to 3x drags as its own layer", async ({ page }, info) => {
    const failures: string[] = [];
    page.on("pageerror", (error) => failures.push(error.message));
    page.on("console", (message) => {
      if (/Aborted|RuntimeError|UTILS_RELEASE_ASSERT|device lost/i.test(message.text())) {
        failures.push(message.text());
      }
    });
    let stopHeartbeat: (() => void) | undefined;
    const report: Record<string, unknown> = { project: info.project.name, ...packageHashes() };
    try {
      await page.goto(process.env.DONNER_WASM_BASE_URL!, { waitUntil: "domcontentloaded" });
      await expect.poll(
        () => page.evaluate(() => (window as Diagnostics).__donnerFirstFramePresented),
        { timeout: 30000, message: "WebGPU must present the production editor" },
      ).toBe(true);
      stopHeartbeat = await startApplicationHeartbeat(page);
      const raster = await rasterIdentity(page);
      report.raster = raster;
      expect(raster).toEqual(expect.objectContaining({
        backingWidth: Math.round(1920 * raster.dpr),
        backingHeight: Math.round(1080 * raster.dpr),
        cssWidth: 1920,
        cssHeight: 1080,
      }));

      // The welcome carousel is centered: at this width the splash tile sits 310 CSS px left of
      // the canvas center.
      const canvas = page.locator("canvas#canvas");
      const bounds = (await canvas.boundingBox())!;
      await dispatchCanvasPointer(
        page,
        { x: bounds.x + bounds.width / 2 - 310, y: bounds.y + 282 },
        true,
      );
      await expect(canvas).toHaveAttribute("data-active-sample-id", "donner-splash");
      await waitForSplashDisplayed(page);
      await zoomSplashForDrag(page, 3);

      // The splash N's left stem, a letter nothing has selected yet.
      const viewport = (await snapshot(page)).viewport!;
      expect(showsSplashDocument(viewport), "the viewport must describe the splash").toBe(true);
      const point = {
        x: Math.round(
          viewport.documentX + viewport.documentWidth * 413 / kSplashDocumentSize.width,
        ),
        y: Math.round(
          viewport.documentY + viewport.documentHeight * 360 / kSplashDocumentSize.height,
        ),
      };
      expect(point.x).toBeGreaterThan(viewport.paneX);
      expect(point.x).toBeLessThan(viewport.paneX + viewport.paneWidth);
      expect(point.y).toBeGreaterThan(viewport.paneY);
      expect(point.y).toBeLessThan(viewport.paneY + viewport.paneHeight);
      await startWorkerResultCounters(page);
      await dispatchCanvasPointer(page, point, true);
      await expect.poll(async () => (await snapshot(page)).interaction?.selectedCount, {
        timeout: 10000,
        message: "the click must select the splash N",
      }).toBe(1);
      await waitForIdle(page);
      // Separate the selection click from the drag press so it cannot become a double click.
      await page.waitForTimeout(400);
      const selection = await stopWorkerResultCounters(page);

      await startWorkerResultCounters(page);
      const stream = await streamDragToIdle(page, point, -1);
      const drag = await stopWorkerResultCounters(page);

      const promotions = [...selection, ...drag]
        .map((result) => result.interactionPromotion)
        .filter((promotion): promotion is InteractionPromotion => Boolean(promotion?.requested));
      const summary = {
        zoom: viewport.zoom,
        dragInputs: stream.dispatchPoints.length,
        promotions,
        dragResults: drag.length,
        activeDragResults: drag.filter((result) => result.activeDragResult).length,
        fastPathFrames: drag.reduce((sum, result) => sum + (result.fastPathFrames ?? 0), 0),
        slowPathFramesWithDirty: drag.reduce(
          (sum, result) => sum + (result.slowPathFramesWithDirty ?? 0),
          0,
        ),
      };
      report.summary = summary;
      console.log(`fresh-selection-after-zoom ${info.project.name} ${JSON.stringify(summary)}`);

      expect(stream.frames.some((frame) => frame.dragging), "the stream must enter drag mode")
        .toBe(true);
      expect(promotions.length, "selecting and dragging the N must request its promotion")
        .toBeGreaterThan(0);
      const refused = promotions.filter((promotion) => promotion.result !== "PromotedLayer");
      expect(
        { refusedRequests: refused.length, firstRefusal: refused[0] ?? null },
        "every promotion request for the freshly selected N must be admitted",
      ).toEqual({ refusedRequests: 0, firstRefusal: null });
      expect(summary.fastPathFrames, "the drag must move the N's layer on the fast path")
        .toBeGreaterThan(0);
      // An admitted drag renders a drag-start and a release result; a refused one renders up to one
      // per pointer move. Eight leaves room for a recapture without accepting that fallback.
      expect(summary.activeDragResults, "the drag must not re-render once per pointer move")
        .toBeLessThanOrEqual(8);
      expect(failures).toEqual([]);
    } finally {
      stopHeartbeat?.();
      await attachJson(info, "fresh-selection-after-zoom.json", report);
    }
  });
});
