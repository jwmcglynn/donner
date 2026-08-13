// Frame-budget and memory harness for the single-canvas whole-app-in-worker
// editor (the single-canvas presenter work exit criteria).
//
// The composited-invariant lane answers "is the output correct"; this answers
// "does the frame budget hold" and "does memory stay bounded", both of which
// need a headed stock browser on real hardware. Playwright's bundled Firefox
// and headless Chromium are both disqualified for perf (SwiftShader / non-stock
// builds), so this drives `channel: "chromium"` and stock Firefox headed,
// exactly like the composited lane's configs.
//
// Reports, for a burst zoom storm plus an idle soak on the Donner Splash:
//   - which frame driver the app thread got (worker rAF vs proxied main-thread
//     rAF vs the setTimeout fallback) and its inter-tick jitter,
//   - UI frame p50/p99/max and the counts over the 8.33 ms budget and the
//     16.7 ms hard ceiling,
//   - ASYNCIFY suspend attribution per frame, broken down by suspend kind,
//   - linear memory, live malloc bytes, and per-subsystem retained bytes with
//     their high waters (see `donner/base/MemoryAttribution.h`).
//
// Memory is a correctness bound here, not a tuning number: the build links with
// `-sMAXIMUM_MEMORY=512MB`, and a heap that reaches it aborts the module
// mid-gesture. Stock Firefox does exactly that on an uninstrumented build, so
// the harness watches for the abort explicitly and reports how far the storm
// got instead of hanging on a dead page.
//
// Exit criteria (the single-canvas presenter work): UI frame p99 < 8.33 ms, no frame over
// 16.7 ms, suspend overhead < 1 ms/frame p99, heap bounded well under the
// 512 MiB ceiling and flat across the soak.
//
// DEVICE PIXEL RATIO IS PART OF THE WORKLOAD, NOT PART OF THE HARNESS.
// Every canvas-scale allocation in this app is proportional to DPR squared, so
// two engines at different DPRs are not running the same test. Playwright's
// Chromium forces `devicePixelRatio` to the context's `deviceScaleFactor`
// (default 1) over CDP; Playwright's Firefox does not, and reports the physical
// display's ratio - 2 on a Retina machine. Measured on the reference machine at
// a 1600x900 viewport: Chromium's canvas backing store is 1600x900 (5.76 MB),
// Firefox's is 3200x1800 (23.04 MB). A Chromium run left at the default was
// therefore measuring one quarter of the raster load a real user on the same
// display gets, on either engine. Always pass an explicit ratio, and state it
// next to any number this harness produces.
//
// Usage, with the package served cross-origin-isolated:
//   node frame-budget-harness.mjs <chromium|firefox> <baseUrl> \
//       [stormSeconds] [soakSeconds] [outJsonPath] [deviceScaleFactor]

import { writeFileSync } from "node:fs";
import { chromium, firefox } from "playwright";

const engine = process.argv[2] || "chromium";
const baseUrl = process.argv[3] || "http://127.0.0.1:8099";
const stormSeconds = Number(process.argv[4] || 10);
const soakSeconds = Number(process.argv[5] || 0);
const outJsonPath = process.argv[6] || "";
// Chromium honours this. Firefox ignores it and uses the display's own ratio,
// which is why the run reports the ratio it actually got rather than the one it
// asked for.
const requestedDeviceScaleFactor = Number(process.argv[7] || 1);

const launcher = engine === "firefox" ? firefox : chromium;
const launchOptions = engine === "firefox"
  ? { headless: false }
  : {
    headless: false,
    channel: "chromium",
    args: ["--enable-unsafe-webgpu", "--use-angle=metal", "--ignore-certificate-errors"],
  };

const browser = await launcher.launch(launchOptions);
const context = await browser.newContext({
  viewport: { width: 1600, height: 900 },
  deviceScaleFactor: requestedDeviceScaleFactor,
});
const page = await context.newPage();

