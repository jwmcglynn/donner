import { expect, type Page, test } from "@playwright/test";
import {
  captureSplashPresentationFrame,
  type CssRegion,
  type EditorBackgroundCoverageStats,
  isSplashCaptureUsable,
  type PixelBounds,
  readCssPngPixelDifferenceStats,
  readEditorBackgroundCoverage,
  readEditorPixelBounds,
  readEditorPixelBoundsFromPng,
  readEditorResizePixelBounds,
  readElementColorStats,
  readPngPixelDifferenceStats,
  readSplashCompositeFrameStats,
  type SplashPresentationFrame,
  type SplashToneCensus,
} from "./canvas-color-stats";
import { waitForAppliedPointer } from "./gesture-streams";

declare global {
  interface Window {
    __donnerBackend?: string;
    __donnerWholeAppWorker?: boolean;
    __donnerCanStartWasm?: boolean;
    __donnerFirstFramePresented?: boolean;
    __donnerActiveSampleStats?: {
      sampleId: string;
      activatedAtMs: number;
    };
    __donnerMainLoopRenderedFrames?: number;
    __donnerWorkerStats?: {
      cachedTileCount?: number;
      completedResults: number;
      offscreenCreateCount?: number;
      offscreenCreateTotal?: number;
      offscreenRecycleCount?: number;
      offscreenRecycleTotal?: number;
      // Device health, published onto the same object as the frame counter.
      // A frame that never lands leaves the counter flat and says nothing
      // else; these say whether the renderer stopped and which bounded GPU
      // wait stopped it. Optional because a build older than the publisher,
      // or a page where the worker never published at all, has neither.
      deviceLost?: boolean;
      gpuWaitTimeoutSite?: string;
      gpuWaitTimeoutMs?: number;
      publishReason?: string;
      publishedAtMs?: number;
      presentedAtMs?: number;
    };
    __donnerInteractionStats?: {
      dragging: boolean;
      pendingClick: boolean;
      selectedCount: number;
      workerBusy: boolean;
      pointerX: number;
      pointerY: number;
    };
    __donnerSampleThumbnailStats?: {
      publishedAtMs?: number;
      publicationGeneration?: number;
      requested: number;
      started: number;
      completed: number;
      rendered: number;
      ready: number;
      pending: boolean;
      active: boolean;
      resultReady: boolean;
    };
    __donnerFrameLoopStats?: FrameLoopStats;
    __donnerOverlayStats?: {
      compositorTileOverlay: boolean;
      geometryDebugOverlay: boolean;
      selectionChromeSnapshotPresent: boolean;
      currentDocVersion: number;
      displayedDocVersion: number;
      overlayVersionGateSuppressions: number;
    };
    __donnerViewportStats?: ViewportStats;
    __donnerEditorFrameRequested?: boolean;
  }
}

// Where the render pane and the presented document sit on screen, published by
// `EditorShell::PublishViewportStats`. Since the single-canvas architecture the
// canvas box describes the whole editor, so this is the only handle a pixel
// probe has on which of its pixels are document and which are chrome. All
// values are page CSS pixels, and a document coordinate maps onto the page with
// `documentX + documentWidth * (x / viewBoxWidth)`.
interface ViewportStats {
  paneX: number;
  paneY: number;
  paneWidth: number;
  paneHeight: number;
  documentX: number;
  documentY: number;
  documentWidth: number;
  documentHeight: number;
  zoom: number;
}

// Per-frame accounting for the demand-driven frame loop in `donner/editor/main.cc`, published to
// the page through the whole-app worker bridge. `renderedFrames` counts the frames the loop chose
// to run, split by what woke it: `inputTriggeredFrames` for a DOM event, `workerTriggeredFrames`
// for an editor-side frame request, `timerTriggeredFrames` for a due idle timer. `callbacks`
// counts every animation-frame tick the loop saw, including the ones it declined, and reaches the
// page with the next frame the loop runs.
interface FrameLoopStats {
  callbacks: number;
  renderedFrames: number;
  inputTriggeredFrames: number;
  workerTriggeredFrames: number;
  timerTriggeredFrames: number;
  workerOnlyFrames: number;
  uiFrameMsSamples: number[];
  /** Page-clock arrival time of the latest frame's sample. */
  lastFrameAtMs?: number;
}

// Shared CI runners execute this suite 2-4x slower than local development
// hardware. Scale tight acceptance polls so the timing assertions verify the
// same invariants without flaking on runner speed; local bounds are unchanged.
const kCiTimeScale = process.env.CI ? 4 : 1;
const scaledMs = (ms: number) => ms * kCiTimeScale;

const kBaseUrl = process.env.DONNER_WASM_BASE_URL || "http://127.0.0.1:8000";

// Where the Basic Shapes blue rounded rectangle sits inside `#canvas` at the
// pinned 1600x900 viewport. Since the single-canvas architecture the document is drawn inside the
// single canvas's WebGPU frame, so pointer targets are fixed canvas offsets
// rather than fractions of a separate document element.
const kBlueRectOffset = { x: 408, y: 353 };

// The render pane's CSS rectangle inside `#canvas` at the pinned 1600x900
// viewport (source pane 560px on the left, panels 420px on the right, menu bar
// and status strip top and bottom). Geode draws the document inside this
// rectangle of the single canvas frame, so it is the region every document
// pixel probe samples.
const kRenderPaneInset = { left: 580, top: 80, right: 440, bottom: 140 };

function renderPaneRegion(
  editorBounds: { x: number; y: number; width: number; height: number },
): CssRegion {
  return {
    x: editorBounds.x + kRenderPaneInset.left,
    y: editorBounds.y + kRenderPaneInset.top,
    width: editorBounds.width - kRenderPaneInset.left - kRenderPaneInset.right,
    height: editorBounds.height - kRenderPaneInset.top - kRenderPaneInset.bottom,
  };
}

// Region for measuring the Splash letter's own rendered extent. The letter
// extends left of kRenderPaneInset.left and grows past it under zoom, so
// tests that treat the letter's width as the zoom observable measure a wider
// span: full pane height, everything left of the panel column. The letter's
// yellow only occurs in document pixels, so the extra width cannot pollute
// the bounds, while the panel column stays excluded because its layer
// thumbnails repeat the letter art in miniature.
function splashLetterMeasureRegion(
  editorBounds: { x: number; y: number; width: number; height: number },
): CssRegion {
  return {
    x: editorBounds.x + 60,
    y: editorBounds.y + kRenderPaneInset.top,
    width: editorBounds.width - 60 - kRenderPaneInset.right,
    height: editorBounds.height - kRenderPaneInset.top - kRenderPaneInset.bottom,
  };
}

async function readViewportStats(page: Page): Promise<ViewportStats> {
  const stats = await page.evaluate(() => window.__donnerViewportStats);
  expect(stats, "the editor never published __donnerViewportStats").toBeDefined();
  return stats as ViewportStats;
}

// The presented document's own rectangle on screen, clipped to the render pane.
//
// This is the window every Splash coverage probe samples, and it is derived
// from what the editor published rather than from a hand-measured inset. A
// fixed inset rectangle answers a different question every time the pane's
// layout moves: `kRenderPaneInset` still names a rectangle that overlaps the
// document but also covers pane chrome on two sides and misses document on a
// third, which is how a "the document covers the pane" assertion came to be
// satisfied at 43% coverage. Scoped to the document the same measurement is
// ~75%, and a capture that contains no document at all lands at zero rather
// than at whatever fraction of chrome the inset happened to frame.
function presentedDocumentRegion(stats: ViewportStats): CssRegion {
  const left = Math.max(stats.documentX, stats.paneX);
  const top = Math.max(stats.documentY, stats.paneY);
  const right = Math.min(stats.documentX + stats.documentWidth, stats.paneX + stats.paneWidth);
  const bottom = Math.min(stats.documentY + stats.documentHeight, stats.paneY + stats.paneHeight);
  return { x: left, y: top, width: right - left, height: bottom - top };
}

// `donner_splash.svg` is authored in a 892x512 viewBox.
const kSplashViewBox = { width: 892, height: 512 };

// The `Donner_D` path's extent in that viewBox, taken from the sample's own
// path data, and a press point inside the letter's solid left stem: right of
// the outer edge (x=271.41), left of the counter (x=290.68), and between the
// two horizontal bars, so the press hits the letter rather than the artwork
// that shows through its counter.
const kSplashLetterD = {
  minX: 271.41,
  minY: 346.89,
  maxX: 332.6,
  maxY: 433.5,
  stemPress: { x: 281, y: 395 },
};

// The Splash drag: ten pointer steps up and to the left, in page CSS pixels.
// Small steps keep the worker continuously superseded, which is the
// presentation window where an incomplete render used to replace the last
// complete one.
const kSplashDragSteps = 10;
const kSplashDragStep = { x: -5, y: -2.5 };

// How far the selection outline's own pixels may sit outside the letter they
// surround: the outline is stroked around the selection bounds and carries a
// corner handle, both of which extend a few CSS pixels past the art. Measured
// at 6-7px on both engines; the bound only has to separate "the outline is
// around this letter" from "the outline is around something else entirely",
// and the per-frame alignment assertion below is what holds it to a pixel.
const kSelectionHandleMargin = 12;

// How far the letter's measured travel may fall short of the pointer's. The
// letter's edge is antialiased against the artboard, so the tone window can
// gain or lose an edge pixel between frames; anything larger is the letter
// failing to follow the drag.
const kLetterTravelTolerance = 4;

function splashDocumentToPage(
  stats: ViewportStats,
  point: { x: number; y: number },
): { x: number; y: number } {
  return {
    x: stats.documentX + stats.documentWidth * (point.x / kSplashViewBox.width),
    y: stats.documentY + stats.documentHeight * (point.y / kSplashViewBox.height),
  };
}

// The window the dragged letter is measured in: the corridor its solid left
// stem sweeps, from where the stem sits at rest to where the full drag travel
// leaves it.
//
// The whole letter cannot be the window. The Splash draws a yellow lightning
// bolt immediately above the D, the "O" immediately right of it, and the blue
// "Donner_line" swoosh diagonally below it (nearest at document y=445, under
// the stem's right edge); the first two match `splash-yellow` and the third
// matches `selection-teal`, so a window that admits any of them reports static
// artwork bounds and a letter that never moved still measures the same every
// frame. The stem corridor - inset from the counter, stopping short of the
// swoosh - contains nothing but the letter and its own outline, which makes
// `letter.minX` and `letter.maxY` honest observables of where the letter is.
function splashLetterTrackingWindow(stats: ViewportStats): CssRegion {
  const scale = stats.documentWidth / kSplashViewBox.width;
  const restStemBottomLeft = splashDocumentToPage(stats, {
    x: kSplashLetterD.minX,
    y: kSplashLetterD.maxY,
  });
  const stemInset = 20 * scale;
  const stemHeight = 50 * scale;
  const margin = 6 * scale;
  const travelX = kSplashDragSteps * kSplashDragStep.x;
  const travelY = kSplashDragSteps * kSplashDragStep.y;
  const left = restStemBottomLeft.x + travelX - margin;
  const top = restStemBottomLeft.y + travelY - stemHeight;
  return {
    x: left,
    y: top,
    width: restStemBottomLeft.x + stemInset - left,
    height: restStemBottomLeft.y + margin - top,
  };
}

// Wait for the demand-driven frame loop to park.
//
// Reading pixels while the loop is still servicing frames is how a capture
// comes to straddle two of them: a full-document screenshot takes longer than a
// frame, so any capture started mid-burst mixes geometry from both sides of it.
// The loop parks once it has presented what it was woken for, so an unchanged
// frame counter across a browser composite is the signal that a capture can
// describe one frame. Reports whether it parked rather than throwing, so the
// caller's own diagnostics carry the failure.
async function waitForParkedFrameLoop(page: Page, timeoutMs: number): Promise<boolean> {
  const deadline = Date.now() + timeoutMs;
  const renderedFrames = () => page.evaluate(() => window.__donnerMainLoopRenderedFrames || 0);
  for (;;) {
    const before = await renderedFrames();
    await waitForBrowserComposite(page);
    if (before === await renderedFrames()) {
      return true;
    }
    if (Date.now() >= deadline) {
      return false;
    }
  }
}

// Whether two captures describe the same presented picture.
//
// Both captures are scored in page CSS pixels off their own screenshot, so
// equality here means the letter and its outline are in the same place and the
// document covers the same share of its rectangle. Nothing that is moving
// produces two of these in a row.
function splashFramesAgree(a: SplashPresentationFrame, b: SplashPresentationFrame): boolean {
  const sameBounds = (left: PixelBounds | null, right: PixelBounds | null): boolean =>
    left === null || right === null
      ? left === right
      : left.minX === right.minX && left.minY === right.minY && left.maxX === right.maxX
        && left.maxY === right.maxY;
  const coverageTolerance = a.census.samples * 0.002;
  return sameBounds(a.letter, b.letter) && sameBounds(a.outline, b.outline)
    && Math.abs(a.census.darkBackgroundPixels - b.census.darkBackgroundPixels) <= coverageTolerance
    && Math.abs(a.census.checkerboardPixels - b.census.checkerboardPixels) <= coverageTolerance;
}

