import type { Page, test as baseTest, TestInfo } from "@playwright/test";
import { writeFile } from "node:fs/promises";
import {
  describePreNavigationStall,
  startupPhaseReport,
  startupWaitMessage,
} from "./startup-phases.mjs";

/**
 * Failure-time evidence for cases that can capture a blank editor page.
 *
 * A blank capture cannot say on its own whether the application never drew, the presentation
 * never reached the canvas, or the browser lost the canvas contents in compositing. This keeps,
 * for the same moment, the presentation counters the editor publishes, a page screenshot, and one
 * direct readback of `canvas#canvas`. In Gecko a readback of a worker-presented WebGPU canvas can
 * itself come back near-empty, so an empty readback there is a data point, not a verdict.
 *
 * Nothing here runs on a passing case. A case opts in with armFailureCanvasEvidence; the hook
 * installed by installFailureCanvasEvidence requests no fixtures, so a test without a page starts
 * no browser, and it captures only after an armed case has already failed. A main-thread readback
 * of the worker-owned canvas, repeated every frame, has stalled Gecko (#1668), so the readback runs
 * once, last, and only when the page still answered the state read. Every step is bounded, so the
 * whole capture takes at most kFailureEvidenceBudgetMs even if the page stops answering.
 *
 * The state read also asks every worker of the page, in the same step, for what it publishes
 * about the browser GPU bridge: the owning worker's queue timeline, object statistics and owner
 * wait counters. Together with the raster worker's phase history from the page, that separates a
 * worker that stopped answering from one waiting on GPU work that never completed. A worker whose
 * JavaScript is blocked cannot answer; it is recorded as a timeout, with the time it was asked:
 * on the page clock when the page answered, otherwise in epoch milliseconds.
 *
 * An armed case also keeps the fatal console messages and uncaught page errors its page reports
 * (an Emscripten abort, a worker error), with stacks, and takes the same GPU state snapshot as the
 * first one arrives, since an abort can end the worker that would otherwise answer at capture.
 *
 * Every case of a spec that installs the hook also keeps the times its startup phases began
 * (startup-phases.mjs). A failed case prints them and an armed one keeps them in its evidence, so
 * a deadline that a stall before navigation consumed is reported as that stall, not as the wait
 * that happened to be running when the deadline arrived. The startup waits name the stall in their
 * own messages; when the deadline arrived elsewhere (inside the navigation itself, say), the hook
 * adds one error that names it, after the evidence is kept.
 */

/** Bound for each evidence step. */
export const kFailureEvidenceStepMs = 4_000;
/** Upper bound for the whole capture: state, screenshot and readback, one step each. */
export const kFailureEvidenceBudgetMs = 3 * kFailureEvidenceStepMs;

/** Longest edge of the readback PNG, in pixels; larger canvases are scaled down. */
const kReadbackMaxEdge = 1600;

const armedPages = new Map<string, Page>();
/** Startup phase marks of the running cases, epoch milliseconds, keyed by test id. */
const startupMarks = new Map<string, Record<string, number>>();
/**
 * Cases this worker process has armed. Every case that opens the editor arms, and the browser is a
 * worker fixture, so a case that starts while this is zero may be the one that launches it.
 */
let armedCases = 0;

/**
 * Record that the running case entered a startup phase (kStartupPhases in startup-phases.mjs).
 *
 * @param testInfo The case's test info.
 * @param phase The phase that starts now.
 */
export function markStartupPhase(testInfo: TestInfo, phase: string): void {
  const marks = startupMarks.get(testInfo.testId);
  if (marks !== undefined) marks[phase] = Date.now();
}

/**
 * A startup wait's message, led by the pre-navigation stall when one consumed the case's budget.
 *
 * @param testInfo The case's test info.
 * @param waitMessage What the wait expects.
 */
export function startupWait(testInfo: TestInfo, waitMessage: string): string {
  return startupWaitMessage(startupMarks.get(testInfo.testId) ?? {}, testInfo.timeout, waitMessage);
}

