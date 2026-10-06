import { expect, type Page, test } from "@playwright/test";
import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import { startApplicationHeartbeat } from "./browser-application-heartbeat.mjs";
import { resourceQueuesAreQuiescent } from "./browser-resource-quiescence.mjs";
import {
  contentMotionFraction,
  installCompositedProbe,
  startCompositedProbe,
} from "./composited-probe";
import { stopCompositedProbe } from "./composited-probe-evidence.mjs";
import { inspectGpuImageTransfer } from "./gpu-image-transfer-canary";
import {
  checkLatencyGate,
  completionCheckMode,
  kCompletionChecks,
  latencyGateMode,
  runCompletionCheck,
} from "./latency-gates.mjs";
import { showsSplashDocument, splashDPoint } from "./splash-aim.mjs";

interface Diagnostics extends Window {
  __donnerBackend?: string;
  __donnerFirstFramePresented?: boolean;
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
}

function packageHashes() {
  return Object.fromEntries(["wasm", "js"].map((extension) => [
    `${extension}Sha256`,
    crypto.createHash("sha256").update(
      fs.readFileSync(path.join(process.env.DONNER_WASM_PACKAGE_DIR!, `editor.${extension}`)),
    ).digest("hex"),
  ]));
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

async function dispatchPointer(page: Page, point: { x: number; y: number }, click = false) {
  // Headed Firefox on macOS can offset native automation moves but not button events.
  await page.evaluate(({ x, y, click }) => {
    const canvas = document.querySelector("canvas#canvas")!;
    for (const type of click ? ["mousemove", "mousedown", "mouseup"] : ["mousemove"]) {
      canvas.dispatchEvent(
        new MouseEvent(type, {
          bubbles: true,
          cancelable: true,
          view: window,
          clientX: x,
          clientY: y,
          button: 0,
          buttons: type === "mousedown" ? 1 : 0,
        }),
      );
    }
  }, { ...point, click });
}

async function waitForIdle(page: Page) {
  await expect.poll(async () => (await snapshot(page)).interaction, {
    message: "the editor must consume pending input and finish its foreground render",
    timeout: 15000,
  }).toEqual(expect.objectContaining({ pendingClick: false, workerBusy: false }));
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
  await dispatchPointer(page, point);
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

async function zoomSplashForDrag(page: Page) {
  for (let step = 0; step < 12; ++step) {
    const viewport = (await snapshot(page)).viewport!;
    if (Math.abs(viewport.zoom - 1.93) < 0.001) break;
    const point = await aimAtSplashD(page);
    const deltaY = -100 * Math.max(-0.25, Math.min(0.25, Math.log(1.93 / viewport.zoom)));
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
  expect((await snapshot(page)).viewport!.zoom).toBeCloseTo(1.93, 2);
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
  gate: "settledClick" | "inputP95" | "inputStall" | "completedStall",
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
 * unless the run only reports completion checks and the miss is in the check's tracked browser.
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

async function continuousDrag(
  page: Page,
  start: { x: number; y: number },
  direction: number,
  phase: string,
) {
  const before = await snapshot(page);
  const stream = await page.evaluate(async ({ start, direction }) => {
    const state = window as Diagnostics;
    const canvas = document.querySelector("canvas#canvas")!;
    const dispatch = (type: string, x: number, y: number, buttons: number) => {
      canvas.dispatchEvent(
        new MouseEvent(type, {
          bubbles: true,
          cancelable: true,
          view: window,
          clientX: x,
          clientY: y,
          button: 0,
          buttons,
        }),
      );
    };
    const frames: {
      frame: number;
      atMs: number;
      pointerX?: number;
      pointerY?: number;
      selectedCount?: number;
      dragging?: boolean;
      workerMs?: number;
      completedResults?: number;
      readbackCount?: unknown;
      readbackPollIterations?: unknown;
      presentation?: Diagnostics["__donnerPresentationQueueStats"];
    }[] = [];
    const dispatchTimes: number[] = [];
    const dispatchPoints: { x: number; y: number }[] = [];
    let previousCompletedSerial = 0;
    let previousFrame = state.__donnerFrameLoopStats?.renderedFrames ?? 0;
    let observing = true;
    let sampleId = 0;
    const observe = () => {
      const loop = state.__donnerFrameLoopStats;
      const presentation = state.__donnerPresentationQueueStats;
      if (
        loop
        && (loop.renderedFrames !== previousFrame
          || (presentation?.completedSerial ?? 0) !== previousCompletedSerial)
      ) {
        previousCompletedSerial = presentation?.completedSerial ?? 0;
        previousFrame = loop.renderedFrames;
        if (frames.length < 256) {
          frames.push({
            frame: loop.renderedFrames,
            atMs: loop.lastFrameAtMs,
            pointerX: state.__donnerInteractionStats?.pointerX,
            pointerY: state.__donnerInteractionStats?.pointerY,
            selectedCount: state.__donnerInteractionStats?.selectedCount,
            dragging: state.__donnerInteractionStats?.dragging,
            workerMs: state.__donnerWorkerStats?.workerMs,
            completedResults: state.__donnerWorkerStats?.completedResults,
            presentation,
            readbackCount: state.__donnerWorkerStats?.readbackCount,
            readbackPollIterations: state.__donnerWorkerStats?.readbackPollIterations,
          });
        }
      }
      if (observing) sampleId = requestAnimationFrame(observe);
    };
    const startedAtMs = performance.now();
    observe();
    dispatch("mousedown", start.x, start.y, 1);
    let end = start;
    try {
      await new Promise<void>((resolve) => {
        let step = 0;
        const move = () => {
          ++step;
          const fraction = step / 60;
          end = {
            x: Math.round(start.x + direction * 120 * fraction),
            y: Math.round(start.y + direction * 72 * fraction),
          };
          dispatchTimes.push(performance.now());
          dispatchPoints.push(end);
          dispatch("mousemove", end.x, end.y, 1);
          if (step < 60) setTimeout(move, 16);
          else resolve();
        };
        setTimeout(move, 16);
      });
    } finally {
      dispatch("mouseup", end.x, end.y, 0);
      if (state.__donnerPresentationQueueStats) {
        await new Promise<void>((resolve) => {
          const deadline = performance.now() + 500;
          const finish = () => {
            const frame = state.__donnerPresentationQueueStats;
            if (
              (frame?.inputRepresented && !frame.mouseDown && frame.pointerX === end.x
                && frame.pointerY === end.y) || performance.now() >= deadline
            ) resolve();
            else setTimeout(finish, 8);
          };
          finish();
        });
      }
      observe();
      observing = false;
      cancelAnimationFrame(sampleId);
    }
    const stream = {
      startedAtMs,
      finishedAtMs: performance.now(),
      dispatchTimes,
      dispatchPoints,
      frames,
      end,
    };
    state.__donnerLastDragStream = stream;
    return stream;
  }, { start, direction });
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
  const after = await snapshot(page);
  const baselineOnly = process.env.DONNER_BROWSER_BASELINE === "1";
  if (!baselineOnly) {
    expect(after.presentation, "candidate completion telemetry is required").toBeTruthy();
  }
  if (!baselineOnly && after.presentation) {
    await expect.poll(async () => {
      const state = await snapshot(page);
      const completed = state.presentation;
      return completed?.inputRepresented && !completed.mouseDown
        && completed.pointerX === stream.end.x && completed.pointerY === stream.end.y
        && Math.abs(completed.viewportZoom - state.viewport!.zoom) < 0.001;
    }, {
      timeout: 5000,
      message: "release must reach a current paired frame at the requested camera",
    }).toBe(true);
  }
  expect(after.interaction?.selectedCount, "the D drag must leave one shape selected").toBe(1);
  expect(stream.frames.some((frame) => frame.dragging), "the stream must enter drag mode").toBe(
    true,
  );
  const gaps = (times: number[]) => times.slice(1).map((time, index) => time - times[index]);
  const latencies = stream.dispatchPoints.flatMap((point, index) => {
    const frame = stream.frames.find((sample) =>
      sample.presentation?.inputRepresented
      && sample.presentation.completedSerial > (before.presentation?.completedSerial ?? 0)
      && sample.presentation.completedAtMs >= stream.startedAtMs
      && stream.dispatchPoints.findIndex((applied) =>
          applied.x === sample.presentation!.pointerX && applied.y === sample.presentation!.pointerY
        ) >= index
      && sample.presentation.completedAtMs >= stream.dispatchTimes[index]
    );
    return frame?.presentation
      ? [frame.presentation.completedAtMs - stream.dispatchTimes[index]]
      : [];
  });
  const completions = new Map<number, typeof stream.frames[number]>();
  for (const frame of stream.frames) {
    if (
      frame.presentation?.inputRepresented
      && frame.presentation.completedSerial > (before.presentation?.completedSerial ?? 0)
      && frame.presentation.completedAtMs >= stream.startedAtMs
      && stream.dispatchPoints.some((point) =>
        point.x === frame.presentation!.pointerX
        && point.y === frame.presentation!.pointerY
      )
    ) {
      completions.set(frame.presentation.completedSerial, frame);
    }
  }
  const completedFrames = [...completions.values()];
  if (!baselineOnly) {
    const positions = new Set(
      completedFrames.filter((frame) => frame.presentation!.mouseDown)
        .map((frame) => `${frame.presentation!.pointerX},${frame.presentation!.pointerY}`),
    );
    expect(positions.size, "held drag must complete multiple represented positions")
      .toBeGreaterThan(1);
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
      await dispatchPointer(
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
            await dispatchPointer(page, point);
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
      await dispatchPointer(page, dragStart, true);
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
      await dispatchPointer(page, { x: 1280 * 0.24, y: 282 }, true);
      await expect(page.locator("canvas#canvas")).toHaveAttribute(
        "data-active-sample-id",
        "donner-splash",
      );
      await waitForIdle(page);
      let point = await zoomSplashForDrag(page);
      const beforeSelection = await snapshot(page);
      await dispatchPointer(page, point, true);
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
      await dispatchPointer(page, { x: 1280 * 0.24, y: 282 }, true);
      await expect(page.locator("canvas#canvas")).toHaveAttribute(
        "data-active-sample-id",
        "donner-splash",
      );
      await waitForIdle(page);
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
      await expect.poll(async () => {
        const state = await snapshot(page);
        return !state.thumbnails?.pending && !state.thumbnails?.active
          && (state.thumbnails?.ready ?? 0) > 0
          && state.presentation?.framesInFlight === 0;
      }, { timeout: 20000 }).toBe(true);
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
      report.before = await snapshot(page);
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

test.describe("UI presentation diagnosis", () => {
  test.use({ viewport: { width: 1600, height: 900 }, deviceScaleFactor: 1 });
  test(
    "held drag advances UI submissions while composited pixels are sampled",
    async ({ page, browser, browserName }, info) => {
      test.skip(
        browserName === "firefox",
        "Quarantined: Firefox stalls until the watchdog fires (#1668)",
      );
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
        await dispatchPointer(page, { x: canvas.x + canvas.width * 0.24, y: canvas.y + 282 }, true);
        await expect(page.locator("canvas#canvas")).toHaveAttribute(
          "data-active-sample-id",
          "donner-splash",
        );
        checkpoint("waiting for Donner document");
        await waitForIdle(page);
        const start = await aimAtSplashD(page);
        checkpoint("selecting D");
        await dispatchPointer(page, start, true);
        await expect.poll(async () => (await snapshot(page)).interaction, { timeout: 15000 })
          .toEqual(
            expect.objectContaining({ selectedCount: 1, pendingClick: false, workerBusy: false }),
          );
        await page.waitForTimeout(800);
        report.before = await snapshot(page, true);
        checkpoint("sampling held drag");
        await installCompositedProbe(page, {
          sampleRegionCss: { x: start.x - 200, y: start.y - 130, width: 245, height: 175 },
          sampleWidth: 96,
          sampleHeight: 96,
          minColorAlpha: 64,
          minColorSpread: 60,
        });
        await startCompositedProbe(page);
        let stream:
          | { dispatchTimes: number[]; dispatchPoints: { x: number; y: number }[] }
          | undefined;
        let usableSamples = 0;
        let drawFraction = 0;
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
        expect(usableSamples).toBeGreaterThanOrEqual(process.env.CI ? 12 : 16);
        expect(drawFraction).toBeGreaterThan(0.8);
        const motion = report.motion as ReturnType<typeof contentMotionFraction>;
        expect(motion.fraction).toBeGreaterThanOrEqual(0.15);
      } finally {
        stopHeartbeat?.();
        await attachJson(info, "ui-presentation-diagnosis", report);
      }
    },
  );
});
