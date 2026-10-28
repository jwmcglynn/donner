import { type Page, test, type TestInfo, type Worker } from "@playwright/test";
import { writeFile } from "node:fs/promises";
import {
  armCanvasGpuDelayFromEnv,
  type CanvasActivity,
  type CanvasGpuDelayReport,
  readCanvasActivity,
} from "./surface-frame-probe";

/**
 * Fault injection for the scored canvas captures, off unless one of these is set:
 *
 * - `DONNER_CANVAS_CAPTURE_GPU_DELAY=<everyFrames>:<iterations>` queues fixed GPU work after the
 *   first canvas write of every Nth frame and, unless the caller holds a button down, keeps moving
 *   the pointer over the canvas's top-left corner while a capture runs. The editor then keeps
 *   presenting frames, so canvas GPU work is still running, or a new frame arrives, around the
 *   screenshot. It also
 *   checks the capture that the gate accepted: the GPU had finished every canvas write when its
 *   screenshot started, and no canvas frame was acquired or written until the screenshot ended.
 * - `DONNER_CANVAS_CAPTURE_STALL_MS=<ms>` keeps the page's main thread busy for that long once per
 *   test, just before its first scored screenshot, as a loaded host can. The test's timeout grows
 *   by the same amount; no capture bound does.
 */
const kCanvasCaptureGpuDelayEnv = "DONNER_CANVAS_CAPTURE_GPU_DELAY";
const kCanvasCaptureStallEnv = "DONNER_CANVAS_CAPTURE_STALL_MS";

/** Canvas activity around one screenshot taken under the fault. */
interface CaptureObservation {
  before: CanvasActivity;
  after: CanvasActivity;
}

function captureStallMs(): number {
  const value = process.env[kCanvasCaptureStallEnv];
  if (value === undefined || value === "") return 0;
  const ms = Number(value);
  if (!Number.isInteger(ms) || ms < 1 || ms > 60_000) {
    throw new Error(`${kCanvasCaptureStallEnv} must be an integer from 1 to 60000`);
  }
  return ms;
}

const stalledTests = new Set<string>();
let faultSequence = 0;

/**
 * Arm the fault for one gated capture of \p owner's canvas in \p page. Returns null when neither
 * variable is set. `wrap` instruments the gate's screenshot callback; `finish` ends the fault,
 * attaches what it observed and, on a successful capture, fails if the accepted screenshot
 * overlapped canvas GPU work or a new canvas frame.
 */
export async function armCanvasCaptureFault(
  page: Page,
  owner: Worker,
  options: { pointerChurn?: boolean } = {},
) {
  const testInfo: TestInfo = test.info();
  const stallMs = stalledTests.has(testInfo.testId) ? 0 : captureStallMs();
  const delay = await armCanvasGpuDelayFromEnv(owner, kCanvasCaptureGpuDelayEnv);
  if (stallMs === 0 && delay === null) return null;
  if (stallMs > 0) {
    stalledTests.add(testInfo.testId);
    testInfo.setTimeout(testInfo.timeout + stallMs);
  }
  // Pointer moves over an empty corner of the canvas: each one is input the next UI frame
  // represents, so frames keep arriving while the gate waits and while the screenshot runs.
  let moving: Promise<void> | null = null;
  let stopMoving = false;
  if (delay !== null && (options.pointerChurn ?? true)) {
    const canvas = await page.locator("canvas#canvas").boundingBox().catch(() => null);
    if (canvas !== null) {
      moving = (async () => {
        for (let step = 0; !stopMoving; ++step) {
          await page.mouse.move(canvas.x + 8 + (step % 2), canvas.y + 8).catch(() => {});
          await new Promise((resolve) => setTimeout(resolve, 16));
        }
      })();
    }
  }
  let stallPending = stallMs > 0;
  const observations: CaptureObservation[] = [];
  return {
    wrap<T>(capture: (timeoutMs: number) => Promise<T>): (timeoutMs: number) => Promise<T> {
      return async (timeoutMs) => {
        if (stallPending) {
          stallPending = false;
          // Returns at once; the busy loop runs in the next page task, ahead of the screenshot.
          await page.evaluate((busyMs) => {
            setTimeout(() => {
              const end = performance.now() + busyMs;
              while (performance.now() < end) {
                // Busy: the page cannot produce the frame the screenshot waits for.
              }
            }, 0);
          }, stallMs);
        }
        if (delay === null) return capture(timeoutMs);
        const before = await readCanvasActivity(owner);
        const result = await capture(timeoutMs);
        const after = await readCanvasActivity(owner);
        observations.push({ before, after });
        return result;
      };
    },
    async finish(captured: boolean): Promise<void> {
      stopMoving = true;
      await moving;
      const report: CanvasGpuDelayReport | null = delay === null ? null : await delay.finish();
      const path = testInfo.outputPath(`canvas-capture-fault-${++faultSequence}.json`);
      await writeFile(
        path,
        JSON.stringify({ stallMs, delay: report, captured, observations }, null, 2),
      );
      await testInfo.attach("canvas-capture-fault", { path, contentType: "application/json" });
      const accepted = observations.at(-1);
      if (!captured || accepted === undefined) return;
      const { before, after } = accepted;
      if (
        before.canvasRetired !== before.canvasSubmits || after.canvasSubmits !== before.canvasSubmits
        || after.frames !== before.frames
      ) {
        throw new Error(
          `the accepted capture overlapped canvas GPU work or a new canvas frame: ${
            JSON.stringify(accepted)
          }`,
        );
      }
    },
  };
}