// Take one capture of the presented document and refuse to score an unusable
// one.
//
// Two ways a capture says nothing about the frame under test. It can straddle
// two presented frames and mix their geometry, which would fake the very defect
// this suite looks for. And it can contain none of the editor's own tones at
// all - zero artboard, zero checkerboard, zero pane backdrop - which is not a
// partially rendered document but a capture of something that is not the
// editor, and is the shape of the sample a shared runner produced when this
// test last failed. Both are retaken, bounded; a capture window that never
// resolves fails with the histogram and the editor's published state attached,
// so the next reader sees what was actually on screen.
//
// "Did not straddle a frame" is asked of the picture, not of the frame counter.
// The counter is the cheap proof and is tried first: the demand-driven loop
// parks once it has presented what it was woken for, so an unchanged counter
// across the capture means one frame. But the counter also advances for wakes
// this capture is not about - opening a sample kicks off a layer-thumbnail
// burst that renders for seconds, one ~90ms frame per layer - and while that
// runs the loop never parks, so a strictly-counter-based validator rejects
// every capture in a row and fails a test whose document is fully and
// correctly presented. Two consecutive captures that agree on the letter, the
// outline and the coverage cannot have mixed two different pictures, so that
// is accepted as the same proof. A capture is never rejected for showing a
// complete document; it is rejected for being uniform, empty, or in motion.
async function captureSplashDragFrame(
  page: Page,
  region: CssRegion,
  letterWindow: CssRegion,
  context: string,
): Promise<SplashPresentationFrame & { renderedFrames: number }> {
  let last: SplashPresentationFrame | null = null;
  let previousUsable: SplashPresentationFrame | null = null;
  for (let attempt = 0; attempt < 4; ++attempt) {
    // Only the first attempt waits for a park. If the loop is going to park it
    // parks within that window; if it is not - the thumbnail burst again - then
    // spending the same wait on every retry only burns the test's budget
    // before the retries that settle this by content can run.
    if (attempt === 0) {
      await waitForParkedFrameLoop(page, scaledMs(2_000));
    } else {
      await waitForBrowserComposite(page);
    }
    const before = await readDocumentPresentationState(page);
    const frame = await captureSplashPresentationFrame(page, region, letterWindow);
    const after = await readDocumentPresentationState(page);
    last = frame;
    if (!isSplashCaptureUsable(frame.census)) {
      previousUsable = null;
      continue;
    }
    if (
      before.renderedFrames === after.renderedFrames
      || (previousUsable !== null && splashFramesAgree(previousUsable, frame))
    ) {
      return { ...frame, renderedFrames: after.renderedFrames };
    }
    previousUsable = frame;
  }
  if (last !== null) {
    await test.info().attach(`unusable-capture-${context}`, {
      body: last.png,
      contentType: "image/png",
    });
  }
  const published = await page.evaluate(() => ({
    frameLoop: window.__donnerFrameLoopStats,
    interaction: window.__donnerInteractionStats,
    renderedFrames: window.__donnerMainLoopRenderedFrames || 0,
    viewport: window.__donnerViewportStats,
    worker: window.__donnerWorkerStats,
  }));
  throw new Error(
    `${context}: no usable capture of the presented document after 4 attempts. `
      + `region=${JSON.stringify(region)} census=${JSON.stringify(last?.census)} `
      + `published=${JSON.stringify(published)}`,
  );
}

// The document's presentation progress. Since the single-canvas architecture there is no separate
// document element and no acceptance token: a completed worker result is the
// document raster, and the app thread draws it into the single canvas.
interface DocumentPresentationState {
  completedResults: number;
  renderedFrames: number;
}

async function readDocumentPresentationState(page: Page): Promise<DocumentPresentationState> {
  return page.evaluate(() => ({
    completedResults: window.__donnerWorkerStats?.completedResults || 0,
    renderedFrames: window.__donnerMainLoopRenderedFrames || 0,
  }));
}

// The worker's device health, published alongside the frame counter every
// gate in this file waits on.
interface WorkerHealth {
  completedResults: number;
  deviceLost: boolean;
  gpuWaitTimeoutSite: string;
  gpuWaitTimeoutMs: number;
  publishReason: string;
  sampleThumbnail: Window["__donnerSampleThumbnailStats"] | null;
  sampleThumbnailPublishedAtMs: number;
  sampleThumbnailPublicationGeneration: number;
  frameLoop: FrameLoopStats | null;
  interaction: Window["__donnerInteractionStats"] | null;
  // The presentation half of the handoff, read in the same round trip as the
  // worker half so a gate cannot pair a counter from one frame with a
  // presentation stamp from another. A result is published without
  // `presentedAtMs` and the frame that consumes it stamps the field once, so
  // "the worker finished" and "the pixels are on the canvas" are two different
  // questions and a probe needs the second.
  renderedFrames: number;
  presentedAtMs: number | undefined;
  activeSampleId: string | undefined;
}

// `unpublished` and -1 distinguish "the worker never published stats at all"
// from "the worker published and reports a healthy device". Those are very
// different failures and the counter alone reports both as a flat zero.
async function readWorkerHealth(page: Page): Promise<WorkerHealth> {
  return page.evaluate(() => ({
    completedResults: window.__donnerWorkerStats?.completedResults ?? 0,
    deviceLost: window.__donnerWorkerStats?.deviceLost ?? false,
    gpuWaitTimeoutSite: window.__donnerWorkerStats?.gpuWaitTimeoutSite ?? "unpublished",
    gpuWaitTimeoutMs: window.__donnerWorkerStats?.gpuWaitTimeoutMs ?? -1,
    publishReason: window.__donnerWorkerStats?.publishReason ?? "unpublished",
    sampleThumbnail: window.__donnerSampleThumbnailStats ?? null,
    sampleThumbnailPublishedAtMs: window.__donnerSampleThumbnailStats?.publishedAtMs ?? -1,
    sampleThumbnailPublicationGeneration: window.__donnerSampleThumbnailStats?.publicationGeneration
      ?? -1,
    frameLoop: window.__donnerFrameLoopStats ?? null,
    interaction: window.__donnerInteractionStats ?? null,
    renderedFrames: window.__donnerMainLoopRenderedFrames || 0,
    presentedAtMs: window.__donnerWorkerStats?.presentedAtMs,
    activeSampleId: window.__donnerActiveSampleStats?.sampleId,
  }));
}

/**
 * Wait for the worker's completed-frame count to satisfy `reached`.
 *
 * Every render gate in this file waits on that counter, and a counter that
 * never moves reports only "still <n>" - which is what the hardest failure
 * looks like too: a bounded GPU wait burning its deadline stops the worker
 * publishing entirely, and on a loaded runner that is indistinguishable from
 * slow. Polling the whole health snapshot instead of the bare number puts the
 * device-lost flag, the wait that timed out and its elapsed time into the
 * value the assertion prints, so a failing run names the cause without a
 * rerun. Waiting behavior is unchanged; only what the failure says changes.
 *
 * The matcher is deliberately `toEqual(objectContaining(...))` and not
 * `toMatchObject`: the latter prints only the keys it was asked to match, so
 * the health fields would never reach the log and this helper would be pure
 * overhead. Verified by running both against a failing poll.
 */
async function expectWorkerResultsToReach(
  page: Page,
  reached: (completedResults: number, health: WorkerHealth) => boolean,
  options: { message: string; timeout: number },
): Promise<void> {
  await expect
    .poll(
      async () => {
        const health = await readWorkerHealth(page);
        return { ...health, reached: reached(health.completedResults, health) };
      },
      {
        message: options.message,
        timeout: options.timeout,
        intervals: [16, 25, 50, 100],
      },
    )
    .toEqual(expect.objectContaining({ reached: true }));
}

// What a drag press is supposed to have accomplished, read as one snapshot: the
// shape is selected and its single prewarm render has landed. Reading both
// together is what makes the failure diagnosable - a press the busy worker
// dropped reports `selectedCount: 0` rather than an unexplained missing render.
async function readPressSelectionState(
  page: Page,
): Promise<{ completedResults: number; selectedCount: number }> {
  return page.evaluate(() => ({
    completedResults: window.__donnerWorkerStats?.completedResults || 0,
    selectedCount: window.__donnerInteractionStats?.selectedCount ?? -1,
  }));
}

async function openEditor(page: Page): Promise<string[]> {
  const failures: string[] = [];
  page.on("console", (message) => {
    if (
      /Failed to wake Wasm renderer pthread|Wasm renderer pthread wake rejected|Aborted|RuntimeError|UTILS_RELEASE_ASSERT/i
        .test(
          message.text(),
        )
    ) {
      failures.push(`[console:${message.type()}] ${message.text()}`);
    }
  });
  page.on("pageerror", (error) => failures.push(`[pageerror] ${error.message}`));

  await page.goto(kBaseUrl, { waitUntil: "domcontentloaded" });
  await expect.poll(() => page.evaluate(() => window.__donnerCanStartWasm)).toBe(true);
  // Playwright's bundled WebKit ships no WebGPU, so the Geode-only package
  // cannot boot there; real-Safari validation covers that engine. Skip
  // instead of stalling on the loading screen.
  const hasWebGpu = await page.evaluate(() => "gpu" in navigator);
  test.skip(!hasWebGpu, "Browser does not expose navigator.gpu");
  await expect(page.locator("canvas#canvas")).toBeVisible();
  await expect
    .poll(() => page.evaluate(() => window.__donnerFirstFramePresented === true), {
      message: "expected Geode to present the editor's first frame",
      timeout: 20_000,
      intervals: [16, 25, 50, 100],
    })
    .toBe(true);
  await expect(page.locator("#status")).toBeHidden({ timeout: 20_000 });
  return failures;
}

// Read the editor canvas the way the page itself sees it, then again after the
// editor has presented one more frame.
//
// A screenshot is two hops away from what the editor drew: the editor presents
// into the canvas, and the browser then has to hand that presented image to
// whoever reads the page. Gecko does not guarantee the second hop survives an
// arbitrary gap between presents for a canvas a worker owns, and this suite
// already carries a same-task retry for its empty first read of a
// just-presented WebGPU canvas (see the composited probe). The editor's frame
// loop is demand driven, so once it parks after presenting there is nothing to
// re-arm that image, and every later read of the page answers empty.
//
// Distinguishing that from "the editor presented nothing" cannot be done from
// the screenshot alone, because both answer with the page background. So sample
// the canvas element directly, wake the editor for one more frame, and sample
// again. `beforeWake > 0` means the canvas held content and only the screenshot
// path missed it; `beforeWake === 0 && afterWake > 0` means the presented image
// was lost while the loop was parked and a fresh present restores it;
// both zero means the editor really is presenting nothing.
//
// This runs only after a pixel wait has already failed, so it can neither
// rescue a failing assertion nor perturb a passing one.
interface PresentedCanvasDiagnosis {
  beforeWake: number;
  afterWake: number;
  framesBeforeWake: number;
  framesAfterWake: number;
  canvasWidth: number;
  canvasHeight: number;
}

async function diagnosePresentedCanvas(
  page: Page,
): Promise<PresentedCanvasDiagnosis | { unavailable: string }> {
  return page
    .evaluate(async () => {
      const surface = document.getElementById("canvas") as HTMLCanvasElement | null;
      if (surface === null) {
        throw new Error("the editor canvas is missing");
      }
      const scratch = document.createElement("canvas");
      scratch.width = 240;
      scratch.height = 150;
      const context = scratch.getContext("2d", { willReadFrequently: true });
      if (context === null) {
        throw new Error("no 2d context for the canvas probe");
      }
      // Count pixels that carry both coverage and hue, so the editor's own
      // chrome registers while the flat page background does not.
      const readColoredPixels = (): number => {
        context.clearRect(0, 0, scratch.width, scratch.height);
        context.drawImage(
          surface,
          0,
          0,
          surface.width,
          surface.height,
          0,
          0,
          scratch.width,
          scratch.height,
        );
        const pixels = context.getImageData(0, 0, scratch.width, scratch.height).data;
        let colored = 0;
        for (let index = 0; index < pixels.length; index += 4) {
          const red = pixels[index];
          const green = pixels[index + 1];
          const blue = pixels[index + 2];
          const alpha = pixels[index + 3];
          const spread = Math.max(red, green, blue) - Math.min(red, green, blue);
          if (alpha >= 40 && spread >= 12) {
            colored += 1;
          }
        }
        return colored;
      };

      const framesBeforeWake = window.__donnerMainLoopRenderedFrames || 0;
      const beforeWake = readColoredPixels();

      // Ask the editor for one frame through the same flag the page uses, then
      // wait for the loop to report it before reading again.
      window.__donnerEditorFrameRequested = true;
      const deadline = performance.now() + 2000;
      while (
        (window.__donnerMainLoopRenderedFrames || 0) === framesBeforeWake
        && performance.now() < deadline
      ) {
        await new Promise((resolve) => requestAnimationFrame(() => resolve(null)));
      }
      await new Promise((resolve) =>
        requestAnimationFrame(() => requestAnimationFrame(() => resolve(null)))
      );

      return {
        beforeWake,
        afterWake: readColoredPixels(),
        framesBeforeWake,
        framesAfterWake: window.__donnerMainLoopRenderedFrames || 0,
        canvasWidth: surface.width,
        canvasHeight: surface.height,
      };
    })
    .catch((error: unknown) => ({ unavailable: String(error) }));
}