/** Console and page errors that signal a fatal runtime failure, as the specs' own filters do. */
const kFatalMessage =
  /Aborted|RuntimeError|Assertion failed|sent an error|UTILS_RELEASE_ASSERT|Failed to wake Wasm/i;
/**
 * Bounds on what is kept per case: the first messages, which usually name the cause, then the
 * latest ones up to the total, each cut to this many characters.
 */
const kKeptFirstFatalMessages = 10;
const kMaxFatalMessages = 40;
const kMaxFatalMessageChars = 4_000;

/**
 * Fatal messages an armed case's page reported, oldest first, and a snapshot of the GPU state
 * taken as the first one arrived: an abort can end the worker that would answer later.
 */
interface FatalRecord {
  messages: string[];
  firstSnapshot: Promise<unknown> | null;
}
const fatalRecords = new Map<string, FatalRecord>();
/** Pages whose fatal messages are already being kept; each is listened to once. */
const listenedPages = new WeakSet<Page>();

async function bounded<T>(work: Promise<T>, timeoutMs: number): Promise<T | "timeout"> {
  let timer: ReturnType<typeof setTimeout> | undefined;
  try {
    return await Promise.race([
      work,
      new Promise<"timeout">((resolve) => {
        timer = setTimeout(() => resolve("timeout"), timeoutMs);
      }),
    ]);
  } finally {
    if (timer !== undefined) clearTimeout(timer);
  }
}

async function attachFile(
  testInfo: TestInfo,
  name: string,
  body: Buffer | string,
  contentType: "image/png" | "application/json",
): Promise<void> {
  const path = testInfo.outputPath(`${name}.${contentType === "image/png" ? "png" : "json"}`);
  await writeFile(path, body);
  await testInfo.attach(name, { path, contentType });
}

interface CanvasReadback {
  width: number;
  height: number;
  readbackWidth: number;
  readbackHeight: number;
  opaquePixels: number;
  coloredPixels: number;
  pngBase64: string | null;
  error: string | null;
}

/** The page methods the capture uses, so a test can drive it with a page that never answers. */
export type EvidencePage = Pick<Page, "evaluate" | "screenshot" | "workers">;

/** Epoch milliseconds in `value` or its nested arrays, moved onto the page clock at `origin`. */
function onPageClock(value: unknown, origin: number): unknown {
  if (typeof value === "number") {
    return value > 1e12 ? Math.round((value - origin) * 10) / 10 : value;
  }
  if (Array.isArray(value)) return value.map((item) => onPageClock(item, origin));
  return value;
}

/** Worker answers with their epoch timestamps moved onto the page clock starting at `origin`. */
function workersOnPageClock(workers: unknown[], origin: number): unknown[] {
  return workers.map((worker) =>
    Object.fromEntries(
      Object.entries(worker as Record<string, unknown>).map(([key, value]) => [
        key,
        key === "gpuTimeline" || key === "answeredAt" || key === "askedAt"
          ? onPageClock(value, origin)
          : value,
      ]),
    )
  );
}

/**
 * What each worker of the page publishes about the browser GPU bridge, read on demand. Times are
 * epoch milliseconds; every read is bounded by `stepMs`.
 */
async function readWorkerGpuState(page: EvidencePage, stepMs: number): Promise<unknown[]> {
  return Promise.all(
    page.workers().map((worker) => {
      const askedAt = Date.now();
      return bounded(
        worker.evaluate(() => {
          const g = globalThis as unknown as Record<string, unknown>;
          const read = (name: string) =>
            typeof g[name] === "function" ? (g[name] as () => unknown)() : null;
          return {
            gpuOwner: g.__donnerGpuOwner === true,
            answeredAt: performance.timeOrigin + performance.now(),
            ownerWait: read("__donnerReadGpuOwnerWaitStats"),
            gpuObjects: read("__donnerReadGpuObjectStats"),
            gpuTimeline: read("__donnerReadGpuTimeline"),
          };
        }).catch((error: unknown) => ({ error: String(error) })),
        stepMs,
      ).then((answer) => (answer === "timeout" ? { timeout: true, askedAt } : answer));
    }),
  );
}