// Once the module aborts, every `page.evaluate` in the teardown path waits out
// Playwright's 30 s default against a page that will never answer. The run is
// already over at that point; the only thing left to do is write down how far
// it got, so give the dead-page path a deadline measured in seconds.
page.setDefaultTimeout(8000);

// A module abort is the failure this harness exists to catch, so it is recorded
// rather than merely logged: every later step checks it and degrades to a
// partial report instead of timing out against a dead page.
const failures = [];
let aborted = false;
const noteFailure = (source, text) => {
  if (/abort|Aborted|out of memory|OOM|Maximum call stack|RuntimeError/i.test(text)) {
    aborted = true;
  }
  failures.push(`${source}: ${text}`);
};

page.on("console", (message) => {
  const text = message.text();
  if (/error|Error|fail|abort|Abort/.test(text)) {
    console.log(`  [console] ${text}`);
    noteFailure("console", text);
  }
});
page.on("pageerror", (error) => {
  console.log(`  [pageerror] ${error.message}`);
  noteFailure("pageerror", error.message);
});

// Wall-clock per phase. A Gecko run that takes six minutes to do thirty seconds
// of work is itself a finding, and without this the time disappears into "the
// harness was slow".
const runStart = Date.now();
const phaseMs = {};
const markPhase = (name) => {
  phaseMs[name] = Date.now() - runStart;
  console.log(`  [phase] ${name} at ${phaseMs[name]} ms`);
};

const percentileOf = (values, fraction) => {
  if (!values || values.length === 0) return 0;
  const sorted = [...values].sort((a, b) => a - b);
  const index = Math.min(sorted.length - 1, Math.floor(fraction * sorted.length));
  return sorted[index];
};

// `page.evaluate` is not governed by the page's default timeout, so every call
// that outlives the module needs its own deadline or the run hangs against a
// dead page. Declared before its first use so the storm loop and the teardown
// reads share one policy.
const withDeadline = async (promise, ms, label) =>
  Promise.race([
    promise,
    new Promise((_, reject) => setTimeout(() => reject(new Error(`${label} deadline`)), ms)),
  ]);

const readMemory = async () => {
  try {
    return await withDeadline(page.evaluate(() => {
      const stats = window.__donnerMemoryStats;
      if (!stats) return null;
      const suspend = window.__donnerAsyncifySuspendStats;
      const copy = JSON.parse(JSON.stringify(stats));
      // Suspends travel with the memory sample rather than in their own series:
      // "how much did the heap grow between two suspend counts" is the question
      // hypothesis (c) turns on, and it is unanswerable if the two are sampled
      // at different instants.
      copy.suspendsSoFar = suspend ? suspend.suspends : 0;
      copy.suspendFramesSoFar = suspend ? suspend.frames : 0;
      copy.renderedFramesSoFar = window.__donnerFrameLoopStats
        ? window.__donnerFrameLoopStats.renderedFrames
        : 0;
      // Sampled here rather than in the end-of-run report: the run this harness
      // exists to characterise ends with a dead page, and a breakdown that can
      // only be read after the module survives is a breakdown of the case that
      // was never in doubt.
      copy.hostFrameTiming = window.__donnerHostFrameTiming
        ? JSON.parse(JSON.stringify(window.__donnerHostFrameTiming))
        : null;
      copy.imguiDrawStats = window.__donnerImGuiDrawStats
        ? JSON.parse(JSON.stringify(window.__donnerImGuiDrawStats))
        : null;
      copy.workerStats = window.__donnerWorkerStats
        ? JSON.parse(JSON.stringify(window.__donnerWorkerStats))
        : null;
      return copy;
    }), 8000, "readMemory");
  } catch (error) {
    noteFailure("readMemory", String(error && error.message));
    return null;
  }
};