async function openBasicShapes(page: Page): Promise<{
  canvasBounds: { x: number; y: number; width: number; height: number };
  documentClip: { x: number; y: number; width: number; height: number };
}> {
  const editorCanvas = page.locator("canvas#canvas");
  const canvasBounds = await editorCanvas.boundingBox();
  expect(canvasBounds).not.toBeNull();
  if (canvasBounds === null) {
    throw new Error("editor canvas is missing");
  }

  // These callers measure overlays and presentation pixels, not the thumbnail-to-foreground
  // scheduler transition. The dedicated Firefox handoff regression below forces that collision
  // deterministically. Let first-use offscreen WebGPU work settle here so a visual oracle cannot
  // spend its entire deadline on an unrelated thumbnail cancellation.
  await expect
    .poll(
      () =>
        page.evaluate(() => {
          const stats = window.__donnerSampleThumbnailStats;
          if (!stats) return false;
          const completed = stats.completed ?? 0;
          return completed > 0 && (stats.ready ?? 0) > 0 && !stats.active && !stats.pending;
        }),
      {
        message: "the first offscreen thumbnail must settle before a visual sample replaces it",
        timeout: scaledMs(20_000),
        intervals: [16, 25, 50, 100],
      },
    )
    .toBe(true);
  const beforeSampleResults = await page.evaluate(
    () => window.__donnerWorkerStats?.completedResults || 0,
  );
  await page.mouse.click(canvasBounds.x + canvasBounds.width * 0.5, canvasBounds.y + 282);
  await expect(editorCanvas).toHaveAttribute("data-active-sample-id", "basic-shapes");
  await expectWorkerResultsToReach(
    page,
    (completedResults) => completedResults > beforeSampleResults,
    {
      message: "Basic Shapes must have its own document render before capture",
      timeout: scaledMs(5_000),
    },
  );
  await waitForBrowserComposite(page);

  const documentClip = {
    x: canvasBounds.x + 280,
    y: canvasBounds.y + 250,
    width: 660,
    height: 430,
  };
  // A completed worker result is not yet a presented document. The counter
  // above advances when the app thread polls the raster off the worker, at
  // least one UI frame before that frame reaches the browser composite - and
  // a result the presenter drops (its raster viewport was superseded while it
  // was in flight) advances the counter without presenting anything, leaving
  // the sample picker on screen. On a loaded runner that gap spans several
  // captures. Wait for the pixels every caller is about to measure: the Basic
  // Shapes blue rounded rectangle inside the render pane.
  // Last pixel count the probe measured, reported below on both paths.
  let lastBluePixels = -1;
  try {
    await expect
      .poll(
        async () => {
          const shot = await page.screenshot({ clip: documentClip });
          const bounds = readEditorPixelBoundsFromPng(shot, "basic-blue", documentClip, {
            minX: 0,
            minY: 0,
            maxX: documentClip.width,
            maxY: documentClip.height,
          });
          lastBluePixels = bounds === null ? 0 : bounds.pixels;
          return lastBluePixels;
        },
        {
          message: "expected the presented render pane to show the Basic Shapes blue rectangle",
          timeout: scaledMs(5_000),
          intervals: [16, 25, 50, 100],
        },
      )
      .toBeGreaterThan(0);
  } finally {
    // "No blue" covers three unrelated faults: the worker never produced a
    // result, a result arrived that no app frame carried, or a frame was
    // presented whose pixels this probe window missed. Publish the app's own
    // counters so the next failure names which one happened instead of only
    // reporting that blue was absent. The read is best-effort: when the whole
    // test times out the page is already being torn down, and a rejection
    // here must not replace the real failure.
    const presentationState = await page
      .evaluate(() => ({
        worker: window.__donnerWorkerStats,
        frames: window.__donnerMainLoopRenderedFrames || 0,
        workerBusy: window.__donnerInteractionStats?.workerBusy,
        activeSample: window.__donnerActiveSampleStats,
      }))
      .catch((error: unknown) => ({ unavailable: String(error) }));
    // Only worth the extra page work when the pixels never arrived; a passing
    // wait already proved the whole path.
    const canvasDiagnosis = lastBluePixels > 0 ? null : await diagnosePresentedCanvas(page);
    console.log(
      `open-basic-shapes bluePixels=${lastBluePixels} state=${JSON.stringify(presentationState)}`
        + ` canvas=${JSON.stringify(canvasDiagnosis)}`,
    );
  }

  return { canvasBounds, documentClip };
}

async function toggleViewMenuItem(page: Page, canvasX: number, itemY: number): Promise<void> {
  await page.mouse.click(canvasX + 260, 11);
  await page.mouse.click(canvasX + 330, itemY);
  await page.evaluate(() => new Promise((resolve) => requestAnimationFrame(() => resolve(null))));
}

// Drive a View-menu overlay toggle to a WANTED state and verify it took effect
// against the app's published `__donnerOverlayStats`. The menu rows can only be
// reached by fixed pixel offsets, and a click that lands between rows silently
// does nothing while leaving the menu open - which is exactly how a runner with
// slightly different row metrics kept an overlay enabled that the test believed
// it had disabled. Escape closes any left-open menu before each retry.
async function setViewOverlayState(
  page: Page,
  canvasX: number,
  itemY: number,
  key: "compositorTileOverlay" | "geometryDebugOverlay",
  enabled: boolean,
): Promise<void> {
  for (let attempt = 0; attempt < 4; ++attempt) {
    const current = await page.evaluate(
      (k) => window.__donnerOverlayStats?.[k],
      key,
    );
    if (current === enabled) {
      return;
    }
    await toggleViewMenuItem(page, canvasX, itemY);
    const flipped = await page
      .waitForFunction(
        ({ k, want }) => window.__donnerOverlayStats?.[k] === want,
        { k: key, want: enabled },
        { timeout: scaledMs(2_000) },
      )
      .then(() => true)
      .catch(() => false);
    if (flipped) {
      return;
    }
    await page.keyboard.press("Escape");
    await page.evaluate(() => new Promise((resolve) => requestAnimationFrame(() => resolve(null))));
  }
  throw new Error(`the ${key} menu toggle never reached ${enabled} after 4 attempts`);
}

async function waitForBrowserComposite(page: Page): Promise<void> {
  await page.evaluate(() =>
    new Promise((resolve) =>
      requestAnimationFrame(() => requestAnimationFrame(() => resolve(null)))
    )
  );
}

// Block until the editor can accept a press.
//
// A `mouse.down` that lands while a render is still in flight is dropped by the
// busy worker - a real app fragility tracked separately - and the drag that
// follows runs as a marquee that never produces a document render. That
// precondition is stated explicitly here instead of being approximated by a
// fixed sleep: readiness means the async renderer is idle, no click is still
// pending, and the worker result count has not moved for a continuous settle
// window (so a queued follow-up render cannot land between the check and the
// press).
async function waitForPressReadiness(page: Page, message: string): Promise<void> {
  const settleMs = scaledMs(250);
  const deadline = Date.now() + scaledMs(6_000);
  let lastCompleted = -1;
  let stableSince = Date.now();
  for (;;) {
    const snapshot = await page.evaluate(() => ({
      busy: window.__donnerInteractionStats?.workerBusy ?? true,
      completed: window.__donnerWorkerStats?.completedResults || 0,
      pendingClick: window.__donnerInteractionStats?.pendingClick ?? true,
    }));
    const now = Date.now();
    if (snapshot.busy || snapshot.pendingClick || snapshot.completed !== lastCompleted) {
      lastCompleted = snapshot.completed;
      stableSince = now;
    } else if (now - stableSince >= settleMs) {
      return;
    }
    if (now >= deadline) {
      throw new Error(
        `${message}: the editor never became ready for a press `
          + `(${JSON.stringify(snapshot)})`,
      );
    }
    await page.waitForTimeout(16);
  }
}

test.use({ viewport: { width: 1600, height: 900 } });

test("Geode Wasm View overlays render tile metadata and sparse Slug triangle edges", async ({ browserName, page }) => {
  test.skip(browserName !== "firefox", "Firefox Geode regression");
  const failures = await openEditor(page);
  const { canvasBounds, documentClip } = await openBasicShapes(page);
  const baseline = await page.screenshot({ clip: documentClip });
  await test.info().attach("overlay-baseline", { body: baseline, contentType: "image/png" });
  // Since the single-canvas architecture the document has no element of its own to measure, so the
  // document-space mapping is recovered from the document's own pixels: the
  // Basic Shapes blue rounded rectangle spans (32,32)-(212,152) in document
  // units. Both the probe points below and the "restored baseline" comparison
  // window are derived from it, so neither can drift onto render-pane chrome.
  const blueRect = readEditorPixelBoundsFromPng(baseline, "basic-blue", documentClip, {
    minX: 0,
    minY: 0,
    maxX: documentClip.width,
    maxY: documentClip.height,
  });
  expect(blueRect, "the Basic Shapes blue rectangle was not visible in the render pane").not
    .toBeNull();
  if (blueRect === null) {
    throw new Error("the Basic Shapes blue rectangle was not visible in the render pane");
  }
  const documentScale =
    ((blueRect.maxX - blueRect.minX) / 180 + (blueRect.maxY - blueRect.minY) / 120) * 0.5;
  expect(documentScale, "the recovered document scale is degenerate").toBeGreaterThan(0.05);
  const documentPointInClip = (documentX: number, documentY: number) => ({
    x: blueRect.minX + (documentX - 32) * documentScale,
    y: blueRect.minY + (documentY - 32) * documentScale,
  });
  const documentTopLeft = documentPointInClip(0, 0);
  const documentBottomRight = documentPointInClip(640, 400);
  const documentPixelsInClip = {
    x: Math.max(0, documentTopLeft.x),
    y: Math.max(0, documentTopLeft.y),
    width: Math.max(
      0,
      Math.min(documentClip.width, documentBottomRight.x) - Math.max(0, documentTopLeft.x),
    ),
    height: Math.max(
      0,
      Math.min(documentClip.height, documentBottomRight.y) - Math.max(0, documentTopLeft.y),
    ),
  };

  const beforeCompositorResults = await page.evaluate(
    () => window.__donnerWorkerStats?.completedResults || 0,
  );
  await setViewOverlayState(page, canvasBounds.x, 155, "compositorTileOverlay", true);
  await expectWorkerResultsToReach(
    page,
    (completedResults) => completedResults > beforeCompositorResults,
    {
      message: "Compositor Tile Overlay must rasterize a fresh document render",
      timeout: scaledMs(2_000),
    },
  );
  await waitForBrowserComposite(page);
  const compositorOverlay = await page.screenshot({ clip: documentClip });
  await test.info().attach("compositor-tile-overlay", {
    body: compositorOverlay,
    contentType: "image/png",
  });
  const compositorDifference = readCssPngPixelDifferenceStats(
    baseline,
    compositorOverlay,
    documentClip,
    documentPixelsInClip,
  );
  expect(
    compositorDifference.changedPixels,
    "Compositor Tile Overlay was checked but contributed no visible document pixels",
  ).toBeGreaterThan(0);

  // Disable the independent compositor overlay before checking renderer
  // geometry pixels, so tile labels cannot masquerade as Slug edges.
  await setViewOverlayState(page, canvasBounds.x, 155, "compositorTileOverlay", false);
  await waitForBrowserComposite(page);
  // Toggling an overlay drops the uploaded composited textures (the same
  // document version renders different pixels, so the cache cannot reuse
  // them), and restoration therefore takes a full render round-trip. Poll the
  // comparison to convergence instead of reading one instant: "must restore"
  // is a claim about where the presentation settles, and a slow runner can
  // screenshot the gap between the drop and the fresh upload.
  //
  // The screenshot clip includes a few pixels of animated render-pane chrome
  // around the document. Compare only the document's own extent; Firefox may
  // change those unrelated chrome pixels while menus close.
  let lastRestoreDifference = "";
  await expect
    .poll(
      async () => {
        const shot = await page.screenshot({ clip: documentClip });
        const difference = readCssPngPixelDifferenceStats(
          baseline,
          shot,
          documentClip,
          documentPixelsInClip,
        );
        lastRestoreDifference = JSON.stringify(difference);
        return difference.changedPixels;
      },
      {
        message: "Disabling Compositor Tile Overlay must restore document pixels",
        timeout: scaledMs(5_000),
        intervals: [50, 100, 250],
      },
    )
    .toBe(0);
  await test.info().attach("overlay-restore-difference", {
    body: lastRestoreDifference,
    contentType: "application/json",
  });
  const geometryBaseline = await page.screenshot({ clip: documentClip });
  await test.info().attach("overlay-disabled-baseline", {
    body: geometryBaseline,
    contentType: "image/png",
  });

  // The blue rounded rectangle spans (32,32)-(212,152). Its emitted
  // triangles share the diagonal through (122,92). Dynamic Slug dilation
  // moves the right submitted edge slightly beyond x=212, so probe a
  // neighborhood around 212.7 rather than assuming the pre-vertex bound.
  // (80,120) is normal fill well away from every triangle edge.
  const sharedTriangleEdge = documentPointInClip(122, 92);
  const dilatedOuterTriangleEdge = documentPointInClip(212.7, 92);
  const untouchedBlueInterior = documentPointInClip(80, 120);

  const beforeGeometryResults = await page.evaluate(
    () => window.__donnerWorkerStats?.completedResults || 0,
  );
  await setViewOverlayState(page, canvasBounds.x, 176, "geometryDebugOverlay", true);
  await expectWorkerResultsToReach(
    page,
    (completedResults) => completedResults > beforeGeometryResults,
    {
      message: "Geometry Debug Overlay must schedule a freshly rasterized document render",
      timeout: scaledMs(2_000),
    },
  );
  await waitForBrowserComposite(page);
  const geometryOverlay = await page.screenshot({ clip: documentClip });
  await test.info().attach("geometry-debug-overlay", {
    body: geometryOverlay,
    contentType: "image/png",
  });
  expect(
    geometryOverlay.equals(geometryBaseline),
    "Geometry Debug Overlay was checked and accepted, but contributed no visible canvas pixels",
  ).toBe(false);
  const edgeDifference = readCssPngPixelDifferenceStats(
    geometryBaseline,
    geometryOverlay,
    documentClip,
    {
      x: sharedTriangleEdge.x - 3,
      y: sharedTriangleEdge.y - 3,
      width: 7,
      height: 7,
    },
  );
  expect(
    edgeDifference.changedPixels,
    "Geometry Debug Overlay must expose the shared edge from the actual Slug triangles",
  ).toBeGreaterThan(0);
  const outerEdgeDifference = readCssPngPixelDifferenceStats(
    geometryBaseline,
    geometryOverlay,
    documentClip,
    {
      x: dilatedOuterTriangleEdge.x - 4,
      y: dilatedOuterTriangleEdge.y - 4,
      width: 9,
      height: 9,
    },
  );
  expect(
    outerEdgeDifference.changedPixels,
    "Geometry Debug Overlay must expose the dynamically-dilated outer Slug edge",
  ).toBeGreaterThan(0);
  const interiorDifference = readCssPngPixelDifferenceStats(
    geometryBaseline,
    geometryOverlay,
    documentClip,
    {
      x: untouchedBlueInterior.x - 6,
      y: untouchedBlueInterior.y - 6,
      width: 13,
      height: 13,
    },
  );
  expect(
    interiorDifference.changedPixels,
    "Geometry Debug Overlay must preserve normal document colors between triangle edges",
  ).toBe(0);
  expect(failures).toEqual([]);
});