/**
 * Read `canvas#canvas` back once, attach the PNG, and return the counts for the record.
 *
 * @param page The failed case's page.
 * @param testInfo The failed case's test info.
 * @param stepMs Bound for the readback.
 */
async function captureCanvasReadback(
  page: EvidencePage,
  testInfo: TestInfo,
  stepMs: number,
): Promise<Record<string, unknown>> {
  const readbackStartedAt = Date.now();
  const readback = await bounded(
    page.evaluate((maxEdge): CanvasReadback => {
      const canvas = document.querySelector<HTMLCanvasElement>("canvas#canvas");
      const empty: CanvasReadback = {
        width: 0,
        height: 0,
        readbackWidth: 0,
        readbackHeight: 0,
        opaquePixels: 0,
        coloredPixels: 0,
        pngBase64: null,
        error: null,
      };
      if (canvas === null) return { ...empty, error: "no canvas#canvas" };
      const scale = Math.min(1, maxEdge / Math.max(canvas.width, canvas.height, 1));
      const readbackWidth = Math.max(1, Math.round(canvas.width * scale));
      const readbackHeight = Math.max(1, Math.round(canvas.height * scale));
      const scratch = document.createElement("canvas");
      scratch.width = readbackWidth;
      scratch.height = readbackHeight;
      const context = scratch.getContext("2d", { willReadFrequently: true });
      if (context === null) return { ...empty, error: "no 2d context" };
      try {
        context.drawImage(canvas, 0, 0, readbackWidth, readbackHeight);
        const pixels = context.getImageData(0, 0, readbackWidth, readbackHeight).data;
        let opaquePixels = 0;
        let coloredPixels = 0;
        for (let index = 0; index < pixels.length; index += 4) {
          if (pixels[index + 3] < 16) continue;
          opaquePixels += 1;
          const r = pixels[index];
          const g = pixels[index + 1];
          const b = pixels[index + 2];
          if (Math.max(r, g, b) - Math.min(r, g, b) >= 40) coloredPixels += 1;
        }
        const dataUrl = scratch.toDataURL("image/png");
        return {
          width: canvas.width,
          height: canvas.height,
          readbackWidth,
          readbackHeight,
          opaquePixels,
          coloredPixels,
          pngBase64: dataUrl.startsWith("data:image/png;base64,")
            ? dataUrl.slice("data:image/png;base64,".length)
            : null,
          error: null,
        };
      } catch (error) {
        return { ...empty, width: canvas.width, height: canvas.height, error: String(error) };
      }
    }, kReadbackMaxEdge).catch((error: unknown) => String(error)),
    stepMs,
  );
  const afterMs = Date.now() - readbackStartedAt;
  if (typeof readback === "string") return { error: readback, afterMs };
  const { pngBase64, ...counts } = readback;
  if (pngBase64 !== null) {
    await attachFile(
      testInfo,
      "failure-canvas-readback",
      Buffer.from(pngBase64, "base64"),
      "image/png",
    );
  }
  return { ...counts, afterMs };
}

/**
 * Capture the presentation state, a page screenshot and one canvas readback, and attach them.
 *
 * @param page The failed case's page.
 * @param testInfo The failed case's test info.
 * @param stepMs Bound for each step.
 * @param fatal The fatal messages the armed case's page reported, if any.
 * @param marks The case's startup phase marks, if it kept any.
 */
