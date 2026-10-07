import type { Page, test as baseTest, TestInfo } from "@playwright/test";
import { writeFile } from "node:fs/promises";

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
 */

/** Bound for each evidence step. */
export const kFailureEvidenceStepMs = 4_000;
/** Upper bound for the whole capture: state, screenshot and readback, one step each. */
export const kFailureEvidenceBudgetMs = 3 * kFailureEvidenceStepMs;

/** Longest edge of the readback PNG, in pixels; larger canvases are scaled down. */
const kReadbackMaxEdge = 1600;

const armedPages = new Map<string, Page>();

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
export type EvidencePage = Pick<Page, "evaluate" | "screenshot">;

/**
 * Capture the presentation state, a page screenshot and one canvas readback, and attach them.
 *
 * @param page The failed case's page.
 * @param testInfo The failed case's test info.
 * @param stepMs Bound for each step.
 */
export async function captureFailureCanvasEvidence(
  page: EvidencePage,
  testInfo: TestInfo,
  stepMs = kFailureEvidenceStepMs,
): Promise<void> {
  const record: Record<string, unknown> = { stepMs };
  const startedAt = Date.now();

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
    }).catch((error: unknown) => ({ error: String(error) })),
    stepMs,
  );
  record.presentation = state;

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

  if (state === "timeout") {
    record.readback = { skipped: "the page did not answer the state read" };
  } else {
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
  armedPages.set(testInfo.testId, page);
}

/**
 * Install the hook that captures failure evidence for armed cases. It requests no fixtures.
 *
 * @param test The spec's test object.
 */
export function installFailureCanvasEvidence(test: typeof baseTest): void {
  test.afterEach(async () => {
    const testInfo = test.info();
    const page = armedPages.get(testInfo.testId);
    armedPages.delete(testInfo.testId);
    if (page === undefined || testInfo.status === testInfo.expectedStatus) return;
    try {
      await captureFailureCanvasEvidence(page, testInfo);
    } catch (error) {
      console.error(`failure canvas evidence capture failed: ${String(error)}`);
    }
  });
}