test("Firefox keeps the dragged shape and its selection outline in every drag frame", async ({ browserName, page }) => {
  // The single-canvas replacement removed the two-surface epoch handoff that used to let a drag
  // frame show the shape at one position and its outline at another. What
  // survives is the user-visible claim: every frame the drag produces shows the
  // blue rectangle and the teal outline together, and the shape only ever moves
  // forward.
  test.skip(browserName !== "firefox", "Firefox Geode regression");
  const failures = await openEditor(page);
  const editorCanvas = page.locator("canvas#canvas");
  const editorBounds = await editorCanvas.boundingBox();
  expect(editorBounds).not.toBeNull();
  if (editorBounds === null) {
    return;
  }

  const beforeSample = await page.evaluate(() => window.__donnerWorkerStats?.completedResults || 0);
  await page.mouse.click(editorBounds.x + editorBounds.width * 0.5, editorBounds.y + 282);
  await expect(editorCanvas).toHaveAttribute("data-active-sample-id", "basic-shapes");
  await expectWorkerResultsToReach(
    page,
    (completedResults) => completedResults > beforeSample,
    {
      message: "expected Basic Shapes to render before the drag",
      timeout: scaledMs(5_000),
    },
  );

  await expect
    .poll(() => readElementColorStats(editorCanvas).then((stats) => stats.coloredPixels), {
      message: "expected the initial Basic Shapes render before starting the drag",
      timeout: scaledMs(2_000),
      intervals: [16, 25, 50, 100],
    })
    .toBeGreaterThan(500);
  const baseline = await readElementColorStats(editorCanvas);

  const dragStart = {
    x: editorBounds.x + kBlueRectOffset.x,
    y: editorBounds.y + kBlueRectOffset.y,
  };
  // The press must land on a settled editor: a mouse-down while the sample's
  // first render is still in flight is dropped by the busy worker (the same
  // fragility the native replay tests defer around), and the whole drag then
  // runs as a marquee that never produces a document render. Poll that
  // readiness explicitly rather than sleeping for it.
  await waitForBrowserComposite(page);
  await page.mouse.move(dragStart.x, dragStart.y);
  await waitForAppliedPointer(page, dragStart, {
    message: "drag press",
    timeoutMs: scaledMs(4_000),
  });
  await waitForPressReadiness(page, "drag press");
  const resultsBeforePress = await page.evaluate(
    () => window.__donnerWorkerStats?.completedResults || 0,
  );
  await page.mouse.down();

  const samples: Array<{
    renderedFrames: number;
    coloredPixels: number;
    blue: PixelBounds;
    teal: PixelBounds;
  }> = [];
  const probeRegion = {
    x: editorBounds.x + 280,
    y: editorBounds.y + 260,
    width: 460,
    height: 280,
  };
  // An active drag presents by transforming the prewarmed selected-layer texture
  // inside UI frames; the worker does not re-rasterize the document until the
  // pointer releases. So the per-step signal that "the drag produced a frame" is
  // the UI frame counter, and the document counter staying flat across the whole
  // drag is itself part of the contract, asserted per step and after the release
  // below. The press selects the shape, which schedules exactly one prewarm
  // render of the selected layer; wait for it, or the drag baseline is captured
  // before it lands and the first step reads the prewarm as a mid-drag raster.
  const resultsDuringDrag = resultsBeforePress + 1;
  await expect
    .poll(() => readPressSelectionState(page), {
      message: "expected the press to select the shape and schedule its one prewarm render",
      timeout: scaledMs(2_000),
      intervals: [16, 25, 50, 100],
    })
    .toEqual({ completedResults: resultsDuringDrag, selectedCount: 1 });
  let previousFrames = await page.evaluate(() => window.__donnerMainLoopRenderedFrames || 0);
  for (let step = 1; step <= 16; ++step) {
    await page.mouse.move(dragStart.x + step * 6, dragStart.y + step * 3);
    await expect
      .poll(async () => page.evaluate(() => window.__donnerMainLoopRenderedFrames || 0), {
        message: `expected a drag-preview frame for drag step ${step}`,
        timeout: scaledMs(2_000),
        intervals: [16, 25, 50, 100],
      })
      .toBeGreaterThan(previousFrames);
    // A capture that straddles two frames can mix geometry from both, which
    // would fake the very defect this test looks for. The demand-driven loop
    // parks once it has presented the move, so retake until the frame count is
    // unchanged across the capture.
    let state: DocumentPresentationState | null = null;
    let geometry: { blue: PixelBounds | null; teal: PixelBounds | null } | null = null;
    for (let attempt = 0; attempt < 4; ++attempt) {
      const beforeState = await readDocumentPresentationState(page);
      geometry = await readEditorResizePixelBounds(page, probeRegion);
      const afterState = await readDocumentPresentationState(page);
      if (beforeState.renderedFrames === afterState.renderedFrames) {
        state = afterState;
        break;
      }
    }
    expect(state, `no stable capture for drag step ${step}`).not.toBeNull();
    if (state === null) {
      continue;
    }
    expect(geometry?.blue, `drag frame ${state.renderedFrames} had no blue document pixels`).not
      .toBeNull();
    // "No teal" has two very different causes and the pixels cannot tell them
    // apart: the chrome draw ran and its pixels missed this probe window, or no
    // chrome was drawn at all. The second means the render coordinator was not
    // holding a selection-chrome snapshot for that frame, which is what the
    // immediate-chrome plan - and with it the whole chrome pass - is installed
    // from. The coordinator holds that snapshot for as long as there is chrome
    // to draw and holds back only the recapture, so a frame without one is a
    // defect rather than ordinary timing between overlay rebuilds. The app
    // publishes which case it was in `selectionChromeSnapshotPresent`, so read
    // it here rather than leaving the next failure to guess. Read only on the
    // failing path: this test asserts on per-frame drag timing, and a probe that
    // costs a round trip per frame changes the thing it is measuring.
    if (geometry?.teal === null || geometry?.teal === undefined) {
      const chromeState = await page
        .evaluate(() => ({
          overlay: window.__donnerOverlayStats,
          frames: window.__donnerMainLoopRenderedFrames || 0,
          worker: window.__donnerWorkerStats,
          workerBusy: window.__donnerInteractionStats?.workerBusy,
        }))
        .catch((error: unknown) => ({ unavailable: String(error) }));
      console.log(
        `drag-teal-missing step=${step} frame=${state.renderedFrames} state=${
          JSON.stringify(chromeState)
        }`,
      );
    }
    expect(geometry?.teal, `drag frame ${state.renderedFrames} had no teal overlay pixels`).not
      .toBeNull();
    if (
      geometry?.blue === null || geometry?.blue === undefined || geometry.teal === null
      || geometry.teal === undefined
    ) {
      continue;
    }
    samples.push({
      renderedFrames: state.renderedFrames,
      blue: geometry.blue,
      coloredPixels: geometry.blue.pixels,
      teal: geometry.teal,
    });
    expect(
      state.completedResults,
      `drag frame ${state.renderedFrames} re-rasterized the document mid-drag`,
    ).toBe(resultsDuringDrag);
    previousFrames = state.renderedFrames;
  }
  await page.mouse.up();

  expect(samples.map((sample) => sample.renderedFrames)).toEqual(
    [...samples.map((sample) => sample.renderedFrames)].sort((a, b) => a - b),
  );
  expect(samples.at(-1)?.renderedFrames || 0).toBeGreaterThan(
    samples[0]?.renderedFrames || 0,
  );
  // The release commits the moved geometry with exactly one document render.
  await expectWorkerResultsToReach(
    page,
    (completedResults) => completedResults === resultsDuringDrag + 1,
    {
      message: "expected the pointer release to commit exactly one document render",
      timeout: scaledMs(2_000),
    },
  );
  const centerX = (bounds: PixelBounds) => (bounds.minX + bounds.maxX) * 0.5;
  const centerY = (bounds: PixelBounds) => (bounds.minY + bounds.maxY) * 0.5;
  for (const sample of samples) {
    expect(
      Math.abs(centerX(sample.blue) - centerX(sample.teal)),
      `drag frame ${sample.renderedFrames} showed historical document X geometry: `
        + `blue=${JSON.stringify(sample.blue)} teal=${JSON.stringify(sample.teal)}`,
    ).toBeLessThanOrEqual(3);
    expect(
      Math.abs(centerY(sample.blue) - centerY(sample.teal)),
      `drag frame ${sample.renderedFrames} showed historical document Y geometry: `
        + `blue=${JSON.stringify(sample.blue)} teal=${JSON.stringify(sample.teal)}`,
    ).toBeLessThanOrEqual(3);
  }
  const blueCenters = samples.map((sample) => centerX(sample.blue));
  expect(blueCenters).toEqual([...blueCenters].sort((a, b) => a - b));
  expect(blueCenters.at(-1) || 0).toBeGreaterThan(blueCenters[0] || 0);
  expect(
    samples.map((sample) => sample.coloredPixels),
    `baseline=${baseline.coloredPixels}; frames=${JSON.stringify(samples)}`,
  ).not.toContain(0);
  expect(failures).toEqual([]);
});