export async function captureFailureCanvasEvidence(
  page: EvidencePage,
  testInfo: TestInfo,
  stepMs = kFailureEvidenceStepMs,
  fatal?: FatalRecord,
  marks?: Record<string, number>,
): Promise<void> {
  const record: Record<string, unknown> = { stepMs };
  const startedAt = Date.now();
  // Awaited last, but bounded from here, so it shares the state step's time.
  const firstFatalSnapshot = fatal?.firstSnapshot ? bounded(fatal.firstSnapshot, stepMs) : null;

  const workersAnswer = readWorkerGpuState(page, stepMs);
  const state = await bounded(
    page.evaluate(() => {
      const w = window as unknown as Record<string, unknown>;
      return {
        timeOrigin: performance.timeOrigin,
        nowMs: performance.now(),
        rasterPhases: w.__donnerRasterWorkPhases ?? null,
        sampleThumbnails: w.__donnerSampleThumbnailStats ?? null,
        frameLoop: w.__donnerFrameLoopStats ?? null,
        presentationQueue: w.__donnerPresentationQueueStats ?? null,
        hostFrames: w.__donnerHostFrameTiming ?? null,
        worker: w.__donnerWorkerStats ?? null,
        viewport: w.__donnerViewportStats ?? null,
        overlay: w.__donnerOverlayStats ?? null,
        interaction: w.__donnerInteractionStats ?? null,
        firstFramePresented: w.__donnerFirstFramePresented ?? null,
        visibility: document.visibilityState,
      };
    }).catch((error: unknown) => ({ error: String(error) })),
    stepMs,
  );
  record.presentation = state;
  const pageTimeOrigin = typeof state === "object" && "timeOrigin" in state
    ? state.timeOrigin
    : null;
  if (marks !== undefined) {
    // Navigation's own start, on the page clock, places the page's age against the case's phases.
    record.startupPhases = startupPhaseReport(marks, testInfo.timeout, {
      navigationTimeOrigin: pageTimeOrigin,
      now: startedAt,
    });
  }
  const workers = await workersAnswer;
  record.workers = pageTimeOrigin === null ? workers : workersOnPageClock(workers, pageTimeOrigin);
  // The test output carries the tail of each history; the attached record keeps all of it.
  const tail = (value: unknown, count: number) =>
    Array.isArray(value) ? value.slice(-count) : value;
  console.log(`failure-gpu-state ${
    JSON.stringify({
      rasterPhases: typeof state === "object" && "rasterPhases" in state
        ? tail(state.rasterPhases, 16)
        : state,
      workers: (record.workers as Record<string, unknown>[]).map((worker) => ({
        ...worker,
        gpuTimeline: tail(worker.gpuTimeline, 32),
      })),
    })
  }`);

  const screenshotStartedAt = Date.now();
  const screenshot = await bounded(
    page.screenshot({ timeout: stepMs }).catch((error: unknown) => String(error)),
    stepMs,
  );
  if (Buffer.isBuffer(screenshot)) {
    await attachFile(testInfo, "failure-page-screenshot", screenshot, "image/png");
    record.screenshot = { afterMs: Date.now() - screenshotStartedAt };
  } else {
    record.screenshot = { error: screenshot };
  }

  record.readback = state === "timeout"
    ? { skipped: "the page did not answer the state read" }
    : await captureCanvasReadback(page, testInfo, stepMs);

  if (fatal !== undefined && fatal.messages.length > 0) {
    record.fatalMessages = fatal.messages;
    record.atFirstFatalMessage = firstFatalSnapshot === null ? null : await firstFatalSnapshot;
    console.log(`failure-fatal-messages ${JSON.stringify(fatal.messages)}`);
  }

  record.totalMs = Date.now() - startedAt;
  await attachFile(
    testInfo,
    "failure-canvas-evidence",
    JSON.stringify(record, null, 2),
    "application/json",
  );
}

/**
 * Keep failure-time canvas evidence for the running case if it fails.
 *
 * @param page The case's page.
 * @param testInfo The case's test info.
 */