// A compact row per sample. The full sample is kept separately (see
// `lastFullSample`): a per-slice full copy of the size histogram would be
// megabytes of JSON for a 30 s run, while these are the columns an attribution
// table is actually read from.
const traceRow = (memory, second) => ({
  second,
  wasmHeapBytes: memory.wasmHeapBytes,
  mallocLiveBytes: memory.mallocLiveBytes,
  mallocFreeBytes: memory.mallocFreeBytes,
  mallocArenaBytes: memory.mallocArenaBytes,
  totalRetainedBytes: memory.totalRetainedBytes,
  suspends: memory.suspendsSoFar,
  renderedFrames: memory.renderedFramesSoFar,
  liveLargeBytes: memory.liveLargeBytes || [],
  stageCumulativeNetBytes: Object.fromEntries(
    Object.entries(memory.stages || {}).map(([name, s]) => [name, s.cumulativeNetBytes]),
  ),
  categoryRetainedBytes: Object.fromEntries(
    Object.entries(memory.categories || {})
      .filter(([, c]) => c.retainedBytes > 0)
      .map(([name, c]) => [name, c.retainedBytes]),
  ),
});

// The abort kills the page, so the last sample that answers "what did the heap
// look like just before it died" has to already be in hand when it happens.
let lastFullSample = null;

const bootStart = Date.now();
await page.goto(baseUrl, { waitUntil: "domcontentloaded" });

await page.waitForFunction(() => window.__donnerFirstFramePresented === true, null, {
  timeout: 60000,
});
const bootMs = Date.now() - bootStart;
markPhase("boot");
console.log(`boot: first frame at ${bootMs} ms`);

// The ratio the engine actually applied, and the canvas backing store it
// produced. Every byte in the tables below scales with this.
const raster = await page.evaluate(() => {
  const canvas = document.getElementById("canvas");
  return {
    devicePixelRatio: window.devicePixelRatio,
    canvasBackingWidth: canvas.width,
    canvasBackingHeight: canvas.height,
    canvasBackingRgbaBytes: canvas.width * canvas.height * 4,
  };
});
console.log(
  `raster: dpr=${raster.devicePixelRatio} (requested ${requestedDeviceScaleFactor})` +
    ` backing=${raster.canvasBackingWidth}x${raster.canvasBackingHeight}` +
    ` (${(raster.canvasBackingRgbaBytes / 1048576).toFixed(2)} MiB)`,
);

// Report the driver arm as soon as one frame has published stats.
await page.waitForFunction(() => Boolean(window.__donnerFrameTickStats), null, { timeout: 20000 });
const driver = await page.evaluate(() => window.__donnerFrameDriver);
const workerRafSupported = await page.evaluate(() => window.__donnerFrameTickStats?.driver);
console.log(`frame driver: ${driver} (published: ${workerRafSupported})`);

markPhase("driverRead");
const memoryAtBoot = await readMemory();
markPhase("memoryAtBoot");

// Open the Donner Splash from the welcome carousel with a real (trusted) click,
// then park the pointer over the render pane: the pane classifies wheel input
// only while it is the hovered window.
const box = await page.locator("canvas#canvas").boundingBox();
await page.mouse.click(box.x + box.width * 0.24, box.y + 282);
await page.waitForTimeout(1500);
await page.mouse.move(box.x + box.width * 0.5, box.y + box.height * 0.55);
await page.waitForTimeout(400);

const opened = await page.evaluate(() => ({
  suspends: window.__donnerAsyncifySuspendStats?.suspends || 0,
  rendered: window.__donnerFrameLoopStats?.renderedFrames || 0,
}));
console.log(`after opening splash: suspends=${opened.suspends} renderedFrames=${opened.rendered}`);

const memoryAtSplash = await readMemory();
markPhase("splash");

// Reset the sample arrays so the storm is measured on its own.
await page.evaluate(() => {
  const loop = window.__donnerFrameLoopStats;
  if (loop) {
    loop.uiFrameMsSamples.length = 0;
  }
  const suspend = window.__donnerAsyncifySuspendStats;
  if (suspend) {
    suspend.frameMsSamples.length = 0;
    suspend.frameCountSamples.length = 0;
  }
  if (window.__donnerMemorySeries) {
    window.__donnerMemorySeries.length = 0;
  }
});

