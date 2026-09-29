import { expect, type Page, test } from "@playwright/test";

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
  __donnerSampleThumbnailStats?: { pending: boolean; active: boolean; resultReady: boolean };
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

async function aimAtSplashD(page: Page) {
  const viewport = (await snapshot(page)).viewport!;
  const point = {
    x: Math.round(viewport.documentX + viewport.documentWidth * 282 / 892),
    y: Math.round(viewport.documentY + viewport.documentHeight * 390 / 512),
  };
  expect(point.x).toBeGreaterThan(viewport.paneX);
  expect(point.x).toBeLessThan(viewport.paneX + viewport.paneWidth);
  expect(point.y).toBeGreaterThan(viewport.paneY);
  expect(point.y).toBeLessThan(viewport.paneY + viewport.paneHeight);
  await dispatchPointer(page, point);
  await expect.poll(async () => (await snapshot(page)).interaction, {
    timeout: 10000,
    message: "the D aiming move must be applied before a press or zoom",
  }).toEqual(expect.objectContaining({ pointerX: point.x, pointerY: point.y }));
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

async function continuousDrag(page: Page, start: { x: number; y: number }, direction: number) {
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
    }[] = [];
    const dispatchTimes: number[] = [];
    let previousFrame = state.__donnerFrameLoopStats?.renderedFrames ?? 0;
    let observing = true;
    let sampleId = 0;
    const observe = () => {
      const loop = state.__donnerFrameLoopStats;
      if (loop && loop.renderedFrames !== previousFrame) {
        previousFrame = loop.renderedFrames;
        frames.push({
          frame: loop.renderedFrames,
          atMs: loop.lastFrameAtMs,
          pointerX: state.__donnerInteractionStats?.pointerX,
          pointerY: state.__donnerInteractionStats?.pointerY,
          selectedCount: state.__donnerInteractionStats?.selectedCount,
          dragging: state.__donnerInteractionStats?.dragging,
          workerMs: state.__donnerWorkerStats?.workerMs,
          completedResults: state.__donnerWorkerStats?.completedResults,
          readbackCount: state.__donnerWorkerStats?.readbackCount,
          readbackPollIterations: state.__donnerWorkerStats?.readbackPollIterations,
        });
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
          dispatch("mousemove", end.x, end.y, 1);
          if (step < 60) setTimeout(move, 16);
          else resolve();
        };
        setTimeout(move, 16);
      });
    } finally {
      dispatch("mouseup", end.x, end.y, 0);
      observing = false;
      cancelAnimationFrame(sampleId);
    }
    return { startedAtMs, finishedAtMs: performance.now(), dispatchTimes, frames, end };
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
  expect(after.interaction?.selectedCount, "the D drag must leave one shape selected").toBe(1);
  expect(stream.frames.some((frame) => frame.dragging), "the stream must enter drag mode").toBe(
    true,
  );
  const gaps = (times: number[]) => times.slice(1).map((time, index) => time - times[index]);
  return {
    ...phaseReport(before, after, []),
    // These are observed frame gaps, not a latched input-to-presentation latency measurement.
    observedFrameGapsMs: distribution(gaps(stream.frames.map((frame) => frame.atMs))),
    inputDispatchGapsMs: distribution(gaps(stream.dispatchTimes)),
    observedFrames: stream.frames.length,
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
    const report: Record<string, unknown> = {
      browser: info.project.name,
      version: browser.version(),
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
      expect(raster).toEqual({
        dpr: 2,
        backingWidth: 2560,
        backingHeight: 1800,
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
          await expect.poll(async () => {
            const current = await snapshot(page);
            const changed = kind === "pointer"
              ? current.interaction?.pointerX === point.x
                && current.interaction?.pointerY === point.y
              : current.viewport?.zoom !== prior.viewport?.zoom;
            const ready = changed
              && (current.frameLoop?.renderedFrames ?? 0) > (prior.frameLoop?.renderedFrames ?? 0)
              && (current.frameLoop?.lastFrameAtMs ?? 0) >= (current.input?.atMs ?? Infinity);
            if (ready) presented = current;
            return ready;
          }, {
            timeout: 10000,
            intervals: [8, 16, 32],
            message: `${phase} step ${step} must present`,
          })
            .toBe(true);
          latencies.push(presented!.frameLoop!.lastFrameAtMs - presented!.input!.atMs);
        }
        report[phase] = phaseReport(before, await snapshot(page), latencies);
        console.log(
          `responsiveness ${phase} ${info.project.name} ${JSON.stringify(report[phase])}`,
        );
        await waitForIdle(page);
      }
      const dragStart = await zoomSplashForDrag(page);
      const firstDrag = await continuousDrag(page, dragStart, -1);
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
      const secondDrag = await continuousDrag(page, firstDrag.stream.end, 1);
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
      expect(failures).toEqual([]);
    } finally {
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
      await info.attach("responsiveness.json", {
        body: JSON.stringify(report, null, 2),
        contentType: "application/json",
      });
    }
  },
);
