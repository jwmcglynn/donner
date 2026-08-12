import { expect, type Page, test } from "@playwright/test";
import {
  captureSplashPresentationFrame,
  type CssRegion,
  isSplashCaptureUsable,
  type PixelBounds,
  readEditorBackgroundCoverage,
  readCssPngPixelDifferenceStats,
  readEditorPixelBoundsFromPng,
  readEditorResizePixelBounds,
  readEditorPixelBounds,
  readElementColorStats,
  readPngPixelDifferenceStats,
  readSplashCompositeFrameStats,
  type SplashPresentationFrame,
  type SplashToneCensus,
} from "./canvas-color-stats";

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
    };
    __donnerInteractionStats?: {
      dragging: boolean;
      pendingClick: boolean;
      publishedAtFrame: number;
      selectedCount: number;
      workerBusy: boolean;
    };
    __donnerFrameLoopStats?: FrameLoopStats;
    __donnerOverlayStats?: {
      compositorTileOverlay: boolean;
      geometryDebugOverlay: boolean;
    };
    __donnerViewportStats?: ViewportStats;
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

// Take one capture of the presented document and refuse to score an unusable
// one.
//
// Two ways a capture says nothing about the frame under test. It can straddle
// two presented frames and mix their geometry, which would fake the very defect
// this suite looks for; the demand-driven loop parks once it has presented the
// move, so retaking until the frame counter is unchanged across the capture
// settles that. And it can contain none of the editor's own tones at all -
// zero artboard, zero checkerboard, zero pane backdrop - which is not a
// partially rendered document but a capture of something that is not the
// editor, and is the shape of the sample a shared runner produced when this
// test last failed. Both are retaken, bounded; a capture window that never
// resolves fails with the histogram and the editor's published state attached,
// so the next reader sees what was actually on screen.
async function captureSplashDragFrame(
  page: Page,
  region: CssRegion,
  letterWindow: CssRegion,
  context: string,
): Promise<SplashPresentationFrame & { renderedFrames: number }> {
  let last: SplashPresentationFrame | null = null;
  for (let attempt = 0; attempt < 4; ++attempt) {
    await waitForParkedFrameLoop(page, scaledMs(2_000));
    const before = await readDocumentPresentationState(page);
    const frame = await captureSplashPresentationFrame(page, region, letterWindow);
    const after = await readDocumentPresentationState(page);
    last = frame;
    if (before.renderedFrames === after.renderedFrames && isSplashCaptureUsable(frame.census)) {
      return { ...frame, renderedFrames: after.renderedFrames };
    }
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

  const beforeSampleResults = await page.evaluate(
    () => window.__donnerWorkerStats?.completedResults || 0,
  );
  await page.mouse.click(canvasBounds.x + canvasBounds.width * 0.5, canvasBounds.y + 282);
  await expect(editorCanvas).toHaveAttribute("data-active-sample-id", "basic-shapes");
  await expect
    .poll(() => page.evaluate(() => window.__donnerWorkerStats?.completedResults || 0), {
      message: "Basic Shapes must have its own document render before capture",
      timeout: scaledMs(5_000),
      intervals: [16, 25, 50, 100],
    })
    .toBeGreaterThan(beforeSampleResults);
  await waitForBrowserComposite(page);

  return {
    canvasBounds,
    documentClip: {
      x: canvasBounds.x + 280,
      y: canvasBounds.y + 250,
      width: 660,
      height: 430,
    },
  };
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
  await expect
    .poll(() => page.evaluate(() => window.__donnerWorkerStats?.completedResults || 0), {
      message: "Compositor Tile Overlay must rasterize a fresh document render",
      timeout: scaledMs(2_000),
      intervals: [16, 25, 50, 100],
    })
    .toBeGreaterThan(beforeCompositorResults);
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
  await expect
    .poll(() => page.evaluate(() => window.__donnerWorkerStats?.completedResults || 0), {
      message: "Geometry Debug Overlay must schedule a freshly rasterized document render",
      timeout: scaledMs(2_000),
      intervals: [16, 25, 50, 100],
    })
    .toBeGreaterThan(beforeGeometryResults);
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
  const edgeDifference = readCssPngPixelDifferenceStats(geometryBaseline, geometryOverlay, documentClip, {
    x: sharedTriangleEdge.x - 3,
    y: sharedTriangleEdge.y - 3,
    width: 7,
    height: 7,
  });
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

test("Firefox keeps the dragged shape and its selection outline in every drag frame", async ({
  browserName,
  page,
}) => {
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
  await expect
    .poll(() => page.evaluate(() => window.__donnerWorkerStats?.completedResults || 0))
    .toBeGreaterThan(beforeSample);

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
  await expect
    .poll(async () => page.evaluate(() => window.__donnerWorkerStats?.completedResults || 0), {
      message: "expected the pointer release to commit exactly one document render",
      timeout: scaledMs(2_000),
      intervals: [16, 25, 50, 100],
    })
    .toBe(resultsDuringDrag + 1);
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

test("Firefox never exposes the checkerboard while dragging a Splash letter", async ({
  browserName,
  page,
}) => {
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
  // one. The letter window held no outline pixels before the press, so an
  // outline in it now, hugging the letter's own left edge, is the selection
  // this drag is about to move.
  const pressFrame = await captureSplashDragFrame(page, documentRegion, letterWindow, "press");
  expect(pressFrame.letter, "the press lost the Splash letter").not.toBeNull();
  expect(
    pressFrame.outline,
    `the press selected something other than the Splash letter it aimed at: `
      + `frame=${JSON.stringify({ letter: pressFrame.letter, outline: pressFrame.outline })}`,
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
  await expect
    .poll(async () => page.evaluate(() => window.__donnerWorkerStats?.completedResults || 0), {
      message: "expected the pointer release to commit exactly one Splash document render",
      timeout: scaledMs(2_000),
      intervals: [16, 25, 50, 100],
    })
    .toBe(resultsDuringDrag + 1);
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
  await expect
    .poll(() => page.evaluate(() => window.__donnerWorkerStats?.completedResults || 0), {
      message: "expected Basic Shapes to finish presenting before the WebKit drag burst",
      timeout: scaledMs(2_000),
      intervals: [16, 25, 50, 100],
    })
    .toBeGreaterThan(beforeSample);

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
  await expect
    .poll(() => page.evaluate(() => window.__donnerWorkerStats?.completedResults || 0), {
      message: "expected WebKit to complete a render after the drag wakeup burst",
      timeout: scaledMs(2_000),
      intervals: [16, 25, 50, 100],
    })
    .toBeGreaterThan(beforeDrag);
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
  const beforeSampleResults = await page.evaluate(
    () => window.__donnerWorkerStats?.completedResults || 0,
  );
  await page.mouse.click(editorBounds.x + editorBounds.width * 0.24, editorBounds.y + 282);
  await expect(editorCanvas).toHaveAttribute("data-active-sample-id", "donner-splash");
  await expect
    .poll(() => page.evaluate(() => window.__donnerWorkerStats?.completedResults || 0), {
      message: "Donner Splash must produce a document render",
      timeout: scaledMs(5_000),
      intervals: [16, 25, 50, 100],
    })
    .toBeGreaterThan(beforeSampleResults);
  return { editorBounds };
}

test("a zoom storm never uncovers the editor background under the Donner Splash", async ({ page }) => {
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
  // neighborhood. A bright stroke stays unambiguously non-background at every
  // zoom level, where the letter counters fill with dark cloud gradient that
  // sweeps the backdrop's color neighborhood. At the natural fit the artboard's
  // top edge crosses the region, so it starts with genuine uncovered backdrop.
  const probeRegion = {
    x: editorBounds.x + 438,
    y: editorBounds.y + 128,
    width: 360,
    height: 300,
  };
  const probeCenter = {
    x: probeRegion.x + probeRegion.width * 0.5,
    y: probeRegion.y + probeRegion.height * 0.5,
  };
  const backgroundPixels = async () =>
    (await readEditorBackgroundCoverage(page, probeRegion)).editorBackgroundPixels;

  // The render pane classifies wheel input only while it is the hovered window.
  await page.mouse.move(probeCenter.x, probeCenter.y);
  const zoomInCoverage: number[] = [];
  for (let notch = 0; notch < 10 && (await backgroundPixels()) > 0; ++notch) {
    await pinchZoom(page, probeCenter, -250);
    await page.waitForTimeout(200);
    zoomInCoverage.push(await backgroundPixels());
  }
  expect(
    await backgroundPixels(),
    `zooming in never filled the probe region with document pixels;`
      + ` coverage=${JSON.stringify(zoomInCoverage)}`,
  ).toBe(0);
  expect(
    zoomInCoverage.length,
    `the document already filled the probe region without zooming: ${
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
    await pinchZoom(page, probeCenter, -250);
    await page.waitForTimeout(200);
  }
  expect(
    await backgroundPixels(),
    "the zoom headroom notches left editor background inside the probe region",
  ).toBe(0);

  // Zoom out and back in. Each notch changes the live viewport by ~27% while
  // the worker is still finishing the previous raster, which is the window in
  // which a lagging document used to uncover the editor background.
  const scaleSamples: number[] = [];
  const uncovered: Array<{ burst: number; phase: string; backgroundPixels: number }> = [];
  for (let burst = 0; burst < 3; ++burst) {
    for (const phase of ["out", "in"] as const) {
      await pinchZoom(page, probeCenter, phase === "out" ? 250 : -250);
      await page.waitForTimeout(200);
      // A zoom-out can expose regions whose tiles have not re-rastered yet;
      // that transient is bounded by one worker latency and is inherent to
      // demand-rendered tiles. The defect this storm hunts is uncovering that
      // PERSISTS - a stale placement that never converges - so each notch
      // asserts convergence back to zero within a bounded settle instead of
      // failing on a single instant read.
      let persisted = await backgroundPixels();
      if (persisted > 0) {
        const settleDeadline = Date.now() + scaledMs(1_000);
        while (persisted > 0 && Date.now() < settleDeadline) {
          await page.waitForTimeout(50);
          persisted = await backgroundPixels();
        }
      }
      if (persisted > 0) {
        uncovered.push({ burst, phase, backgroundPixels: persisted });
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
    `zoom-storm zoomIn=${JSON.stringify(zoomInCoverage)} scales=${JSON.stringify(scaleSamples)}`,
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
  const detail = `frames=${stormFrames} input=${inputFrames}` +
    ` document=${beforeDocument.completedResults}->${afterStormDocument.completedResults}`;

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