test("Firefox never exposes the checkerboard while dragging a Splash letter", async ({ browserName, page }) => {
  test.skip(browserName !== "firefox", "Firefox Geode regression");
  const failures = await openEditor(page);
  // Open through the shared helper so the sample's first document render has to
  // complete before anything is measured. Clicking the picker and going straight
  // to pixels raced the load: the picker is still on screen for as long as the
  // first Splash raster takes, and every assertion below then describes the
  // picker instead of the document.
  await openDonnerSplash(page);

  const viewport = await readViewportStats(page);
  const documentRegion = presentedDocumentRegion(viewport);
  const letterWindow = splashLetterTrackingWindow(viewport);
  expect(
    letterWindow.x >= documentRegion.x && letterWindow.y >= documentRegion.y
      && letterWindow.x + letterWindow.width <= documentRegion.x + documentRegion.width
      && letterWindow.y + letterWindow.height <= documentRegion.y + documentRegion.height,
    `the letter tracking window left the presented document: `
      + `window=${JSON.stringify(letterWindow)} document=${JSON.stringify(documentRegion)} `
      + `viewport=${JSON.stringify(viewport)}`,
  ).toBe(true);

  // Poll for the coverage the baseline demands rather than for any coverage at
  // all: a partially presented document satisfies "greater than zero" and would
  // turn the assertion below into a race against the raster.
  await expect
    .poll(async () => {
      const frame = await captureSplashPresentationFrame(page, documentRegion, letterWindow);
      return frame.census.darkBackgroundPixels / frame.census.samples;
    }, {
      message: "expected the Splash document to cover its own presented rectangle before the drag",
      timeout: scaledMs(5_000),
      intervals: [16, 25, 50, 100],
    })
    .toBeGreaterThan(0.25);
  const restFrame = await captureSplashDragFrame(page, documentRegion, letterWindow, "pre-press");
  const baseline = restFrame.census;
  expect(
    baseline.darkBackgroundPixels,
    `Splash background was not present before drag: ${JSON.stringify(baseline)}`,
  ).toBeGreaterThan(baseline.samples * 0.25);
  expect(
    baseline.checkerboardPixels,
    `checkerboard leaked through the Splash baseline: ${JSON.stringify(baseline)}`,
  ).toBeLessThan(baseline.samples * 0.1);
  expect(
    restFrame.letter,
    `the rendered Splash D was not present before the drag: ${JSON.stringify(baseline)}`,
  ).not.toBeNull();
  expect(
    restFrame.outline,
    "a selection outline was already inside the letter window before anything was selected",
  ).toBeNull();

  // The document has no element of its own since the single-canvas
  // architecture, so the press is placed by mapping the letter's own document
  // coordinates through the editor's published viewport geometry. Deriving the
  // press from a measured yellow bound instead is what made this test vacuous:
  // the search window clipped the letter, so the measured `minX` was pinned to
  // the window edge, the press landed a fixed 10px inside it on whatever
  // happened to be there, and the letter never moved - which every assertion
  // below then confirmed against byte-identical bounds.
  const dragStart = splashDocumentToPage(viewport, kSplashLetterD.stemPress);
  await page.mouse.move(dragStart.x, dragStart.y);
  await waitForAppliedPointer(page, dragStart, {
    message: "Splash drag press",
    timeoutMs: scaledMs(4_000),
  });
  await waitForPressReadiness(page, "Splash drag press");
  const resultsBeforePress = await page.evaluate(
    () => window.__donnerWorkerStats?.completedResults || 0,
  );
  await page.mouse.down();
  // The press selects the letter, which schedules exactly one prewarm render of
  // the selected layer; the drag baseline is taken once that has landed so the
  // first step does not read it as a mid-drag raster.
  const resultsDuringDrag = resultsBeforePress + 1;
  await expect
    .poll(() => readPressSelectionState(page), {
      message: "expected the press to select the letter and schedule its one prewarm render",
      timeout: scaledMs(2_000),
      intervals: [16, 25, 50, 100],
    })
    .toEqual({ completedResults: resultsDuringDrag, selectedCount: 1 });

  // One selected element is not yet proof that it is the RIGHT element: a press
  // that misses the letter and lands on the artboard behind it also reports
  // one. Nor does the completed prewarm render prove its selection chrome has
  // reached the browser composite: the overlay refresh is an idle follow-up
  // after that worker result. Wait on the actual visible outline, bounded. A
  // genuinely wrong selection still never puts an outline in the letter
  // corridor and fails below with the final full-document capture attached.
  // Each retake logs the editor's published frame-loop and overlay state so a
  // missing outline is attributable: a parked frame counter with the chrome
  // snapshot present means the outline was drawn but never presented; a
  // growing suppression counter means the version gate hid it; an advancing
  // frame counter with the snapshot absent means the chrome was genuinely
  // lost.
  const readOutlineWaitProbe = () =>
    page.evaluate(() => ({
      atMs: Math.round(performance.now()),
      completedResults: window.__donnerWorkerStats?.completedResults || 0,
      overlay: window.__donnerOverlayStats,
      renderedFrames: window.__donnerMainLoopRenderedFrames || 0,
    }));
  const outlineWaitTimeline: Array<Awaited<ReturnType<typeof readOutlineWaitProbe>>> = [];
  let pressFrame = await captureSplashDragFrame(page, documentRegion, letterWindow, "press");
  outlineWaitTimeline.push(await readOutlineWaitProbe());
  const pressOutlineDeadline = Date.now() + scaledMs(750);
  while (pressFrame.outline === null && Date.now() < pressOutlineDeadline) {
    await waitForBrowserComposite(page);
    pressFrame = await captureSplashDragFrame(
      page,
      documentRegion,
      letterWindow,
      "press outline",
    );
    outlineWaitTimeline.push(await readOutlineWaitProbe());
  }
  expect(pressFrame.letter, "the press lost the Splash letter").not.toBeNull();
  if (pressFrame.outline === null) {
    // The letter window is a corridor a few dozen pixels wide, so "no outline
    // in it" cannot say which failure this is: chrome drawn around the root
    // group instead of the letter, chrome that never reached the screen, and
    // chrome nobody can see all read the same here. The capture is of the
    // whole presented document, so attaching it answers that directly.
    await test.info().attach("press-frame", {
      body: pressFrame.png,
      contentType: "image/png",
    });
  }
  expect(
    pressFrame.outline,
    `the press selected something other than the Splash letter it aimed at: `
      + `frame=${JSON.stringify({ letter: pressFrame.letter, outline: pressFrame.outline })} `
      + `window=${JSON.stringify(letterWindow)} press=${JSON.stringify(dragStart)} `
      + `published=${
        JSON.stringify(
          await page.evaluate(() => ({
            interaction: window.__donnerInteractionStats,
            renderedFrames: window.__donnerMainLoopRenderedFrames || 0,
            worker: window.__donnerWorkerStats,
          })),
        )
      } outlineWaitTimeline=${JSON.stringify(outlineWaitTimeline)}`,
  ).not.toBeNull();
  const letterAtRest = pressFrame.letter!;
  const outlineAtRest = pressFrame.outline!;
  expect(
    Math.abs(outlineAtRest.minX - letterAtRest.minX),
    `the selection outline is not around the pressed letter: `
      + `letter=${JSON.stringify(letterAtRest)} outline=${JSON.stringify(outlineAtRest)}`,
  ).toBeLessThanOrEqual(kSelectionHandleMargin);

  const samples: Array<{
    census: SplashToneCensus;
    letter: PixelBounds | null;
    outline: PixelBounds | null;
    renderedFrames: number;
    step: number;
  }> = [];
  // The letter drag presents by transforming the prewarmed layer texture in UI
  // frames rather than re-rasterizing the Splash per move, so each step waits on
  // the frame counter to prove the loop ran at all.
  //
  // The counter cannot say WHICH step a frame presented: it advances for any
  // wake, including one already in flight when the move went out, so a step can
  // be handed a frame that predates its own pointer move. The letter's position
  // is what identifies the step, so a capture still showing the previous step's
  // position is retaken. A step whose move never reaches the screen fails here
  // with both positions named rather than quietly contributing a duplicate
  // sample.
  let previousLetterMinX = letterAtRest.minX;
  for (let step = 1; step <= kSplashDragSteps; ++step) {
    const framesBeforeMove = await page.evaluate(
      () => window.__donnerMainLoopRenderedFrames || 0,
    );
    await page.mouse.move(
      dragStart.x + step * kSplashDragStep.x,
      dragStart.y + step * kSplashDragStep.y,
    );
    await expect
      .poll(async () => page.evaluate(() => window.__donnerMainLoopRenderedFrames || 0), {
        message: `expected a Splash drag-preview frame for step ${step}`,
        timeout: scaledMs(2_000),
        intervals: [16, 25, 50, 100],
      })
      .toBeGreaterThan(framesBeforeMove);
    await waitForBrowserComposite(page);
    let frame = await captureSplashDragFrame(
      page,
      documentRegion,
      letterWindow,
      `drag step ${step}`,
    );
    // A capture can also simply be earlier than the presentation: the loop parks
    // once it has presented the move, but the browser composite the screenshot
    // reads can trail it. Retake, bounded, while the letter still reports the
    // previous step's position.
    const stepDeadline = Date.now() + scaledMs(750);
    while (
      frame.letter !== null && frame.letter.minX >= previousLetterMinX
      && Date.now() < stepDeadline
    ) {
      await waitForBrowserComposite(page);
      frame = await captureSplashDragFrame(page, documentRegion, letterWindow, `drag step ${step}`);
    }
    samples.push({
      census: frame.census,
      letter: frame.letter,
      outline: frame.outline,
      renderedFrames: frame.renderedFrames,
      step,
    });
    expect(frame.letter, `Splash drag frame ${frame.renderedFrames} lost the Splash letter`).not
      .toBeNull();
    expect(
      frame.letter!.minX,
      `Splash drag step ${step} never reached the screen: the letter is still at `
        + `${previousLetterMinX}`,
    ).toBeLessThan(previousLetterMinX);
    previousLetterMinX = frame.letter!.minX;
    expect(
      await page.evaluate(() => window.__donnerWorkerStats?.completedResults || 0),
      `Splash drag frame ${frame.renderedFrames} re-rasterized the document mid-drag`,
    ).toBe(resultsDuringDrag);
  }
  await page.mouse.up();

  for (const sample of samples) {
    expect(
      sample.census.darkBackgroundPixels,
      `drag sample ${sample.step} exposed an incomplete document render: ${
        JSON.stringify(sample.census)
      }`,
    ).toBeGreaterThan(baseline.darkBackgroundPixels * 0.8);
    expect(
      sample.census.checkerboardPixels,
      `drag sample ${sample.step} flashed the document checkerboard: ${
        JSON.stringify(sample.census)
      }`,
    ).toBeLessThan(sample.census.samples * 0.1);
    expect(sample.outline, `drag frame ${sample.renderedFrames} lost the selection outline`).not
      .toBeNull();
  }

  // The letter has to have moved as far as the pointer did. Each step already
  // proved the letter left the previous step's position; this is what stops a
  // drag that merely jitters the letter from satisfying that, and it is what
  // the alignment claim below needs to mean anything - identical bounds every
  // frame are perfectly aligned.
  const letterPositions = samples.map((sample) => ({
    step: sample.step,
    letterMinX: sample.letter!.minX,
    letterMaxY: sample.letter!.maxY,
    outlineMinX: sample.outline!.minX,
  }));
  const firstSample = letterPositions[0];
  const lastSample = letterPositions.at(-1)!;
  const expectedTravelX = (lastSample.step - firstSample.step) * kSplashDragStep.x;
  const expectedTravelY = (lastSample.step - firstSample.step) * kSplashDragStep.y;
  expect(
    Math.abs(lastSample.letterMinX - firstSample.letterMinX - expectedTravelX),
    `the Splash letter did not follow the pointer: expected ${expectedTravelX}px of travel, `
      + `positions=${JSON.stringify(letterPositions)}`,
  ).toBeLessThanOrEqual(kLetterTravelTolerance);
  expect(
    Math.abs(lastSample.letterMaxY - firstSample.letterMaxY - expectedTravelY),
    `the Splash letter did not follow the pointer: expected ${expectedTravelY}px of travel, `
      + `positions=${JSON.stringify(letterPositions)}`,
  ).toBeLessThanOrEqual(kLetterTravelTolerance);

  // With the letter genuinely moving, this is the claim the test is named for:
  // every presented drag frame shows the letter and its outline at the same
  // place, never the letter at one epoch's geometry and the outline at
  // another's.
  const baselineAlignment = firstSample.letterMinX - firstSample.outlineMinX;
  const alignmentErrors = letterPositions.map((position) => ({
    step: position.step,
    error: Math.abs(position.letterMinX - position.outlineMinX - baselineAlignment),
    letterMinX: position.letterMinX,
    outlineMinX: position.outlineMinX,
  }));
  expect(
    Math.max(...alignmentErrors.map((sample) => sample.error)),
    `Splash drag frames diverged from their outlines: ${JSON.stringify(alignmentErrors)}`,
  ).toBeLessThanOrEqual(2);

  // The release commits the moved letter with exactly one document render.
  await expectWorkerResultsToReach(
    page,
    (completedResults) => completedResults === resultsDuringDrag + 1,
    {
      message: "expected the pointer release to commit exactly one Splash document render",
      timeout: scaledMs(2_000),
    },
  );
  expect(failures).toEqual([]);
});

