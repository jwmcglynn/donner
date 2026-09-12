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
  };
  __donnerViewportStats?: {
    paneX: number;
    paneY: number;
    paneWidth: number;
    paneHeight: number;
    zoom: number;
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
  await expect.poll(async () => (await snapshot(page)).interaction, {
    message: "the editor must consume pending input and finish its foreground render",
    timeout: 15000,
  }).toEqual(expect.objectContaining({ pendingClick: false, workerBusy: false }));
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
      for (const type of ["mousemove", "mousedown", "wheel"]) {
        window.addEventListener(type, (event) => {
          const pointer = event as MouseEvent;
          (window as Diagnostics).__donnerResponsivenessInput = {
            type,
            atMs: performance.now(),
            x: pointer.clientX,
            y: pointer.clientY,
          };
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
      await page.mouse.click(bounds!.x + bounds!.width * 0.24, bounds!.y + 282);
      await expect(canvas).toHaveAttribute("data-active-sample-id", "donner-splash");
      await expect.poll(async () => {
        const state = await snapshot(page);
        return (state.worker?.completedResults ?? 0) > (beforeSample.worker?.completedResults ?? 0)
          && state.worker?.presentedAtMs !== undefined;
      }, { timeout: 20000, message: "the clicked sample must reach a presenting frame" }).toBe(
        true,
      );
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
            await page.mouse.move(point.x, point.y);
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
