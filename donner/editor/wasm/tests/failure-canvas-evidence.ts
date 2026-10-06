import type { Page, test as baseTest, TestInfo } from "@playwright/test";
import { writeFile } from "node:fs/promises";

/**
 * Failure-time evidence for cases that can capture a blank editor page.
 *
 * A blank capture cannot say on its own whether the application never drew, the presentation
 * never reached the canvas, or the browser lost the canvas contents in compositing. Next to the
 * page screenshot this keeps one direct readback of `canvas#canvas` and the presentation counters
 * the editor publishes, so the three can be compared for the same moment.
 *
 * The readback runs only after the case has already failed, never on a passing run: a readback of
 * the worker-owned canvas from the main thread can stall Gecko when it is repeated every frame, so
 * it must never be able to turn a pass into a failure. Every step is bounded, and the whole capture
 * finishes within kFailureEvidenceBudgetMs even if the page stops answering.
 */

/** Bound for each evidence step. */
export const kFailureEvidenceStepMs = 5_000;
/** Upper bound for the whole capture: screenshot, readback and state, one step each. */
export const kFailureEvidenceBudgetMs = 3 * kFailureEvidenceStepMs;

/** Longest edge of the readback PNG, in pixels; larger canvases are scaled down. */
const kReadbackMaxEdge = 1600;

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

/**
 * Capture a page screenshot, one canvas readback and the presentation state, and attach them.
 *
 * @param page The failed case's page.
 * @param testInfo The failed case's test info.
 * @param stepMs Bound for each step.
 */
export async function captureFailureCanvasEvidence(
  page: Page,
  testInfo: TestInfo,
  stepMs = kFailureEvidenceStepMs,
): Promise<void> {
  const record: Record<string, unknown> = { stepMs };
  const startedAt = Date.now();

  const screenshot = await bounded(
    page.screenshot({ timeout: stepMs }).catch((error: unknown) => String(error)),
    stepMs,
  );
  if (Buffer.isBuffer(screenshot)) {
    await attachFile(testInfo, "failure-page-screenshot", screenshot, "image/png");
    record.screenshot = { capturedAfterMs: Date.now() - startedAt };
  } else {
    record.screenshot = { error: screenshot };
  }

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
        return {
          ...empty,
          width: canvas.width,
          height: canvas.height,
          error: String(error),
        };
      }
    }, kReadbackMaxEdge).catch((error: unknown) => String(error)),
    stepMs,
  );
  if (typeof readback === "string") {
    record.readback = { error: readback, afterMs: Date.now() - readbackStartedAt };
  } else {
    const { pngBase64, ...counts } = readback;
    record.readback = { ...counts, afterMs: Date.now() - readbackStartedAt };
    if (pngBase64 !== null) {
      await attachFile(
        testInfo,
        "failure-canvas-readback",
        Buffer.from(pngBase64, "base64"),
        "image/png",
      );
    }
  }

  // A readback that timed out may have left the page unresponsive; the state read is bounded the
  // same way and simply records the timeout.
  const state = await bounded(
    page.evaluate(() => {
      const w = window as unknown as Record<string, unknown>;
      return {
        presentationQueue: w.__donnerPresentationQueueStats ?? null,
        hostFrames: w.__donnerHostFrameTiming ?? null,
        worker: w.__donnerWorkerStats ?? null,
        viewport: w.__donnerViewportStats ?? null,
        overlay: w.__donnerOverlayStats ?? null,
        interaction: w.__donnerInteractionStats ?? null,
        firstFramePresented: w.__donnerFirstFramePresented ?? null,
        visibility: document.visibilityState,
      };
    }).catch((error: unknown) => String(error)),
    stepMs,
  );
  record.presentation = state;
  record.totalMs = Date.now() - startedAt;
  await attachFile(
    testInfo,
    "failure-canvas-evidence",
    JSON.stringify(record, null, 2),
    "application/json",
  );
}

/**
 * Capture failure evidence after every failed case whose title is listed.
 *
 * @param test The spec's test object.
 * @param titles Exact titles of the cases to cover.
 */
export function installFailureCanvasEvidence(
  test: typeof baseTest,
  titles: readonly string[],
): void {
  test.afterEach(async ({ page }, testInfo) => {
    if (testInfo.status === testInfo.expectedStatus) return;
    if (!titles.includes(testInfo.title)) return;
    try {
      await captureFailureCanvasEvidence(page, testInfo);
    } catch (error) {
      console.error(`failure canvas evidence capture failed: ${String(error)}`);
    }
  });
}