// Burst zoom storm over the canvas centre: ctrl-wheel notches paced inside the
// page so the inter-event gap is timer resolution, not a CDP round trip. Driven
// in one-second slices from the harness so an abort mid-storm ends the loop
// with a partial report rather than a 10-second hang on a dead page.
const stormTrace = [];
const runStormSlice = async (seconds, indexBase) =>
  page.evaluate(async ([sliceSeconds, base]) => {
    const canvas = document.getElementById("canvas");
    const rect = canvas.getBoundingClientRect();
    const x = rect.left + rect.width / 2;
    const y = rect.top + rect.height / 2;
    const deadline = performance.now() + sliceSeconds * 1000;
    let direction = -1;
    let index = base;
    while (performance.now() < deadline) {
      for (let notch = 0; notch < 6; ++notch) {
        canvas.dispatchEvent(new WheelEvent("wheel", {
          bubbles: true,
          cancelable: true,
          clientX: x + Math.sin(index * 0.7) * 0.5,
          clientY: y + Math.cos(index * 0.9) * 0.5,
          ctrlKey: true,
          deltaMode: 0,
          deltaY: direction * 42,
        }));
        ++index;
      }
      direction = -direction;
      await new Promise((resolve) => requestAnimationFrame(resolve));
    }
    return index;
  }, [seconds, indexBase]);

// The heap can go from flat to the ceiling inside one second, so the trace is
// sampled four times per second and the single worst sample is kept whole: an
// attribution table built from a post-hoc read would describe the state after
// whatever released, not the state that hit the ceiling.
const kSliceSeconds = 0.25;
let peakSample = null;
const notePeak = (memory, phase, elapsedSeconds) => {
  if (!memory) return;
  if (peakSample === null || memory.mallocLiveBytes > peakSample.memory.mallocLiveBytes) {
    peakSample = { phase, elapsedSeconds, memory };
  }
};

let stormCompletedSeconds = 0;
let notchIndex = 0;
const stormSlices = Math.round(stormSeconds / kSliceSeconds);
for (let slice = 0; slice < stormSlices && !aborted; ++slice) {
  try {
    notchIndex = await withDeadline(
      runStormSlice(kSliceSeconds, notchIndex),
      Math.max(5000, kSliceSeconds * 8000),
      "storm slice",
    );
  } catch (error) {
    noteFailure("storm", String(error && error.message));
    break;
  }
  stormCompletedSeconds = (slice + 1) * kSliceSeconds;
  const memory = await readMemory();
  notePeak(memory, "storm", stormCompletedSeconds);
  if (memory) {
    lastFullSample = { phase: "storm", elapsedSeconds: stormCompletedSeconds, memory };
    stormTrace.push(traceRow(memory, stormCompletedSeconds));
  }
}
markPhase("stormEnd");
console.log(`storm: ${stormCompletedSeconds}/${stormSeconds} s completed (aborted=${aborted})`);

// Let the last frames drain.
if (!aborted) {
  await page.waitForTimeout(500);
}
const memoryAfterStorm = await readMemory();

// Idle soak: no input at all. Anything that keeps growing here is retention the
// storm created and nothing releases, as distinct from per-frame churn.
const soakTrace = [];
let soakCompletedSeconds = 0;
for (let second = 0; second < soakSeconds && !aborted; ++second) {
  await page.waitForTimeout(1000);
  soakCompletedSeconds = second + 1;
  const memory = await readMemory();
  notePeak(memory, "soak", soakCompletedSeconds);
  if (memory) {
    lastFullSample = { phase: "soak", elapsedSeconds: soakCompletedSeconds, memory };
    soakTrace.push(traceRow(memory, soakCompletedSeconds));
  }
}
if (soakSeconds > 0) {
  console.log(`soak: ${soakCompletedSeconds}/${soakSeconds} s completed (aborted=${aborted})`);
}