test("WebKit Geode survives a burst of drag wakeups without fatal errors", async ({ browserName, page }) => {
  test.skip(browserName !== "webkit", "WebKit Geode regression");
  const failures = await openEditor(page);
  expect(await page.evaluate(() => window.__donnerBackend)).toBe("geode");

  const editorCanvas = page.locator("canvas#canvas");
  const editorBounds = await editorCanvas.boundingBox();
  expect(editorBounds).not.toBeNull();
  if (editorBounds === null) {
    return;
  }

  const beforeSample = await page.evaluate(() => window.__donnerWorkerStats?.completedResults || 0);
  await page.mouse.click(editorBounds.x + editorBounds.width * 0.5, editorBounds.y + 282);
  await expect(editorCanvas).toHaveAttribute("data-active-sample-id", "basic-shapes");
  await expectWorkerResultsToReach(
    page,
    (completedResults) => completedResults > beforeSample,
    {
      message: "expected Basic Shapes to finish presenting before the WebKit drag burst",
      timeout: scaledMs(2_000),
    },
  );

  const dragStart = {
    x: editorBounds.x + kBlueRectOffset.x,
    y: editorBounds.y + kBlueRectOffset.y,
  };
  const beforeDrag = await page.evaluate(() => window.__donnerWorkerStats?.completedResults || 0);
  await page.mouse.move(dragStart.x, dragStart.y);
  await page.mouse.down();
  for (let step = 1; step <= 48; ++step) {
    await page.mouse.move(dragStart.x + step * 2, dragStart.y + step);
    if (step % 8 === 0) {
      await page.evaluate(() => new Promise((resolve) => requestAnimationFrame(resolve)));
    }
  }
  await page.mouse.up();
  await expectWorkerResultsToReach(
    page,
    (completedResults) => completedResults > beforeDrag,
    {
      message: "expected WebKit to complete a render after the drag wakeup burst",
      timeout: scaledMs(2_000),
    },
  );
  expect(failures).toEqual([]);
});

/**
 * Dispatch `count` ctrl+wheel pinch-zoom notches, as Chromium delivers trackpad
 * pinch. All notches go out in one task so the editor sees the whole zoom delta
 * before it can render, which is exactly the trackpad case: the viewport runs
 * ahead of the document raster the worker has finished.
 */