export function armFailureCanvasEvidence(page: Page, testInfo: TestInfo): void {
  if (!armedPages.has(testInfo.testId)) armedCases += 1;
  armedPages.set(testInfo.testId, page);
  if (listenedPages.has(page)) return;
  listenedPages.add(page);
  const fatal: FatalRecord = fatalRecords.get(testInfo.testId)
    ?? { messages: [], firstSnapshot: null };
  fatalRecords.set(testInfo.testId, fatal);
  const keep = (text: string) => {
    if (!kFatalMessage.test(text)) return;
    fatal.messages.push(text.slice(0, kMaxFatalMessageChars));
    if (fatal.messages.length > kMaxFatalMessages) {
      fatal.messages.splice(kKeptFirstFatalMessages, 1);
    }
    fatal.firstSnapshot ??= snapshotGpuState(page, kFailureEvidenceStepMs);
  };
  page.on("console", (message) => {
    const { url, lineNumber } = message.location();
    keep(`[console:${message.type()}] ${message.text()} (${url}:${lineNumber})`);
  });
  // An uncaught error keeps its stack, which for a Wasm abort names the frames that reached it.
  page.on("pageerror", (error) => keep(`[pageerror] ${error.stack ?? error.message}`));
}

/** The raster phases and every worker's GPU state, read now and bounded by `stepMs` per read. */
async function snapshotGpuState(page: EvidencePage, stepMs: number): Promise<unknown> {
  const workersAnswer = readWorkerGpuState(page, stepMs);
  const state = await bounded(
    page.evaluate(() => ({
      timeOrigin: performance.timeOrigin,
      nowMs: performance.now(),
      rasterPhases: (window as unknown as Record<string, unknown>).__donnerRasterWorkPhases ?? null,
    })).catch((error: unknown) => ({ error: String(error) })),
    stepMs,
  );
  const workers = await workersAnswer;
  return {
    page: state,
    workers: typeof state === "object" && "timeOrigin" in state
      ? workersOnPageClock(workers, state.timeOrigin)
      : workers,
  };
}

/**
 * Install the hook that captures failure evidence for armed cases. It requests no fixtures.
 *
 * @param test The spec's test object.
 */
export function installFailureCanvasEvidence(test: typeof baseTest): void {
  // Registered before the spec's own hooks, and requesting no fixtures, so it runs before the
  // page is set up: the first mark is as close to the start of the case's budget as a hook gets.
  test.beforeEach(() => {
    startupMarks.set(test.info().testId, {
      testStart: Date.now(),
      firstCaseInWorker: armedCases === 0 ? 1 : 0,
    });
  });
  test.afterEach(async () => {
    const testInfo = test.info();
    const page = armedPages.get(testInfo.testId);
    const fatal = fatalRecords.get(testInfo.testId);
    const marks = startupMarks.get(testInfo.testId);
    armedPages.delete(testInfo.testId);
    fatalRecords.delete(testInfo.testId);
    startupMarks.delete(testInfo.testId);
    if (testInfo.status === testInfo.expectedStatus) return;
    const report = marks === undefined ? null : startupPhaseReport(marks, testInfo.timeout);
    if (report !== null) console.log(`failure-startup-phases ${JSON.stringify(report)}`);
    if (page !== undefined) {
      try {
        await captureFailureCanvasEvidence(page, testInfo, kFailureEvidenceStepMs, fatal, marks);
      } catch (error) {
        console.error(`failure canvas evidence capture failed: ${String(error)}`);
      }
    }
    // Only an already timed-out case gets this error, and only when no error names the stall yet.
    // A case that may have launched the browser keeps the stall in its log line only: its figure
    // is an upper bound that can include time the budget did not count.
    const stall = report === null || report.firstCaseInWorker
      ? null
      : describePreNavigationStall(report);
    if (
      stall !== null && testInfo.status === "timedOut"
      && !testInfo.errors.some((error) => error.message?.includes("pre-navigation stall"))
    ) {
      throw new Error(
        `${stall}; the test timeout above was caused by this stall, not by the step that was`
          + " running when it arrived",
      );
    }
  });
}