markPhase("soakEnd");
let report = {};
try {
  report = await withDeadline(page.evaluate(() => {
    const percentile = (values, fraction) => {
      if (!values || values.length === 0) return 0;
      const sorted = [...values].sort((a, b) => a - b);
      const index = Math.min(sorted.length - 1, Math.floor(fraction * sorted.length));
      return sorted[index];
    };
    const loop = window.__donnerFrameLoopStats || {};
    const suspend = window.__donnerAsyncifySuspendStats || {};
    const ui = loop.uiFrameMsSamples || [];
    const suspendSamples = suspend.frameMsSamples || [];
    return {
      tick: window.__donnerFrameTickStats,
      // Where the UI frame's milliseconds and the heap's largest blocks come
      // from: the host present's internal split, and the size of the ImGui draw
      // data each frame hands the backend.
      hostFrameTiming: window.__donnerHostFrameTiming
        ? JSON.parse(JSON.stringify(window.__donnerHostFrameTiming))
        : null,
      imguiDrawStats: window.__donnerImGuiDrawStats
        ? JSON.parse(JSON.stringify(window.__donnerImGuiDrawStats))
        : null,
      workerStats: window.__donnerWorkerStats
        ? JSON.parse(JSON.stringify(window.__donnerWorkerStats))
        : null,
      // The bridge publishes its name tables as JS literals, so a stale literal
      // silently mislabels every row of the attribution table. These flags are
      // the runtime half of that guard (the other half is a native test).
      nameTableMismatch: {
        categories: Boolean(window.__donnerMemoryCategoryMismatch),
        stages: Boolean(window.__donnerMemoryStageMismatch),
        allocTags: Boolean(window.__donnerAllocTagMismatch),
      },
      heapBytes: window.__donnerHeapBytes,
      heapHighWaterBytes: window.__donnerHeapBytesHighWater,
      memory: window.__donnerMemoryStats
        ? JSON.parse(JSON.stringify(window.__donnerMemoryStats))
        : null,
      frames: {
        count: ui.length,
        p50: percentile(ui, 0.5),
        p99: percentile(ui, 0.99),
        max: ui.length ? Math.max(...ui) : 0,
        over8_33: ui.filter((v) => v > 8.33).length,
        over16_7: ui.filter((v) => v > 16.7).length,
      },
      suspend: {
        frames: suspend.frames,
        totalSuspends: suspend.suspends,
        p50Ms: percentile(suspendSamples, 0.5),
        p99Ms: percentile(suspendSamples, 0.99),
        maxMs: suspend.longestMs,
        tileYieldMs: suspend.tileYieldMs,
        gpuReadbackMs: suspend.gpuReadbackMs,
        deviceWaitMs: suspend.deviceWaitMs,
        startupMs: suspend.startupMs,
      },
    };
  }), 8000, "report");
} catch (error) {
  noteFailure("report", String(error && error.message));
}

markPhase("reportEnd");
const output = {
  phaseMs,
  engine,
  requestedDeviceScaleFactor,
  raster,
  bootMs,
  driver,
  aborted,
  stormSeconds,
  stormCompletedSeconds,
  soakSeconds,
  soakCompletedSeconds,
  failures,
  memoryAtBoot,
  memoryAtSplash,
  memoryAfterStorm,
  peakSample,
  lastFullSample,
  stormTrace,
  soakTrace,
  ...report,
};

console.log(JSON.stringify(output, null, 2));
if (outJsonPath) {
  writeFileSync(outJsonPath, JSON.stringify(output, null, 2));
}
// A browser whose content process died can leave `close()` waiting forever.
// The report is already written; do not let teardown be the thing that turns a
// measured failure into a hung run.
await Promise.race([
  browser.close().catch(() => {}),
  new Promise((resolve) => setTimeout(resolve, 10000)),
]);
process.exit(aborted ? 2 : 0);