async function pinchZoom(
  page: Page,
  at: { x: number; y: number },
  deltaY: number,
  count = 1,
): Promise<void> {
  await page.evaluate(({ x, y, deltaY, count }) => {
    const target = document.getElementById("canvas");
    if (target === null) {
      throw new Error("canvas not found");
    }
    for (let notch = 0; notch < count; ++notch) {
      target.dispatchEvent(
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
    }
  }, { ...at, deltaY, count });
}

// One presented picture, named by the worker result it drew.
//
// A result is published with no `presentedAtMs` and the frame that consumes it
// stamps the field exactly once, so the pair says both "which raster" and
// "has a frame drawn it". Both halves come out of one round trip, so a gate
// cannot pair a counter from one frame with a stamp from another.
interface PresentedGeneration {
  completedResults: number;
  presentedAtMs: number | undefined;
}

async function readPresentedGeneration(page: Page): Promise<PresentedGeneration> {
  return page.evaluate(() => ({
    completedResults: window.__donnerWorkerStats?.completedResults || 0,
    presentedAtMs: window.__donnerWorkerStats?.presentedAtMs,
  }));
}

/**
 * Send one pinch notch and return once the raster it caused is on the canvas.
 *
 * A constant delay after a notch is not an observable of that notch. The worker
 * can take longer than any constant, and until the frame that draws its result
 * runs, the pane still shows the pre-gesture picture - which a coverage probe
 * scores as covered, because it was. The loop then decides on a frame the
 * gesture never touched, and the next notch supersedes the one that would have
 * shown the defect: with the notch's presentation held past a 200 ms wait, the
 * probe scored zero on all six storm notches while the frame each notch
 * actually presented carried 36375 uncovered backdrop pixels.
 *
 * Waiting for a strictly later presented result closes that window, and it is
 * bounded: a notch whose raster never reaches the canvas fails here with the
 * worker's own health attached rather than being scored as covered.
 */
async function pinchZoomAndAwaitPresentation(
  page: Page,
  at: { x: number; y: number },
  deltaY: number,
  context: string,
): Promise<void> {
  const before = await readPresentedGeneration(page);
  await pinchZoom(page, at, deltaY);
  await expectWorkerResultsToReach(
    page,
    (completedResults, health) =>
      completedResults > before.completedResults && health.presentedAtMs !== undefined,
    {
      message: `${context}: the zoom notch never presented a newer document raster`,
      timeout: scaledMs(5_000),
    },
  );
  await waitForBrowserComposite(page);
}

// Locate the Donner Splash "D" by its own yellow pixels, in CSS coordinates
// relative to `region`. Since the single-canvas architecture the document has no element to
// measure, so every document-space probe is anchored on rendered content.
async function readSplashLetterBounds(
  page: Page,
  region: CssRegion,
): Promise<PixelBounds | null> {
  const stats = await readSplashCompositeFrameStats(page, region, {
    minX: 0,
    minY: 0,
    maxX: region.width,
    maxY: region.height,
  });
  return stats.yellow;
}

async function openDonnerSplash(page: Page): Promise<{
  editorBounds: { x: number; y: number; width: number; height: number };
}> {
  const editorCanvas = page.locator("canvas#canvas");
  const editorBounds = await editorCanvas.boundingBox();
  expect(editorBounds).not.toBeNull();
  if (editorBounds === null) {
    throw new Error("editor canvas is missing");
  }
  const before = await page.evaluate(() => ({
    completedResults: window.__donnerWorkerStats?.completedResults || 0,
    renderedFrames: window.__donnerMainLoopRenderedFrames || 0,
  }));
  await page.mouse.click(editorBounds.x + editorBounds.width * 0.24, editorBounds.y + 282);
  await expect(editorCanvas).toHaveAttribute("data-active-sample-id", "donner-splash");
  // Wait for the Splash's raster to reach the canvas, not for the worker to
  // publish it.
  //
  // Those are two events and every pixel probe downstream is about the second
  // one. The editor already says which: a worker result is published with no
  // `presentedAtMs`, and the frame that consumes it stamps that field exactly
  // once when it finishes (`main.cc`). Gating on `completedResults` alone
  // returns inside the window between them - the run that sent this gate back
  // for repair published at 2087 ms and presented at 2139 ms, where the two
  // animation frames this used to wait for are ~33 ms - and because the editor
  // is a single canvas the pane still carries whatever was last presented for
  // the whole of it, which is not the sample under test. `smoke.spec.ts`
  // already gates on this handoff and calls it `pixelsPresented`.
  await expectWorkerResultsToReach(
    page,
    (completedResults, health) =>
      health.activeSampleId === "donner-splash"
      && completedResults > before.completedResults
      && health.renderedFrames > before.renderedFrames
      && health.presentedAtMs !== undefined,
    {
      message: "Donner Splash must present a document frame in the editor canvas",
      timeout: scaledMs(5_000),
    },
  );
  await waitForBrowserComposite(page);
  return { editorBounds };
}

// Whether the presented document covers the whole probe region.
//
// The claim is that the pane backdrop is never exposed inside the region, so
// the acceptance value is none of it. A fraction of the region was tolerated
// instead, and 0.2% of the 360x300 probe is 216 pixels: a fully uncovered row
// across the region's width would have scored as covered.
//
// That fraction was absorbing the backdrop tone window's own +-2 slack rather
// than anything about the presented picture. Counted exactly, the only pixels
// the slack added are the artboard's antialiased top edge (32 of 27752 at the
// natural fit, and an edge only exists where the document does not cover the
// region) and one (44,47,57) art pixel of 108,000 at the depth the zoom-in loop
// stops at. `canvas-color-stats` now counts the calibrated tone exactly, which
// leaves a covered region at zero and nothing for a floor to absorb.
function isProbeRegionCovered(stats: EditorBackgroundCoverageStats): boolean {
  return stats.editorBackgroundPixels === 0;
}

// How much of the probe region the render pane's own tones must account for
// before a capture may be scored.
//
// A low background count is the zoom test's success condition, so a low
// background count has to mean one thing. A capture of something that is not
// the render pane also scores low, and the editor is a single canvas that keeps
// showing the last presented frame, so "not the render pane" is a state the
// suite really reaches: the welcome sample picker measures 0 backdrop and 1041
// document pixels of 108,000, together under 1% of the region. A presented
// Splash accounts for 55% of it at the natural fit, 29% at the depth the
// zoom-in loop stops at, and 66% at the headroom depth the storm works around,
// all of it backdrop, artboard, checkerboard or letter. The floor sits an order
// of magnitude above the unusable case and a factor of three below the
// tightest usable one.
//
// This is deliberately stricter than `isSplashCaptureUsable`, which asks only
// that a capture carry some editor tone. That rules out a flat or empty
// capture, which is the class the drag suite hit; it does not rule out a
// perfectly good picture of a different part of the editor, which is the class
// this probe hit.
const kPaneObservationFraction = 0.1;

// How long to wait between two reads of the same probe.
//
// Both bounded waits below poll the same observable through the same
// screenshot, so they wait the same amount between attempts: one interval is
// short enough that a settle costs little on a fast runner, and the deadlines
// that contain them are what scale.
const kProbeRetakeIntervalMs = 50;

/**
 * Read the probe region, and refuse to score a capture that is not an
 * observation of the render pane.
 *
 * The background count and the presence check come out of the same capture, so
 * they cannot describe different frames, and they read disjoint tone windows,
 * so neither can fake the other.
 *
 * Retakes are bounded, and a capture that never shows the pane fails with the
 * picture itself attached alongside its histogram and the editor's published
 * state, so the next reader sees what was actually on screen instead of a bare
 * number.
 */
async function readProbeCoverage(
  page: Page,
  region: CssRegion,
  context: string,
): Promise<EditorBackgroundCoverageStats> {
  // The transient this absorbs is one publish-to-present handoff, measured at
  // ~52 ms on the run that produced the failure, so the retake window is a
  // deadline rather than a fixed number of animation frames: four composites
  // are ~130 ms on a fast runner and can be less than one handoff on a loaded
  // one, and running out of retries throws, which would abort a caller that
  // was prepared to wait.
  const deadline = Date.now() + scaledMs(1_000);
  let last: EditorBackgroundCoverageStats | null = null;
  for (;;) {
    last = await readEditorBackgroundCoverage(page, region);
    if (
      last.editorBackgroundPixels + last.documentPixels
        >= last.samples * kPaneObservationFraction
    ) {
      return last;
    }
    if (Date.now() >= deadline) {
      break;
    }
    await page.waitForTimeout(kProbeRetakeIntervalMs);
  }
  await test.info().attach(`no-render-pane-${context}`, {
    body: last.png,
    contentType: "image/png",
  });
  const published = await page.evaluate(() => ({
    frameLoop: window.__donnerFrameLoopStats,
    renderedFrames: window.__donnerMainLoopRenderedFrames || 0,
    viewport: window.__donnerViewportStats,
    worker: window.__donnerWorkerStats,
  }));
  throw new Error(
    `${context}: no capture of the presented render pane before the deadline. `
      + `region=${JSON.stringify(region)} census=${JSON.stringify(last.census)} `
      + `published=${JSON.stringify(published)}`,
  );
}

/**
 * Poll the probe until the document covers it, bounded.
 *
 * Presentation lags the live viewport by up to one worker latency, so a single
 * read taken a fixed interval after a zoom notch scores whichever side of that
 * latency the runner happened to land on. Every assertion in the zoom storm
 * instead waits for convergence and then reports what it converged to; the
 * callers differ only in what they do with a deadline that expired.
 */
async function settleUntilCovered(
  page: Page,
  region: CssRegion,
  context: string,
): Promise<EditorBackgroundCoverageStats> {
  const deadline = Date.now() + scaledMs(1_000);
  let stats = await readProbeCoverage(page, region, context);
  while (!isProbeRegionCovered(stats) && Date.now() < deadline) {
    await page.waitForTimeout(kProbeRetakeIntervalMs);
    stats = await readProbeCoverage(page, region, context);
  }
  return stats;
}

// The Splash coverage probe's rectangle inside the editor canvas. Shared by the
// zoom storm and by the guard test below, which has to sample the same region
// the storm scores for its claim to be about the storm.
const kSplashProbeOffset = { x: 438, y: 128, width: 360, height: 300 };

function splashProbeRegion(
  editorBounds: { x: number; y: number; width: number; height: number },
): CssRegion {
  return {
    x: editorBounds.x + kSplashProbeOffset.x,
    y: editorBounds.y + kSplashProbeOffset.y,
    width: kSplashProbeOffset.width,
    height: kSplashProbeOffset.height,
  };
}

test("the Splash coverage probe refuses a capture that is not the render pane", async ({ page }) => {
  // Pin the ambiguity that produced the zoom storm's `coverage=[]` failure.
  //
  // The editor is a single canvas, so until a frame draws a sample's raster the
  // pane shows whatever was presented before it. The welcome sample picker is
  // one such picture and the only one a test can hold still, so sampling the
  // storm's own rectangle before any sample is opened reproduces the shape of
  // the ambiguity with no timing involved.
  const failures = await openEditor(page);
  const editorBounds = await page.locator("canvas#canvas").boundingBox();
  expect(editorBounds).not.toBeNull();
  if (editorBounds === null) {
    throw new Error("editor canvas is missing");
  }
  const probeRegion = splashProbeRegion(editorBounds);

  const picker = await readEditorBackgroundCoverage(page, probeRegion);
  // A bare background count cannot tell this frame from one the document fully
  // covers: both are far under the floor the storm treats as covered. That is
  // the whole defect - the storm's exit condition and "there is no document
  // here" were the same number, so the loop was skipped and the assertion one
  // screenshot later reported the settled coverage it had just declined to
  // zoom away.
  expect(
    isProbeRegionCovered(picker),
    `the welcome picker no longer scores as covered: ${JSON.stringify(picker.census)}`,
  ).toBe(true);
  // So the probe must refuse to score it rather than hand back the number.
  await expect(readProbeCoverage(page, probeRegion, "welcome picker")).rejects.toThrow(
    /no capture of the presented render pane/,
  );
  expect(failures).toEqual([]);
});

test("the Splash coverage probe counts editor background, not document pixels", async ({ page }) => {
  // Pin what the zoom storm's probe is allowed to count.
  //
  // The editor publishes where it put the document, so the probe's answer is
  // checkable against geometry rather than against a colour someone measured
  // once. At the natural fit the probe rectangle overhangs the artboard's top
  // edge, and the pixels above that edge - and only those - are editor
  // background. This is the assertion the pre-fix probe failed: it counted
  // (13,15,29), which is `#0d0f1d`, the Splash artboard's own fill, so it
  // reported 12242 where the overhang is 360 x 77.5 = 27900. Counting a tone
  // the document itself draws is what made "the document covers the region"
  // and "there is no document here" the same number.
  const failures = await openEditor(page);
  const { editorBounds } = await openDonnerSplash(page);
  const probeRegion = splashProbeRegion(editorBounds);
  const viewport = await readViewportStats(page);
  // The area identity below holds only while the top edge is the region's ONLY
  // uncovered side, so state that as a precondition rather than let a moved
  // layout surface as an unexplained count mismatch.
  const overhangRows = viewport.documentY - probeRegion.y;
  const layout = `viewport=${JSON.stringify(viewport)} probe=${JSON.stringify(probeRegion)}`;
  expect(overhangRows, `the probe no longer overhangs the artboard's top edge; ${layout}`)
    .toBeGreaterThan(0);
  expect(viewport.documentX, `the artboard no longer covers the probe's left edge; ${layout}`)
    .toBeLessThanOrEqual(probeRegion.x);
  expect(
    viewport.documentX + viewport.documentWidth,
    `the artboard no longer covers the probe's right edge; ${layout}`,
  ).toBeGreaterThanOrEqual(probeRegion.x + probeRegion.width);
  expect(
    viewport.documentY + viewport.documentHeight,
    `the artboard no longer covers the probe's bottom edge; ${layout}`,
  ).toBeGreaterThanOrEqual(probeRegion.y + probeRegion.height);
  const stats = await readProbeCoverage(page, probeRegion, "natural fit");
  // The capture may be taken at a device pixel ratio above 1; scale the
  // expected area into the capture's own pixels rather than assuming one.
  const captureScale = stats.samples / (probeRegion.width * probeRegion.height);
  const expectedBackground = overhangRows * probeRegion.width * captureScale;
  // A tenth of the overhang is far more slack than edge antialiasing needs
  // (measured 27720 against 27900, 0.6%) and far tighter than any tone the
  // document draws could survive.
  expect(
    stats.editorBackgroundPixels,
    `the probe is not measuring the artboard overhang: expected about `
      + `${expectedBackground}, census=${JSON.stringify(stats.census)}`,
  ).toBeGreaterThan(expectedBackground * 0.9);
  expect(
    stats.editorBackgroundPixels,
    `the probe counts more than the artboard overhang: expected about `
      + `${expectedBackground}, census=${JSON.stringify(stats.census)}`,
  ).toBeLessThan(expectedBackground * 1.1);
  expect(failures).toEqual([]);
});

test("a zoom storm never uncovers the editor background under the Donner Splash", async ({ page }) => {
  // The per-test budget is the one deadline in this suite that does not scale
  // with the runner. Every bound inside the storm does: eight bounded settles
  // of `scaledMs(1_000)`, each of which may spend another such deadline
  // retaking an unusable capture, exceed the config's flat 30 s on a shared
  // runner well before they are exhausted, so the slow run this test exists to
  // describe would be killed before it could print its census. Scale the budget
  // the same way the bounds inside it are scaled; a settled run takes 8.7 s.
  test.setTimeout(scaledMs(30_000));
  const failures = await openEditor(page);
  const { editorBounds } = await openDonnerSplash(page);

  // A rectangle strictly inside the render pane. Once the document is zoomed in
  // far enough to fill it, every frame that shows the bare editor background
  // inside it is showing background where document pixels belong. the single-canvas architecture
  // removed the CSS placement that produced that defect by scaling a stale
  // raster through the live viewport; the pixel claim is what outlives it.
  //
  // The region is centered on a solid stroke of the Splash letter (618, 278)
  // rather than the pane center: the zoom below anchors at the region center,
  // so the pixels that fill the region at depth are the anchor's own
  // neighborhood, and a bright stroke keeps the region unambiguously filled at
  // every zoom level. At the natural fit the artboard's top edge crosses the
  // region, so it starts with genuine uncovered backdrop: the editor publishes
  // `documentY` 77.5 CSS pixels below the region's top, and that 360 x 77.5
  // strip scores 27720 pane-backdrop pixels.
  const probeRegion = splashProbeRegion(editorBounds);
  const probeCenter = {
    x: probeRegion.x + probeRegion.width * 0.5,
    y: probeRegion.y + probeRegion.height * 0.5,
  };
  const probe = async (context: string) => readProbeCoverage(page, probeRegion, context);

  // The render pane classifies wheel input only while it is the hovered window.
  await page.mouse.move(probeCenter.x, probeCenter.y);
  const zoomInCoverage: number[] = [];
  for (let notch = 0; notch < 10; ++notch) {
    if (isProbeRegionCovered(await probe(`zoom-in notch ${notch}`))) {
      break;
    }
    await pinchZoomAndAwaitPresentation(page, probeCenter, -250, `zoom-in notch ${notch}`);
    // This read does not settle: the loop is zooming in precisely because the
    // region is not covered yet, so a read that catches a later notch's
    // presentation costs one extra notch and nothing else, and the value it
    // pushes is diagnostic. The assertions below, which do decide the test, all
    // settle on a deadline that scales with the runner.
    zoomInCoverage.push((await probe(`zoom-in notch ${notch}`)).editorBackgroundPixels);
  }
  const zoomedIn = await settleUntilCovered(page, probeRegion, "zoom-in settle");
  expect(
    zoomedIn.editorBackgroundPixels,
    `zooming in never covered the probe region with document pixels;`
      + ` coverage=${JSON.stringify(zoomInCoverage)} census=${JSON.stringify(zoomedIn.census)}`,
  ).toBe(0);
  expect(
    zoomInCoverage.length,
    `the document already covered the probe region without zooming: ${
      JSON.stringify(zoomInCoverage)
    }`,
  ).toBeGreaterThan(0);

  // Headroom before the storm. At the document's natural fit the probe region
  // genuinely overhangs the document by a few pixels, and the storm below
  // deliberately zooms out a notch at a time. Stopping the loop the instant
  // coverage first holds can leave the storm's own zoom-out landing back on
  // that legitimate overhang, which the coverage assertion cannot tell apart
  // from the defect this test guards. How much slack the loop happens to leave
  // depends on how far presentation lags the live viewport, so it is not
  // something to rely on: zoom in explicitly past the boundary.
  const kZoomHeadroomNotches = 2;
  for (let notch = 0; notch < kZoomHeadroomNotches; ++notch) {
    await pinchZoomAndAwaitPresentation(page, probeCenter, -250, `zoom headroom notch ${notch}`);
  }
  const headroom = await settleUntilCovered(page, probeRegion, "zoom headroom");
  expect(
    headroom.editorBackgroundPixels,
    `the zoom headroom notches left editor background inside the probe region;`
      + ` census=${JSON.stringify(headroom.census)}`,
  ).toBe(0);

  // Zoom out and back in. Each notch changes the live viewport by ~27% while
  // the worker is still finishing the previous raster, which is the window in
  // which a lagging document used to uncover the editor background.
  const scaleSamples: number[] = [];
  const stormCoverage: number[] = [];
  const uncovered: Array<
    { burst: number; phase: string; backgroundPixels: number; census: SplashToneCensus }
  > = [];
  for (let burst = 0; burst < 3; ++burst) {
    for (const phase of ["out", "in"] as const) {
      await pinchZoomAndAwaitPresentation(
        page,
        probeCenter,
        phase === "out" ? 250 : -250,
        `storm ${burst} ${phase}`,
      );
      // A zoom-out can expose regions whose tiles have not re-rastered yet;
      // that transient is bounded by one worker latency and is inherent to
      // demand-rendered tiles. The defect this storm hunts is uncovering that
      // PERSISTS - a stale placement that never converges - so each notch
      // asserts convergence within a bounded settle instead of failing on a
      // single instant read.
      const settled = await settleUntilCovered(page, probeRegion, `storm ${burst} ${phase}`);
      stormCoverage.push(settled.editorBackgroundPixels);
      if (!isProbeRegionCovered(settled)) {
        uncovered.push({
          burst,
          phase,
          backgroundPixels: settled.editorBackgroundPixels,
          census: settled.census,
        });
      }
      // At storm depth the letter overflows any pane-bounded region on some
      // viewports, so its measured width and edge positions can saturate at the
      // region bounds. The letter's yellow fill count keeps tracking the zoom
      // even when every edge clips: the stroke covers a different fraction of
      // the region at each zoom level.
      const letter = await readSplashLetterBounds(page, splashLetterMeasureRegion(editorBounds));
      if (letter !== null) {
        scaleSamples.push(letter.pixels);
      }
    }
  }

  // Guard against an inconclusive run: synthesized wheel notches are not always
  // accepted, and a storm the editor ignored proves nothing.
  expect(
    new Set(scaleSamples).size,
    `the zoom storm never changed the presented document scale: ${JSON.stringify(scaleSamples)}`,
  ).toBeGreaterThan(1);
  expect(
    uncovered,
    `zoom storm exposed the editor background inside the probe region;`
      + ` probe=${JSON.stringify(probeRegion)} first=${JSON.stringify(uncovered.slice(0, 6))}`,
  ).toEqual([]);
  console.log(
    `zoom-storm zoomIn=${JSON.stringify(zoomInCoverage)} storm=${JSON.stringify(stormCoverage)}`
      + ` scales=${JSON.stringify(scaleSamples)}`,
  );
  expect(failures).toEqual([]);
});

test("a size-commit re-rasterize reuses the pooled offscreen renderer", async ({ page }) => {
  // Regression pin for the ~200 ms zoom hitch: a canvas-size commit re-
  // rasterizes every compositor tile, and the worker used to construct and
  // tear down one offscreen renderer per tile (each teardown blocking on two
  // GPU-idle device polls). Tiles must now be served by the compositor's
  // pooled instance: a settled post-zoom re-rasterize frame reports zero
  // offscreen constructions while still rasterizing tiles. The tile count
  // also proves rasterization is not starved by the pooled pass - request and
  // callback delivery now rides the explicit between-tiles yield instead of
  // the removed teardown polls.
  const failures = await openEditor(page);
  const { editorBounds } = await openDonnerSplash(page);
  const zoomPoint = {
    x: editorBounds.x + 480,
    y: editorBounds.y + 320,
  };
  await page.mouse.move(zoomPoint.x, zoomPoint.y);

  // Settle the load, then snapshot the monotonic pool totals. The totals are
  // controller-lifetime, so the assertions below hold across whichever frames
  // the async stat polls happen to observe.
  const readPoolTotals = () =>
    page.evaluate(() => ({
      completedResults: window.__donnerWorkerStats?.completedResults ?? 0,
      offscreenCreateTotal: window.__donnerWorkerStats?.offscreenCreateTotal ?? -1,
      offscreenRecycleTotal: window.__donnerWorkerStats?.offscreenRecycleTotal ?? -1,
    }));
  // The totals publish with each completed result, one result behind the
  // compositor's live counters: the load-time pool construction happens in
  // the deferred warmup lane and only becomes visible in stats once the next
  // render lands, so a 0 baseline here is normal. Require only that the
  // fields exist.
  await expect
    .poll(async () => (await readPoolTotals()).offscreenCreateTotal, {
      message: "the loaded document never published offscreen pool totals",
      timeout: scaledMs(5_000),
    })
    .toBeGreaterThanOrEqual(0);
  const beforeZoom = await readPoolTotals();

  // Zoom in and hold still: past the canvas-size commit debounce the worker
  // re-rasterizes every compositor tile at the committed canvas size.
  await pinchZoom(page, zoomPoint, -250);

  // The recycle total must grow - the re-rasterize served its tiles from the
  // pool. Starvation of the rasterize pass (the failure mode of removing the
  // teardown polls without the between-tiles yield) times out here with the
  // total frozen.
  await expect
    .poll(async () => (await readPoolTotals()).offscreenRecycleTotal, {
      message: "no tile was rasterized from the pooled offscreen renderer after the size commit",
      timeout: scaledMs(8_000),
    })
    .toBeGreaterThan(beforeZoom.offscreenRecycleTotal);

  const afterZoom = await readPoolTotals();
  // Two constructions are legitimate in this window: the pool's initial
  // instance (invisible in the baseline when it happened in the deferred
  // warmup lane, see above) and one replacement after a superseding request
  // cancels a pass mid-draw (cancellation discards rather than recycles).
  // Per-tile construction - the regression this test pins - grows the total
  // by the full tile count for every rasterize pass instead.
  expect(
    afterZoom.offscreenCreateTotal,
    `the size-commit re-rasterize constructed offscreen renderers instead of reusing the pool:`
      + ` before=${JSON.stringify(beforeZoom)} after=${JSON.stringify(afterZoom)}`,
  ).toBeLessThanOrEqual(beforeZoom.offscreenCreateTotal + 2);
  expect(failures).toEqual([]);
});

test("trackpad pan moves the presented document content", async ({ page }) => {
  // Pan was doubly broken in browsers: it never requested a document render,
  // and placement was pinned to the raster's own viewport so the document could
  // not move between renders. Both fixes are pinned here on what the user sees:
  // a plain (non-ctrl) wheel over the pane must move the rendered artwork.
  const failures = await openEditor(page);
  const { editorBounds } = await openDonnerSplash(page);
  const paneRegion = renderPaneRegion(editorBounds);
  const panPoint = { x: editorBounds.x + 480, y: editorBounds.y + 320 };
  await page.mouse.move(panPoint.x, panPoint.y);
  const before = await readSplashLetterBounds(page, paneRegion);
  expect(before, "the Splash letter was not rendered before the pan").not.toBeNull();
  if (before === null) {
    return;
  }

  await page.evaluate(({ x, y }) => {
    for (let i = 0; i < 6; ++i) {
      document.getElementById("canvas")!.dispatchEvent(
        new WheelEvent("wheel", {
          bubbles: true,
          cancelable: true,
          clientX: x,
          clientY: y,
          deltaMode: WheelEvent.DOM_DELTA_PIXEL,
          deltaX: 0,
          deltaY: 40,
        }),
      );
    }
  }, panPoint);

  await expect
    .poll(
      async () => {
        const after = await readSplashLetterBounds(page, paneRegion);
        return after === null ? 0 : Math.abs(after.minY - before.minY);
      },
      {
        message: "a plain wheel pan never moved the rendered document content",
        timeout: scaledMs(5_000),
      },
    )
    .toBeGreaterThan(20);
  expect(failures).toEqual([]);
});

test("browser trackpad pinch matches the desktop zoom identity", async ({ page }) => {
  // A synthesized pinch (ctrl-flagged wheel with no physical key held)
  // carries deltaY = -100*ln(scale); the input bridge discriminates it and
  // applies the desktop calibration so the applied zoom equals the gesture's
  // scale. A pinch to 1.25x must scale the rendered artwork by 1.25 within
  // a few percent (raster snapping contributes the tolerance).
  const failures = await openEditor(page);
  const { editorBounds } = await openDonnerSplash(page);
  const zoomPoint = { x: editorBounds.x + 480, y: editorBounds.y + 320 };
  await page.mouse.move(zoomPoint.x, zoomPoint.y);

  // the single-canvas architecture leaves no element whose box tracks the zoom, so the zoom
  // observable is the rendered Splash letter's own width, measured in the wide
  // letter region (the shared pane probe region clips the letter to a fragment
  // whose width does not track the zoom).
  const letterRegion = splashLetterMeasureRegion(editorBounds);
  const letterWidth = async () => {
    const bounds = await readSplashLetterBounds(page, letterRegion);
    return bounds === null ? 0 : bounds.maxX - bounds.minX;
  };
  const beforeWidth = await letterWidth();
  expect(beforeWidth).toBeGreaterThan(0);

  const kTargetScale = 1.25;
  await page.evaluate(({ x, y, scale }) => {
    document.getElementById("canvas")!.dispatchEvent(
      new WheelEvent("wheel", {
        bubbles: true,
        cancelable: true,
        clientX: x,
        clientY: y,
        ctrlKey: true,
        deltaMode: WheelEvent.DOM_DELTA_PIXEL,
        deltaY: -100 * Math.log(scale),
      }),
    );
  }, { ...zoomPoint, scale: kTargetScale });

  await expect
    .poll(async () => (await letterWidth()) / beforeWidth, {
      message: "the pinch never scaled the rendered document content",
      timeout: scaledMs(5_000),
    })
    .toBeGreaterThan(kTargetScale * 0.93);
  expect((await letterWidth()) / beforeWidth).toBeLessThan(kTargetScale * 1.07);
  expect(failures).toEqual([]);
});

// Reads the main-loop frame gate probe published by donner/editor/main.cc.
async function readFrameLoopStats(page: Page): Promise<FrameLoopStats> {
  const stats = await page.evaluate(() => window.__donnerFrameLoopStats);
  expect(stats, "the editor must publish __donnerFrameLoopStats").toBeTruthy();
  return stats as FrameLoopStats;
}

// Each burst delivers its notches inside one task, so a burst can only produce a small, bounded
// number of input-driven frames however slowly the engine runs. Two per burst leaves room for
// ImGui trickling a wheel transition across frames without letting a per-render wake pass.
const kStormBursts = 4;
const kMaxInputFramesPerBurst = 2;

// How long the storm's raster tail is allowed to keep waking the loop before it must park, and how
// long the parked loop is then held to running nothing at all.
const kParkTimeoutMs = 8_000;
const kParkProbeMs = 300;
const kQuietWindowMs = 1_500;
const kIdleWindowMs = 2_000;

// Ticks the animation-frame driver must have offered across an idle window for the "the loop
// declined them" claim to mean anything. Far below any plausible tick rate over the windows above,
// so this only fails when the driver stopped entirely.
const kMinDeclinedTicks = 10;

// Wait until the demand-driven loop stops running frames. The render worker keeps waking it for a
// tail after a gesture, so the park is a bounded wait rather than an instant claim; the quiet
// window the callers assert afterwards is the actual contract.
async function waitForFrameLoopToPark(page: Page): Promise<void> {
  await expect
    .poll(
      async () => {
        const first = (await readFrameLoopStats(page)).renderedFrames;
        await page.waitForTimeout(scaledMs(kParkProbeMs));
        return (await readFrameLoopStats(page)).renderedFrames - first;
      },
      {
        message: "the frame loop never stopped running frames once nothing was asking for one",
        timeout: scaledMs(kParkTimeoutMs),
        intervals: [50],
      },
    )
    .toBe(0);
}

test("a gesture storm runs frames only while the gesture is live", async ({ page }) => {
  const failures = await openEditor(page);
  const { canvasBounds } = await openBasicShapes(page);

  const paneCenter = { x: canvasBounds.width * 0.55, y: canvasBounds.height * 0.5 };
  // Park the pointer over the canvas and let ImGui's hover and tooltip delays saturate. Those
  // delays arm the loop's idle timer, so an unsettled pointer would keep waking frames and make
  // this test measure the wrong thing.
  await page.mouse.move(canvasBounds.x + paneCenter.x, canvasBounds.y + paneCenter.y);
  await page.waitForTimeout(scaledMs(1_500));
  await waitForFrameLoopToPark(page);

  const before = await readFrameLoopStats(page);
  const beforeDocument = await readDocumentPresentationState(page);

  // A gesture storm shaped like a real trackpad pinch: a burst of notches delivered in one task,
  // then a settle window in which the only thing waking the main loop is the render worker
  // publishing document rasters.
  for (let burst = 0; burst < kStormBursts; ++burst) {
    await pinchZoom(page, paneCenter, burst % 2 === 0 ? 70 : -70, 6);
    await page.waitForTimeout(scaledMs(350));
  }

  const afterStorm = await readFrameLoopStats(page);
  const afterStormDocument = await readDocumentPresentationState(page);
  const inputFrames = afterStorm.inputTriggeredFrames - before.inputTriggeredFrames;
  const stormFrames = afterStorm.renderedFrames - before.renderedFrames;
  const detail = `frames=${stormFrames} input=${inputFrames}`
    + ` document=${beforeDocument.completedResults}->${afterStormDocument.completedResults}`;

  // The document advanced across the storm, and the frames that carried it were the ones the storm
  // asked for. Every presented frame is a full UI frame in the single-canvas architecture, so the
  // wasted-work claim the old two-canvas split expressed as "present without rebuilding the UI" is
  // now expressed as "do not run a frame nobody asked for".
  expect(
    afterStormDocument.completedResults,
    `the document must re-rasterize across the storm (${detail})`,
  ).toBeGreaterThan(beforeDocument.completedResults);
  expect(inputFrames, `every burst must wake the loop (${detail})`)
    .toBeGreaterThanOrEqual(kStormBursts);
  expect(
    inputFrames,
    `a burst of notches must not wake a frame per notch (${detail})`,
  ).toBeLessThanOrEqual(kStormBursts * kMaxInputFramesPerBurst);

  // The storm's raster tail drains and the loop parks: no input, no editor request, no timer, so
  // no frames. A loop that free-runs at vsync instead is exactly the wasted full-UI work this test
  // has always existed to catch.
  await waitForFrameLoopToPark(page);
  const parked = await readFrameLoopStats(page);
  await page.waitForTimeout(scaledMs(kQuietWindowMs));
  const afterQuiet = await readFrameLoopStats(page);
  expect(
    afterQuiet.renderedFrames - parked.renderedFrames,
    "the settled storm tail must run no frames at all",
  ).toBe(0);

  // A synthetic hover is a DOM input event, so it must take the parked loop straight back to work.
  await page.mouse.move(canvasBounds.x + canvasBounds.width - 200, canvasBounds.y + 320);
  await expect
    .poll(async () => (await readFrameLoopStats(page)).renderedFrames, {
      message: "expected a synthetic hover to wake the parked frame loop",
      timeout: scaledMs(3_000),
      intervals: [16, 25, 50, 100],
    })
    .toBeGreaterThan(afterQuiet.renderedFrames);

  expect(failures).toEqual([]);
});

test("an idle editor parks the frame loop and wakes on demand", async ({ page }) => {
  const failures = await openEditor(page);
  const { canvasBounds } = await openBasicShapes(page);
  await waitForFrameLoopToPark(page);

  const parked = await readFrameLoopStats(page);
  await page.waitForTimeout(scaledMs(kIdleWindowMs));
  const afterIdle = await readFrameLoopStats(page);
  expect(
    afterIdle.renderedFrames - parked.renderedFrames,
    "an idle editor must run no frames",
  ).toBe(0);

  // Waking the loop is what publishes the ticks it declined while parked, so the wake and the
  // no-spinning claim are one measurement: the pinch must run frames, and the frame that reports
  // them must also report an idle window's worth of animation-frame ticks that ran no frame.
  const zoomPoint = { x: canvasBounds.width * 0.55, y: canvasBounds.height * 0.5 };
  await pinchZoom(page, zoomPoint, -70, 6);
  await expect
    .poll(async () => (await readFrameLoopStats(page)).renderedFrames, {
      message: "expected a pinch to wake the parked frame loop",
      timeout: scaledMs(3_000),
      intervals: [16, 25, 50, 100],
    })
    .toBeGreaterThan(afterIdle.renderedFrames);

  const woken = await readFrameLoopStats(page);
  expect(
    woken.inputTriggeredFrames,
    "the wake must be attributed to the DOM event that caused it",
  ).toBeGreaterThan(afterIdle.inputTriggeredFrames);
  expect(
    woken.callbacks - afterIdle.callbacks,
    "the driver must have kept clocking the loop across the idle window",
  ).toBeGreaterThanOrEqual(kMinDeclinedTicks);
  expect(failures).toEqual([]);
});
