import { expect, type Page, test } from "@playwright/test";
import { readFileSync } from "node:fs";
import { readEditorPixelBounds, readEditorPixelBoundsFromPng } from "./canvas-color-stats";
import {
  attachCompositedReadbacks,
  blackFrameStats,
  type CompositedProbeResult,
  type CompositedSample,
  contentMotionFraction,
  dragRegressions,
  installCompositedPixelClassifier,
  installCompositedProbe,
  readDocumentArtWidth,
  readViewportStats,
  startCompositedProbe,
  stopCompositedProbe,
  type ViewportStats,
  visibleDocumentRegion,
} from "./composited-probe";
import { dragStream, pointerClick } from "./gesture-streams";

/**
 * Composited-output invariants for SHAPE DRAG and CLICK-SELECT.
 *
 * WHAT THIS SUITE IS FOR
 *
 * `composited-invariants.spec.ts` covers viewport gestures: zoom, pan, pinch
 * gain. Every stream there mutates only the viewport, so the whole family is
 * blind to the pipeline that runs when the DOCUMENT changes on every event.
 * Dragging a shape is that pipeline: each pointer move rewrites a transform,
 * asks for a new document version, and has to get the result back on screen
 * before the next move arrives. Its failures are its own - the dragged object
 * jumping back to a position it already left, a blank frame at drag start, and
 * a selection whose outline never appears at all - and none of them is
 * reachable from a wheel stream.
 *
 * WHY THESE ASSERTIONS ARE PRESENTER-AGNOSTIC
 *
 * Written deliberately against what a user can see, not against how the pixels
 * got there. Every assertion below reads either the composited read-back of
 * the visible document surface or a page screenshot, and none of them names an
 * epoch mechanism, a surface slot, an overlay texture, or a bake flag. The
 * presentation architecture underneath is expected to be replaced; these are
 * the behaviors that must survive the replacement unchanged, so they are
 * written so that they can.
 *
 * The one exception is the epoch token, used only as an ORDERING label for the
 * samples in the pop-back test's diagnostic output. No assertion depends on it
 * existing.
 */

const kCiTimeScale = process.env.CI ? 4 : 1;
const scaledMs = (ms: number) => ms * kCiTimeScale;

const kBaseUrl = process.env.DONNER_WASM_BASE_URL || "http://127.0.0.1:8000";

test.use({ viewport: { width: 1600, height: 900 } });

/**
 * Boot the editor and start collecting fatal console output.
 *
 * Reimplemented here rather than imported from a sibling spec, following the
 * convention those specs set: a coverage lane that cannot run because an
 * unrelated file is mid-refactor is not coverage.
 */
async function openEditor(page: Page): Promise<string[]> {
  const failures: string[] = [];
  page.on("console", (message) => {
    if (
      /Failed to wake Wasm renderer pthread|Wasm renderer pthread wake rejected|Aborted|RuntimeError|UTILS_RELEASE_ASSERT/i
        .test(message.text())
    ) {
      failures.push(`[console:${message.type()}] ${message.text()}`);
    }
  });
  page.on("pageerror", (error) => failures.push(`[pageerror] ${error.message}`));

  await page.goto(kBaseUrl, { waitUntil: "domcontentloaded" });
  const hasWebGpu = await page.evaluate(() => "gpu" in navigator);
  if (process.env.DONNER_WASM_REQUIRE_WEBGPU === "1") {
    expect(hasWebGpu, "the required browser lane must expose navigator.gpu").toBe(true);
  }
  test.skip(!hasWebGpu, "Browser does not expose navigator.gpu");
  await expect
    .poll(
      () =>
        page.evaluate(() =>
          (window as unknown as { __donnerCanStartWasm?: boolean }).__donnerCanStartWasm
        ),
      { timeout: scaledMs(30_000) },
    )
    .toBe(true);
  await expect(page.locator("#status")).toBeHidden({ timeout: scaledMs(20_000) });
  return failures;
}

interface Rect {
  x: number;
  y: number;
  width: number;
  height: number;
}

/** The published pane and document geometry, once the editor has published it. */
async function readSettledViewportStats(page: Page): Promise<ViewportStats> {
  await expect
    .poll(async () => (await readViewportStats(page))?.documentWidth ?? 0, {
      message: "the editor must publish its pane and document geometry",
      timeout: scaledMs(10_000),
      intervals: [16, 25, 50, 100],
    })
    .toBeGreaterThan(0);
  const viewport = await readViewportStats(page);
  if (viewport === null) {
    throw new Error("viewport stats disappeared after publishing");
  }
  return viewport;
}

/**
 * Open the Donner Splash from the carousel and wait until it is really open.
 *
 * The readiness check is the layers panel's row count, which is the
 * page-visible form of "the editor has a document" - it reads "(no document)"
 * until the model is live, and until then the sample carousel is still the
 * modal on top and the render pane discards every gesture. The old check was a
 * whole-canvas chromatic extent, which the editor's own chrome satisfies before
 * the document exists, so it returned immediately and every gesture below raced
 * the load.
 *
 * `documentRect` is the presented document's screen rectangle as the editor
 * publishes it, NOT the canvas box. They were the same thing only while the
 * document had its own element; on the single canvas the box is the whole
 * editor, and mapping document coordinates through it puts every derived point
 * hundreds of pixels away from the shape it names.
 */
async function openDonnerSplash(page: Page): Promise<{ editorBounds: Rect; documentRect: Rect }> {
  const editorCanvas = page.locator("canvas#canvas");
  const editorBounds = await editorCanvas.boundingBox();
  expect(editorBounds, "the editor canvas is missing").not.toBeNull();
  if (editorBounds === null) {
    throw new Error("editor canvas is missing");
  }
  // The carousel's first thumbnail initializes and exercises the worker-owned offscreen WebGPU
  // renderer. Replacing it while that first-use operation is active forces the foreground render
  // through a cancellation handoff that is unrelated to the drag invariant and can monopolize the
  // first Firefox worker frame on a loaded runner. Let that one bounded initialization finish; the
  // document-worker result below remains the actual proof that the selected sample is live.
  await expect
    .poll(
      () =>
        page.evaluate(() => {
          const stats = (window as unknown as {
            __donnerSampleThumbnailStats?: {
              completed?: number;
              ready?: number;
              active?: boolean;
              pending?: boolean;
            };
          }).__donnerSampleThumbnailStats;
          if (!stats) return false;
          const completed = stats.completed ?? 0;
          return completed > 0 && (stats.ready ?? 0) > 0 && !stats.active && !stats.pending;
        }),
      {
        message: "the first offscreen thumbnail must settle before the drag sample replaces it",
        timeout: scaledMs(20_000),
        intervals: [16, 25, 50, 100],
      },
    )
    .toBe(true);
  const beforeSampleResults = await page.evaluate(
    () =>
      (window as unknown as { __donnerWorkerStats?: { completedResults?: number } })
        .__donnerWorkerStats?.completedResults ?? 0,
  );
  await page.mouse.click(editorBounds.x + editorBounds.width * 0.24, editorBounds.y + 282);
  await expect(editorCanvas).toHaveAttribute("data-active-sample-id", "donner-splash");
  await expect(editorCanvas).toBeVisible();
  await expect
    .poll(
      () =>
        page.evaluate((before) => {
          const diagnostics = window as unknown as {
            __donnerWorkerStats?: { completedResults?: number };
            __donnerSampleThumbnailStats?: unknown;
            __donnerFrameLoopStats?: unknown;
            __donnerInteractionStats?: unknown;
          };
          const worker = diagnostics.__donnerWorkerStats ?? null;
          return {
            reached: (worker?.completedResults ?? 0) > before,
            worker,
            sampleThumbnail: diagnostics.__donnerSampleThumbnailStats ?? null,
            frameLoop: diagnostics.__donnerFrameLoopStats ?? null,
            interaction: diagnostics.__donnerInteractionStats ?? null,
          };
        }, beforeSampleResults),
      {
        message: "Donner Splash must become the editor's live document",
        timeout: scaledMs(20_000),
        intervals: [16, 25, 50, 100],
      },
    )
    .toEqual(expect.objectContaining({ reached: true }));
  const viewport = await readSettledViewportStats(page);
  await expect
    .poll(() => readDocumentArtWidth(page, visibleDocumentRegion(viewport)), {
      message: "Donner Splash must present document pixels into the canvas",
      timeout: scaledMs(10_000),
      intervals: [16, 25, 50, 100],
    })
    .toBeGreaterThan(0);
  // The debounced canvas-size commit lands after the first frame; sampling
  // before it settles measures the load, not the gesture.
  await page.waitForTimeout(scaledMs(1_500));
  const settled = await readSettledViewportStats(page);
  return {
    editorBounds,
    documentRect: {
      x: settled.documentX,
      y: settled.documentY,
      width: settled.documentWidth,
      height: settled.documentHeight,
    },
  };
}

/**
 * The Donner_D left stem, in viewport CSS px.
 *
 * Document coordinates (282, 400) of the 892x512 Splash artboard, the same
 * target `browser-presentation-regression.spec.ts` drags. It is the letter's
 * solid stem rather than its counter, where hit-testing would correctly select
 * the background behind the glyph instead.
 */
function splashLetterStem(documentRect: Rect): { x: number; y: number } {
  return {
    x: documentRect.x + documentRect.width * (282 / 892),
    y: documentRect.y + documentRect.height * (400 / 512),
  };
}

/**
 * A read-back window covering the dragged letter and the corridor it travels.
 *
 * Padded generously around both endpoints so the object stays inside the
 * window for the whole gesture: a centroid computed over a window the object
 * leaves reports the remaining background, not a stalled drag.
 */
function letterTravelRegion(
  from: { x: number; y: number },
  dx: number,
  dy: number,
): { x: number; y: number; width: number; height: number } {
  const pad = 90;
  const minX = Math.min(from.x, from.x + dx) - pad;
  const minY = Math.min(from.y, from.y + dy) - pad;
  const maxX = Math.max(from.x, from.x + dx) + pad;
  const maxY = Math.max(from.y, from.y + dy) + pad;
  return { x: minX, y: minY, width: maxX - minX, height: maxY - minY };
}

const kOrderingDrag = {
  dx: -140,
  dy: -70,
  reversals: 3,
  reversalAmplitudePx: 90,
} as const;

const kBlueRectOffset = { x: 408, y: 353 };
const kOldOrderingCssPerSample = { x: 320 / 96, y: 250 / 96 };

function orderingTrajectoryRegion(
  initial: Rect,
): Rect & { sampleWidth: number; sampleHeight: number } {
  let minDx = 0;
  let maxDx = 0;
  let minDy = 0;
  let maxDy = 0;
  // Bound the exact continuous path used by dragStream. 4096 subdivisions put the largest
  // unsampled gap below 0.25 CSS px; the 4px AA/selection margin dominates that gap.
  for (let step = 0; step <= 4096; ++step) {
    const fraction = step / 4096;
    const swing = Math.sin(fraction * Math.PI * kOrderingDrag.reversals);
    const dx = kOrderingDrag.dx * fraction + kOrderingDrag.reversalAmplitudePx * swing;
    const dy = kOrderingDrag.dy * fraction + kOrderingDrag.reversalAmplitudePx * 0.5 * swing;
    minDx = Math.min(minDx, dx);
    maxDx = Math.max(maxDx, dx);
    minDy = Math.min(minDy, dy);
    maxDy = Math.max(maxDy, dy);
  }
  const margin = 4;
  const region = {
    x: initial.x + minDx - margin,
    y: initial.y + minDy - margin,
    width: initial.width + maxDx - minDx + margin * 2,
    height: initial.height + maxDy - minDy + margin * 2,
  };
  return {
    ...region,
    sampleWidth: Math.ceil(region.width / kOldOrderingCssPerSample.x),
    sampleHeight: Math.ceil(region.height / kOldOrderingCssPerSample.y),
  };
}

async function openBasicShapes(page: Page): Promise<{
  editorBounds: Rect;
  documentRect: Rect;
  blueBounds: Rect;
}> {
  const editorCanvas = page.locator("canvas#canvas");
  const editorBounds = await editorCanvas.boundingBox();
  expect(editorBounds, "the editor canvas is missing").not.toBeNull();
  if (editorBounds === null) throw new Error("editor canvas is missing");
  await expect.poll(() =>
    page.evaluate(() => {
      const stats = (window as unknown as {
        __donnerSampleThumbnailStats?: {
          completed?: number;
          ready?: number;
          active?: boolean;
          pending?: boolean;
        };
      }).__donnerSampleThumbnailStats;
      return !!stats && (stats.completed ?? 0) > 0 && (stats.ready ?? 0) > 0
        && !stats.active && !stats.pending;
    }), { timeout: scaledMs(20_000), intervals: [16, 25, 50, 100] }).toBe(true);
  const before = await page.evaluate(() =>
    (window as unknown as { __donnerWorkerStats?: { completedResults?: number } })
      .__donnerWorkerStats?.completedResults ?? 0
  );
  await page.mouse.click(editorBounds.x + editorBounds.width * 0.5, editorBounds.y + 282);
  await expect(editorCanvas).toHaveAttribute("data-active-sample-id", "basic-shapes");
  await expect.poll(
    () =>
      page.evaluate(
        (prior) =>
          ((window as unknown as { __donnerWorkerStats?: { completedResults?: number } })
            .__donnerWorkerStats?.completedResults ?? 0) > prior,
        before,
      ),
    { timeout: scaledMs(20_000), intervals: [16, 25, 50, 100] },
  ).toBe(true);
  // The debounced canvas-size commit lands after the first worker result. Bind the measured blue
  // bounds and trajectory ROI only after that geometry has settled.
  await page.waitForTimeout(scaledMs(1_500));
  const settled = await readSettledViewportStats(page);
  const documentRect = {
    x: settled.documentX,
    y: settled.documentY,
    width: settled.documentWidth,
    height: settled.documentHeight,
  };
  let blueBounds: Rect | null = null;
  await expect.poll(async () => {
    const shot = await page.screenshot({ clip: documentRect });
    const bounds = readEditorPixelBoundsFromPng(shot, "basic-blue", documentRect, {
      minX: 0,
      minY: 0,
      maxX: documentRect.width,
      maxY: documentRect.height,
    });
    blueBounds = bounds === null ? null : {
      x: documentRect.x + bounds.minX,
      y: documentRect.y + bounds.minY,
      width: bounds.maxX - bounds.minX,
      height: bounds.maxY - bounds.minY,
    };
    return bounds?.pixels ?? 0;
  }, {
    message: "Basic Shapes must present its unique blue rectangle before the drag probe starts",
    timeout: scaledMs(10_000),
    intervals: [16, 25, 50, 100],
  }).toBeGreaterThan(0);
  if (blueBounds === null) throw new Error("Basic Shapes blue rectangle is missing");
  return {
    editorBounds,
    documentRect: {
      ...documentRect,
    },
    blueBounds,
  };
}

/** Confirm the editor really has a selection, so a drag drags something. */
async function selectedCount(page: Page): Promise<number> {
  return page.evaluate(() =>
    (window as unknown as { __donnerInteractionStats?: { selectedCount?: number } })
      .__donnerInteractionStats?.selectedCount || 0
  );
}

/** The editor's own account of the interaction, for failure diagnostics. */
async function interactionStats(page: Page): Promise<unknown> {
  return page.evaluate(() =>
    (window as unknown as { __donnerInteractionStats?: unknown }).__donnerInteractionStats ?? null
  );
}

function assertProbeUsable(result: CompositedProbeResult, minimumSamples: number): void {
  expect(
    result.samples.length,
    `the probe collected ${result.samples.length} samples, expected at least ${minimumSamples};`
      + " the sampled window was too short to say anything about per-frame behavior",
  ).toBeGreaterThanOrEqual(minimumSamples);
  const usable = result.samples.filter((sample) => sample.drawOk).length;
  expect(
    usable / result.samples.length,
    `only ${usable}/${result.samples.length} samples produced composited pixels;`
      + " the read-back path is unavailable, so these invariants would pass vacuously",
  ).toBeGreaterThan(0.8);
}

// The composited read-back of a worker-owned WebGPU canvas is not free, and it
// runs on the same thread as the editor. Sampling every animation frame across
// a drag measurably depresses the frame rate it is measuring - on stock
// Firefox the same drag ran at 5.8 presented frames/s with the read-back
// active and 13.7/s without it. That is fine for the CORRECTNESS invariants
// here, which are per-transition and do not care how many frames a second the
// editor manages. It is why this suite deliberately asserts nothing about
// throughput; drag throughput belongs in a lane that does not perturb it.
const kMinimumProbeSamples = process.env.CI ? 12 : 16;

test.describe("composited drag invariants", () => {
  test("g: a shape drag never presents a position it already left", async ({ browserName, page }) => {
    // Keep the read-backs behind every candidate: this gesture drags the
    // rectangle past the artboard's top-left corner, where the editor pins the
    // selection position chip on top of it, and the images are what separate a
    // shape that moved from a shape with chrome over it.
    test.setTimeout(scaledMs(120_000));
    const failures = await openEditor(page);
    const { editorBounds, blueBounds } = await openBasicShapes(page);
    const target = { x: editorBounds.x + kBlueRectOffset.x, y: editorBounds.y + kBlueRectOffset.y };
    const measurement = orderingTrajectoryRegion(blueBounds);

    await pointerClick(page, target);
    await expect.poll(() => selectedCount(page), {
      message: "the click must select the Basic Shapes blue rectangle before dragging it",
      timeout: scaledMs(5_000),
      intervals: [16, 25, 50, 100],
    }).toBeGreaterThan(0);
    // Let the select click's own frame finish before pressing again. A press
    // that arrives while the click is still buffered is consumed as part of it
    // and the drag never starts.
    await page.waitForTimeout(scaledMs(800));

    // Measure the fixture's only blue object inside one fixed region that bounds the complete
    // sinusoidal trajectory. The fixed crop cannot follow the pointer and hide a stale frame.
    // Resolution remains at least as fine as the prior 320x250 CSS / 96x96 readback contract.
    await installCompositedProbe(page, {
      captureReadbacks: true,
      sampleRegionCss: measurement,
      sampleWidth: measurement.sampleWidth,
      sampleHeight: measurement.sampleHeight,
      minColorAlpha: 64,
      minColorSpread: 60,
      // Basic Shapes authors exactly one object in this colour, so the predicate
      // isolates the dragged rectangle from every other fixture shape. It does
      // not isolate it from the editor's own chrome: the selection position
      // chip paints over the rectangle here, which removes matching pixels
      // without moving the rectangle. That is why the ordering verdict needs
      // the extent as well as the centroid.
      colorMask: "basic-blue",
    });
    await startCompositedProbe(page);
    const stream = await dragStream(page, target, {
      durationMs: scaledMs(1_800),
      dx: kOrderingDrag.dx,
      dy: kOrderingDrag.dy,
      hz: 90,
      reversals: kOrderingDrag.reversals,
      reversalAmplitudePx: kOrderingDrag.reversalAmplitudePx,
    });
    await page.waitForTimeout(scaledMs(400));
    const result = await stopCompositedProbe(page, test.info(), stream);

    assertProbeUsable(result, kMinimumProbeSamples);

    // A centroid change needs object-identity evidence before it can establish an older frame.
    // Keep the existing latency window while retaining the exact images behind each candidate.
    const violations = dragRegressions(result.samples, stream.trace, 1.0, 2.0, scaledMs(150));
    if (violations.length > 0) {
      const baselineIndex = result.samples.findIndex((sample) =>
        sample.drawOk && sample.coloredWidth > 0
      );
      const indices = [
        baselineIndex,
        ...violations.slice(0, 5).flatMap((violation) => [
          violation.predecessorIndex,
          violation.sampleIndex,
          violation.sampleIndex + 1,
        ]),
      ];
      const readbacks = await attachCompositedReadbacks(page, test.info(), indices);
      console.log(`drag-readback-evidence ${JSON.stringify({ baselineIndex, ...readbacks })}`);
    }
    const presentedFrames = new Set(
      result.samples.filter((sample) => sample.drawOk && sample.coloredWidth > 0).map((sample) =>
        `${Math.round(sample.coloredCentroidX)}x${Math.round(sample.coloredCentroidY)}`
      ),
    );
    console.log(
      `drag-frame-monotonicity engine=${browserName} samples=${result.samples.length}`
        + ` presentedFrames=${presentedFrames.size} violations=${violations.length}`
        + ` pointerEvents=${stream.pointerEvents}`
        + ` meanIntervalMs=${stream.meanIntervalMs.toFixed(1)}`,
    );
    // An ignored drag proves nothing: the gesture has to have advanced the
    // presentation for ordering to say anything at all.
    expect(
      presentedFrames.size,
      `the drag produced too few presented frames to test ordering;`
        + ` stream={pointerEvents:${stream.pointerEvents},elapsedMs:${
          stream.elapsedMs.toFixed(0)
        }}`,
    ).toBeGreaterThan(2);
    expect(
      violations.slice(0, 5),
      `${violations.length} samples presented an OLDER frame than one already shown`,
    ).toEqual([]);
    expect(failures).toEqual([]);
  });

  test("h: a shape drag never blanks the document, including at drag start", async ({ browserName, page }) => {
    // GUARDS: the first-drag black frame. Pressing down on a shape and starting
    // to move it flashes the editor background for one frame before the first
    // dragged frame arrives.
    //
    // Two things can produce that flash and this test looks for both: a
    // presented document region that reads back empty, and a frame in which the
    // editor canvas was not on the page at all. The second is the one a
    // pixel-only check misses, because with no canvas there is nothing to read
    // back and the sample is simply skipped.
    //
    // Measured at 204c60176 on stock Chrome and stock Firefox: zero of either,
    // across the click, the press, and the first 400 ms of motion. Enforced at
    // that observation. NOTE for whoever sees this go red: this samples canvas
    // contents rather than the composited page, so a flash produced purely at
    // the compositor level would be invisible to the read-back half.
    test.setTimeout(scaledMs(120_000));
    const failures = await openEditor(page);
    const { documentRect } = await openDonnerSplash(page);
    const stem = splashLetterStem(documentRect);

    await installCompositedProbe(page);
    await startCompositedProbe(page);
    // Sample across the whole interaction: the select click, the press, and
    // the first motion. The reported symptom is on the FIRST drag, so the
    // window has to include the transition into dragging, not just the steady
    // state after it.
    await pointerClick(page, stem);
    await expect.poll(() => selectedCount(page), {
      message: "the click must select the Splash letter before dragging it",
      timeout: scaledMs(5_000),
      intervals: [16, 25, 50, 100],
    }).toBeGreaterThan(0);
    // Let the select click's own frame finish before pressing again. A press
    // that arrives while the click is still buffered is consumed as part of it
    // and the drag never starts.
    await page.waitForTimeout(scaledMs(800));
    await page.waitForTimeout(scaledMs(300));
    const stream = await dragStream(page, stem, {
      durationMs: scaledMs(1_200),
      dx: -120,
      dy: -60,
      hz: 90,
    });
    const result = await stopCompositedProbe(page, test.info(), stream);

    assertProbeUsable(result, kMinimumProbeSamples);

    const stats = blackFrameStats(result.samples, 40);
    const blankSurfaceSamples = result.samples
      .map((sample, index) => ({ index, sample }))
      .filter(({ sample }) => !sample.canvasPresent);
    console.log(
      `drag-black-frames engine=${browserName} samples=${result.samples.length}`
        + ` black=${stats.blackSamples} fraction=${stats.fraction.toFixed(4)}`
        + ` longestRun=${stats.longestRun} noSurface=${blankSurfaceSamples.length}`
        + ` pointerEvents=${stream.pointerEvents}`,
    );
    expect(
      blankSurfaceSamples.map(({ index }) => index).slice(0, 5),
      `${blankSurfaceSamples.length} sampled frames had no editor canvas on screen during a`
        + " drag; that is the one-frame blank the user reports",
    ).toEqual([]);
    expect(
      stats.longestRun,
      `the document was missing for ${stats.longestRun} consecutive composited frames`,
    ).toBeLessThanOrEqual(browserName === "firefox" ? 3 : 2);
    expect(failures).toEqual([]);
  });

  test("i: the presented document follows a shape drag", async ({ browserName, page }) => {
    // GUARDS: a drag that only updates the screen when the pipeline happens to
    // catch up. The dragged object's own motion is the observable, not the
    // surface's: a shape drag does not move the viewport, so every
    // surface-geometry assertion in the sibling suite is constant here and
    // would pass against a completely frozen document.
    //
    // Measured at 204c60176 over a monotone drag: the presented content
    // centroid moved in 0.35 (stock Chrome) to 0.71-0.86 (stock Firefox) of
    // sampled frames. The bound is set well under both, because the defect it
    // separates from is an order of magnitude lower: content that only moves
    // when some unrelated event forces a frame moves in a few percent of them.
    test.setTimeout(scaledMs(120_000));
    const failures = await openEditor(page);
    const { documentRect } = await openDonnerSplash(page);
    const stem = splashLetterStem(documentRect);

    await pointerClick(page, stem);
    await expect.poll(() => selectedCount(page), {
      message: "the click must select the Splash letter before dragging it",
      timeout: scaledMs(5_000),
      intervals: [16, 25, 50, 100],
    }).toBeGreaterThan(0);
    // Let the select click's own frame finish before pressing again. A press
    // that arrives while the click is still buffered is consumed as part of it
    // and the drag never starts.
    await page.waitForTimeout(scaledMs(800));

    // Sample only the letter and the corridor it travels through: over the
    // whole artboard the stationary majority of the document pins the centroid
    // and a working drag measures as zero motion.
    await installCompositedProbe(page, {
      sampleRegionCss: letterTravelRegion(stem, -160, -80),
      sampleWidth: 96,
      sampleHeight: 96,
      // The Splash artboard background is #10131e, whose channel spread is 14 -
      // just above the probe's default chromatic threshold of 12. With the
      // default the background counts as content, outnumbers the letter, and
      // pins the centroid at the window's center no matter what the letter
      // does. Raising the bar keeps only strongly chromatic pixels, which for
      // this artboard is the letter itself.
      minColorAlpha: 64,
      minColorSpread: 60,
    });
    await startCompositedProbe(page);
    const stream = await dragStream(page, stem, {
      durationMs: scaledMs(browserName === "firefox" ? 2_800 : 1_800),
      dx: -160,
      dy: -80,
      hz: 90,
    });
    const result = await stopCompositedProbe(page, test.info(), stream);

    assertProbeUsable(result, kMinimumProbeSamples);
    const motion = contentMotionFraction(result.samples);
    console.log(
      `drag-content-motion engine=${browserName} samples=${result.samples.length}`
        + ` moved=${motion.movedSamples}/${motion.comparedSamples}`
        + ` fraction=${motion.fraction.toFixed(3)} pointerEvents=${stream.pointerEvents}`,
    );
    expect(
      motion.fraction,
      `the presented document changed in only ${motion.movedSamples}/${motion.comparedSamples}`
        + " sampled frames during a continuous shape drag",
    ).toBeGreaterThanOrEqual(0.15);
    expect(failures).toEqual([]);
  });

  // FIXME(single-canvas follow-up): RED at 204c60176 and left red on purpose.
  //
  // Measured on hardware Chromium over a 2.4 s reversing drag: 6 regressions
  // in 149 samples, each one a presented displacement of 1.5-3 read-back px
  // AGAINST a pointer that moved 8-9 CSS px the other way, at frame-token
  // transitions 3->4, 21->22 and 44->45. The tokens are strictly increasing at
  // every one of them, so this is not out-of-order presentation - test (g)
  // passes on the same runs. It is a NEWER frame carrying an OLDER position,
  // which is what the user reports as the shape popping back mid-drag.
  //
  // Not fixed here: the mechanism lives in the epoch presentation machinery
  // that the single-canvas architecture deletes rather than repairs, and the operator's direction
  // is not to patch it in isolation. This test is written against the correct
  // behavior and flips to enforced when the single-canvas follow-up phases land. If it goes GREEN
  // before then, something fixed it - promote it rather than deleting it.
  test.fixme("j: the presented shape never moves against the drag", async ({ browserName, page }) => {
    // GUARDS: the drag pop-back, pixel side. (g) covers presentation ORDER; a
    // correctly ordered sequence can still put an older position on screen if
    // a newer frame carries a stale transform, and that is what the user sees
    // as the shape snapping back.
    //
    // The gesture reverses direction on purpose. Under a monotone drag a
    // regression and a stall are the same observation; under a reversing path
    // the presented displacement can be projected onto the gesture's own
    // instantaneous direction, and only a negative projection is a pop-back.
    test.setTimeout(scaledMs(120_000));
    const failures = await openEditor(page);
    const { documentRect } = await openDonnerSplash(page);
    const stem = splashLetterStem(documentRect);

    await pointerClick(page, stem);
    await expect.poll(() => selectedCount(page), {
      message: "the click must select the Splash letter before dragging it",
      timeout: scaledMs(5_000),
      intervals: [16, 25, 50, 100],
    }).toBeGreaterThan(0);
    // Let the select click's own frame finish before pressing again. A press
    // that arrives while the click is still buffered is consumed as part of it
    // and the drag never starts.
    await page.waitForTimeout(scaledMs(800));

    await installCompositedProbe(page, {
      sampleRegionCss: letterTravelRegion(stem, -120 - 110, -60 - 55),
      sampleWidth: 96,
      sampleHeight: 96,
      // The Splash artboard background is #10131e, whose channel spread is 14 -
      // just above the probe's default chromatic threshold of 12. With the
      // default the background counts as content, outnumbers the letter, and
      // pins the centroid at the window's center no matter what the letter
      // does. Raising the bar keeps only strongly chromatic pixels, which for
      // this artboard is the letter itself.
      minColorAlpha: 64,
      minColorSpread: 60,
    });
    await startCompositedProbe(page);
    const stream = await dragStream(page, stem, {
      durationMs: scaledMs(2_400),
      dx: -120,
      dy: -60,
      hz: 90,
      reversals: 4,
      reversalAmplitudePx: 110,
    });
    const result = await stopCompositedProbe(page, test.info(), stream);

    assertProbeUsable(result, kMinimumProbeSamples);
    const motion = contentMotionFraction(result.samples);
    expect(
      motion.movedSamples,
      "the reversing drag never moved the presented content, so a regression could not appear",
    ).toBeGreaterThan(2);

    const regressions = dragRegressions(result.samples, stream.trace, 1.0, 2.0, scaledMs(150));
    console.log(
      `drag-pop-back engine=${browserName} samples=${result.samples.length}`
        + ` moved=${motion.movedSamples}/${motion.comparedSamples}`
        + ` regressions=${regressions.length}`
        + ` first=${JSON.stringify(regressions.slice(0, 3))}`,
    );
    expect(
      regressions.slice(0, 5),
      `${regressions.length} sampled frames moved the presented shape AGAINST the gesture`
        + " direction (first few shown); the user sees this as the shape popping back",
    ).toEqual([]);
    expect(failures).toEqual([]);
  });

  test("k: clicking a shape shows its selection outline", async ({ browserName, page }) => {
    // GUARDS: click-select produces no visible feedback. Clicking a shape in
    // the Splash selects it - the editor's own state says so - but no outline
    // appears on screen in some engines while it does in others.
    //
    // Deliberately the crudest possible observable: a page screenshot of the
    // region around the clicked shape, before and after the click, counting
    // pixels in the selection outline's color. That is what the user sees. It
    // does not care whether the outline is painted into the document surface,
    // composited from a separate texture, or drawn by something that does not
    // exist yet, which is exactly the point - the presentation architecture
    // underneath is expected to change and this assertion must survive it.
    //
    // A screenshot is used rather than the composited read-back because the
    // read-back can only see canvases it is told about, and a per-engine
    // difference in WHICH layer carries the outline is one of the things this
    // is trying to detect. Measured at 204c60176, an in-page read-back of the
    // editor's own canvas returns fully transparent under Chromium, so a
    // read-back-based version of this test cannot distinguish "no outline"
    // from "cannot see that layer".
    test.setTimeout(scaledMs(120_000));
    const failures = await openEditor(page);
    const { documentRect } = await openDonnerSplash(page);
    const stem = splashLetterStem(documentRect);

    // The letter plus a margin for the outline and its handles. Bounded so an
    // unrelated teal pixel elsewhere in the artboard cannot satisfy the count.
    const letterRegion = {
      x: Math.max(0, stem.x - 110),
      y: Math.max(0, stem.y - 130),
      width: 230,
      height: 190,
    };

    const before = await readEditorPixelBounds(page, letterRegion, "selection-teal");
    const beforePixels = before?.pixels ?? 0;

    await page.mouse.click(stem.x, stem.y);
    await expect.poll(() => selectedCount(page), {
      message: "the click must produce a selection for its feedback to be testable",
      timeout: scaledMs(5_000),
      intervals: [16, 25, 50, 100],
    }).toBeGreaterThan(0);
    // Let the select click's own frame finish before pressing again. A press
    // that arrives while the click is still buffered is consumed as part of it
    // and the drag never starts.
    await page.waitForTimeout(scaledMs(800));

    // Poll rather than sleep: the outline may arrive with the next presented
    // frame on one engine and immediately on another, and a fixed wait would
    // encode one of those as the requirement.
    let afterPixels = 0;
    try {
      await expect
        .poll(async () => {
          afterPixels = (await readEditorPixelBounds(page, letterRegion, "selection-teal"))?.pixels
            ?? 0;
          return afterPixels - beforePixels;
        }, {
          message: "clicking a shape must make its selection outline visible",
          timeout: scaledMs(6_000),
          intervals: [100, 150, 250, 500],
        })
        .toBeGreaterThan(50);
    } finally {
      // Reported on the way out either way. The counts are the whole diagnosis
      // - an unchanged `after` is chrome that never arrived, a large `before`
      // is a window measuring artwork instead of chrome - and printing them
      // only on success is what left a red run saying nothing but its delta.
      console.log(
        `click-select-outline engine=${browserName} before=${beforePixels} after=${afterPixels}`
          + ` delta=${afterPixels - beforePixels} region=${JSON.stringify(letterRegion)}`
          + ` interaction=${JSON.stringify(await interactionStats(page))}`,
      );
    }
    expect(failures).toEqual([]);
  });
});

// Pure-function coverage for the regression classifier itself. No page, no
// browser state: these pin the measurement physics that test (g) stands on,
// after two CI flakes showed the classifier reading something other than a
// presented position. The first counted pipeline latency at a drag REVERSAL as
// an out-of-order frame (presented content finishing the old direction for one
// sample after the pointer turned around). The second counted a change in the
// matching pixel POPULATION as a change in position: the editor's selection
// position chip covers part of the dragged rectangle, so the frames that draw
// it are missing those pixels and the centroid moves while the rectangle does
// not.
test.describe("dragRegressions classifier (pure)", () => {
  /** Side of the synthetic shape these fixtures present, read-back px. */
  const kSyntheticExtentPx = 24;

  /**
   * Sample with only the fields the classifier reads.
   *
   * The masked population is a rigid square centred on the centroid, because
   * that is what a dragged shape is: its extent travels with it. Fixtures that
   * need the centroid and the extent to DISAGREE - a shape with editor chrome
   * over part of it - build their samples with `partlyHiddenSample`.
   */
  function sampleAt(t: number, centroidX: number): CompositedSample {
    return {
      t,
      drawOk: true,
      coloredCentroidX: centroidX,
      coloredCentroidY: 50,
      coloredPixels: kSyntheticExtentPx * kSyntheticExtentPx,
      coloredMinX: centroidX - (kSyntheticExtentPx - 1) / 2,
      coloredMinY: 50 - (kSyntheticExtentPx - 1) / 2,
      coloredWidth: kSyntheticExtentPx,
      coloredHeight: kSyntheticExtentPx,
    } as unknown as CompositedSample;
  }

  function classifiedCentroid(
    yellowXs: readonly number[],
    cyanXs: readonly number[],
    colorMask: "yellow-content" | null,
  ): number {
    const previousWindow = Object.getOwnPropertyDescriptor(globalThis, "window");
    const classifierWindow: {
      __donnerMatchesCompositedContentPixel?: (
        red: number,
        green: number,
        blue: number,
        alpha: number,
      ) => boolean;
    } = {};
    Object.defineProperty(globalThis, "window", { configurable: true, value: classifierWindow });
    try {
      installCompositedPixelClassifier({
        minColorAlpha: 64,
        minColorSpread: 60,
        colorMask,
      });
      const matches = classifierWindow.__donnerMatchesCompositedContentPixel!;
      const matched = [
        ...yellowXs.filter(() => matches(240, 190, 10, 255)),
        ...cyanXs.filter(() => matches(10, 190, 220, 255)),
      ];
      return matched.reduce((sum, x) => sum + x, 0) / matched.length;
    } finally {
      if (previousWindow) Object.defineProperty(globalThis, "window", previousWindow);
      else Reflect.deleteProperty(globalThis, "window");
    }
  }

  test("yellow content motion excludes the legacy cyan-overlay false pop-back", () => {
    const backgroundYellow = 9;
    const legacySamples = [
      sampleAt(0, classifiedCentroid([1, 2, 3, backgroundYellow], [10, 11, 12, 13], null)),
      sampleAt(30, classifiedCentroid([1, 2, 3, backgroundYellow], [], null)),
      sampleAt(60, classifiedCentroid([3, 4, 5, backgroundYellow], [], null)),
    ];
    const pointer = [[0, 100, 200], [30, 103, 200], [60, 106, 200], [90, 109, 200]] as const;
    expect(dragRegressions(legacySamples, pointer, 1.0, 2.0, 150).map((item) => item.sampleIndex))
      .toEqual([1]);

    const maskedSamples = [
      sampleAt(
        0,
        classifiedCentroid([1, 2, 3, backgroundYellow], [10, 11, 12, 13], "yellow-content"),
      ),
      sampleAt(30, classifiedCentroid([1, 2, 3, backgroundYellow], [], "yellow-content")),
      sampleAt(60, classifiedCentroid([3, 4, 5, backgroundYellow], [], "yellow-content")),
    ];
    expect(dragRegressions(maskedSamples, pointer, 1.0, 2.0, 150)).toEqual([]);

    maskedSamples.push(
      sampleAt(90, classifiedCentroid([1, 2, 3, backgroundYellow], [], "yellow-content")),
    );
    expect(dragRegressions(maskedSamples, pointer, 1.0, 2.0, 150).map((item) => item.sampleIndex))
      .toEqual([3]);
  });

  test("ordering ROI covers every reversal at the prior spatial resolution", () => {
    const initial = { x: 400, y: 300, width: 180, height: 120 };
    const region = orderingTrajectoryRegion(initial);
    expect(region.width / region.sampleWidth).toBeLessThanOrEqual(kOldOrderingCssPerSample.x);
    expect(region.height / region.sampleHeight).toBeLessThanOrEqual(kOldOrderingCssPerSample.y);
    for (let step = 0; step <= 4096; ++step) {
      const fraction = step / 4096;
      const swing = Math.sin(fraction * Math.PI * kOrderingDrag.reversals);
      const dx = kOrderingDrag.dx * fraction + kOrderingDrag.reversalAmplitudePx * swing;
      const dy = kOrderingDrag.dy * fraction + kOrderingDrag.reversalAmplitudePx * 0.5 * swing;
      expect(initial.x + dx).toBeGreaterThanOrEqual(region.x);
      expect(initial.y + dy).toBeGreaterThanOrEqual(region.y);
      expect(initial.x + initial.width + dx).toBeLessThanOrEqual(region.x + region.width);
      expect(initial.y + initial.height + dy).toBeLessThanOrEqual(region.y + region.height);
    }
    // At the guaranteed resolution, a real 5 CSS px backstep remains larger than the unchanged
    // one-readback-pixel ordering threshold.
    expect(5 / (region.width / region.sampleWidth)).toBeGreaterThan(1);
  });

  /** Pointer trace moving +3 css px per 10 ms until `reverseAt`, then -3. */
  function reversingTrace(
    endMs: number,
    reverseAt: number,
  ): Array<readonly [number, number, number]> {
    const trace: Array<readonly [number, number, number]> = [];
    let x = 100;
    for (let t = 0; t <= endMs; t += 10) {
      trace.push([t, x, 200] as const);
      x += t < reverseAt ? 3 : -3;
    }
    return trace;
  }

  test("a pop-back during steady one-direction motion is reported", () => {
    const trace = reversingTrace(600, Infinity);
    // Presented follows the pointer with a 60 ms lag, except sample index 6
    // re-presents a position from 5 samples earlier: a genuine older frame.
    const samples: CompositedSample[] = [];
    for (let i = 0; i < 18; ++i) {
      const t = 60 + i * 30;
      const lagged = 100 + ((t - 60) / 10) * 3;
      samples.push(sampleAt(t, i === 6 ? lagged - 45 : lagged));
    }
    const violations = dragRegressions(samples, trace);
    expect(violations.map((v) => v.sampleIndex)).toEqual([6]);
    // A real older frame carries the whole shape back, so the reported extent
    // travels with the centroid rather than staying put.
    expect(violations.map((v) => ({ presentedDx: v.presentedDx, extentDx: v.extentDx })))
      .toEqual([{ presentedDx: -36, extentDx: -36 }]);
  });

  test("evidence names the prior usable sample across an unreadable frame", () => {
    const samples = [
      sampleAt(0, 100),
      { ...sampleAt(30, 105), drawOk: false },
      sampleAt(60, 80),
    ];
    const violations = dragRegressions(samples, [[0, 100, 200], [60, 120, 200]]);
    expect(violations.map((violation) => [violation.predecessorIndex, violation.sampleIndex]))
      .toEqual([[0, 2]]);
  });

  test("one sample of lag at a pointer reversal is latency, not a violation", () => {
    // The CI flake's shape: pointer reverses at t=300; the presented centroid
    // lags 60 ms, so for two samples after the reversal it still moves in the
    // old direction while the pointer moves in the new one.
    const trace = reversingTrace(600, 300);
    const samples: CompositedSample[] = [];
    for (let i = 0; i < 18; ++i) {
      const t = 60 + i * 30;
      const laggedT = t - 60;
      const lagged = laggedT < 300
        ? 100 + (laggedT / 10) * 3
        : 100 + 90 - ((laggedT - 300) / 10) * 3;
      samples.push(sampleAt(t, lagged));
    }
    expect(dragRegressions(samples, trace)).toEqual([]);
  });

  test("a slow pipeline's lag at a reversal is excused only within the latency window", () => {
    // The CI shape: presentation lags the pointer by ~300 ms on a loaded
    // runner, so after the t=600 reversal the content keeps moving the old
    // way for several samples. A 150 ms latency window cannot reach back to
    // motion that explains it and reports a violation; the CI-scaled window
    // the specs pass covers it. Both classifications are pinned.
    const trace = reversingTrace(1200, 600);
    const samples: CompositedSample[] = [];
    for (let i = 0; i < 36; ++i) {
      const t = 300 + i * 30;
      const laggedT = t - 300;
      const lagged = laggedT < 600
        ? 100 + (laggedT / 10) * 3
        : 100 + 180 - ((laggedT - 600) / 10) * 3;
      samples.push(sampleAt(t, lagged));
    }
    expect(dragRegressions(samples, trace, 1.0, 2.0, 600).length).toBe(0);
    expect(dragRegressions(samples, trace, 1.0, 2.0, 150).length).toBeGreaterThan(0);
  });

  test("the apex-cancellation shape from CI is excused: slow pointer at a turn, content finishing the old way", () => {
    // Reconstruction of the observed CI false positives: the sample interval
    // sits well past the t=600 apex (pointer now firmly leftward), while the
    // presented content still moves right with a ~350 ms lag. A formulation
    // that reads one net-displacement window anchored before the interval
    // straddles the apex, cancels below the noise floor, and reports latency
    // as a violation. The lag sweep finds the pre-apex rightward window that
    // explains the motion.
    const trace = reversingTrace(1200, 600);
    const samples: CompositedSample[] = [];
    for (let i = 0; i < 30; ++i) {
      const t = 350 + i * 30;
      const laggedT = t - 350;
      const lagged = laggedT < 600
        ? 100 + (laggedT / 10) * 3
        : 100 + 180 - ((laggedT - 600) / 10) * 3;
      samples.push(sampleAt(t, lagged));
    }
    expect(dragRegressions(samples, trace, 1.0, 2.0, 600)).toEqual([]);
  });

  test("a stale frame that no latency in the window explains is still reported", () => {
    // Reversal at t=300, pop-back at t=930: every lag hypothesis up to 600 ms
    // lands in the post-reversal leftward phase, so a presented step back
    // toward an already-left position matches no plausible latency and must
    // be reported even though a reversal exists earlier in the drag.
    const trace = reversingTrace(1200, 300);
    const samples: CompositedSample[] = [];
    for (let i = 0; i < 30; ++i) {
      const t = 60 + i * 30;
      const laggedT = t - 60;
      const lagged = laggedT < 300
        ? 100 + (laggedT / 10) * 3
        : 100 + 90 - ((laggedT - 300) / 10) * 3;
      samples.push(sampleAt(t, Math.max(lagged, 40)));
    }
    // Sample 29 (t=930) re-presents a position 20 px to the RIGHT of the
    // lagged trajectory: a position the leftward drag left 200+ ms ago under
    // every lag hypothesis.
    samples[29] = sampleAt(samples[29].t, samples[29].coloredCentroidX + 20);
    const violations = dragRegressions(samples, trace, 1.0, 2.0, 600);
    expect(violations.map((v) => v.sampleIndex)).toEqual([29]);
  });

  /**
   * A consecutive frame pair retained from a red run of this lane on CI.
   *
   * Everything here is measured, not modelled: `trace` is the recorded pointer
   * stream around the pair, and the centroids and the bounding box come from
   * the two read-back images the run attached. In every pair the later frame's
   * masked pixels are a strict SUBSET of the earlier frame's and the two
   * bounding boxes are identical - the shape did not move at all. What changed
   * is the editor's own chrome: the selection position chip clamps to the
   * artboard's top-left corner once a drag carries the selection off the
   * artboard, so it lands on the dragged rectangle, and the frames where it is
   * drawn are missing exactly the pixels it covers. Reading that as a position
   * is what reported an older frame in a document that never moved.
   */
  interface MeasuredFrame {
    t: number;
    centroidX: number;
    centroidY: number;
    pixels: number;
  }

  interface PartlyHiddenFramePair {
    name: string;
    trace: Array<readonly [number, number, number]>;
    boundingBox: { minX: number; minY: number; width: number; height: number };
    earlier: MeasuredFrame;
    later: MeasuredFrame;
  }

  function partlyHiddenSample(
    boundingBox: PartlyHiddenFramePair["boundingBox"],
    frame: MeasuredFrame,
  ): CompositedSample {
    return {
      t: frame.t,
      drawOk: true,
      coloredPixels: frame.pixels,
      coloredCentroidX: frame.centroidX,
      coloredCentroidY: frame.centroidY,
      coloredMinX: boundingBox.minX,
      coloredMinY: boundingBox.minY,
      coloredWidth: boundingBox.width,
      coloredHeight: boundingBox.height,
    } as unknown as CompositedSample;
  }

  const kPartlyHiddenFramePairs: PartlyHiddenFramePair[] = [
    {
      name: "chip drawn over the rectangle's top-left corner",
      trace: [
        [36602.42, 297.875567, 298.438351],
        [36654.60, 297.118639, 297.331977],
        [36699.98, 295.322267, 296.217313],
        [36736.82, 293.205649, 296.346521],
        [36766.36, 292.199876, 294.818285],
        [36807.70, 291.217578, 294.247043],
        [36835.56, 289.108474, 294.050686],
        [36883.86, 287.256958, 292.390915],
      ],
      boundingBox: { minX: 43, minY: 22, width: 25, height: 34 },
      earlier: { t: 36864.36, centroidX: 54.944444, centroidY: 38.425532, pixels: 846 },
      later: { t: 36883.96, centroidX: 55.533069, centroidY: 40.083333, pixels: 756 },
    },
    {
      name: "chip drawn while the rectangle still spans the artboard corner",
      trace: [
        [27330.56, 304.530555, 300.995808],
        [27389.02, 303.733552, 300.494740],
        [27422.30, 301.822677, 300.408923],
        [27467.40, 300.167322, 298.850920],
        [27513.08, 299.584132, 298.857245],
        [27561.64, 298.493788, 298.269401],
        [27599.26, 296.487127, 296.927561],
      ],
      boundingBox: { minX: 40, minY: 20, width: 31, height: 38 },
      earlier: { t: 27562.00, centroidX: 54.926621, centroidY: 38.408703, pixels: 1172 },
      later: { t: 27599.46, centroidX: 55.772595, centroidY: 40.414966, pixels: 1029 },
    },
    {
      name: "chip drawn later in the same drag, one sample apart",
      trace: [
        [43508.84, 293.205649, 296.346521],
        [43540.54, 292.199876, 294.818285],
        [43571.54, 291.217578, 294.247043],
        [43634.66, 289.108474, 294.050686],
        [43673.98, 287.256958, 292.390915],
        [43713.02, 286.457179, 292.299920],
        [43744.12, 285.120714, 291.575901],
        [43783.36, 282.863346, 290.121135],
      ],
      boundingBox: { minX: 42, minY: 21, width: 25, height: 34 },
      earlier: { t: 43744.42, centroidX: 53.972813, centroidY: 37.462175, pixels: 846 },
      later: { t: 43783.50, centroidX: 54.595628, centroidY: 39.435792, pixels: 732 },
    },
  ];

  test("editor chrome hiding part of the shape is not an older frame", () => {
    const verdicts = Object.fromEntries(kPartlyHiddenFramePairs.map((pair) => [
      pair.name,
      dragRegressions(
        [
          partlyHiddenSample(pair.boundingBox, pair.earlier),
          partlyHiddenSample(pair.boundingBox, pair.later),
        ],
        pair.trace,
        1.0,
        2.0,
        150,
      ),
    ]));
    expect(
      verdicts,
      "each pair's two frames share one bounding box, so the shape held its position"
        + " and the centroid shift is the hidden pixels, not a presented position",
    ).toEqual(Object.fromEntries(kPartlyHiddenFramePairs.map((pair) => [pair.name, []])));
  });

  test("a shape that really moved back is still reported", () => {
    // The same centroid shift as the pairs above, with the bounding box carried
    // back by it and rounded the way the probe reports one: that is a shape at
    // an earlier position, and the extent evidence must not excuse it.
    const pair = kPartlyHiddenFramePairs[0];
    const shiftX = pair.later.centroidX - pair.earlier.centroidX;
    const shiftY = pair.later.centroidY - pair.earlier.centroidY;
    const samples = [
      partlyHiddenSample(pair.boundingBox, pair.earlier),
      partlyHiddenSample({
        ...pair.boundingBox,
        minX: Math.round(pair.boundingBox.minX + shiftX),
        minY: Math.round(pair.boundingBox.minY + shiftY),
      }, pair.later),
    ];
    const violations = dragRegressions(samples, pair.trace, 1.0, 2.0, 150);
    expect(violations.map((v) => ({
      sampleIndex: v.sampleIndex,
      extentDx: v.extentDx,
      extentDy: v.extentDy,
    }))).toEqual([{ sampleIndex: 1, extentDx: 1, extentDy: 2 }]);
  });

  test("the quantised box keeps its verdict once a pop-back projects past 1.4 px", () => {
    // The box is reported in whole read-back pixels, so rounding each edge can
    // move its projection by up to half a pixel per axis and a pop-back barely
    // past the 1.0 tolerance can round to no movement at all. Sweeping a rigid
    // 25x34 shape over 625 sub-pixel phases and every direction in 3 degree
    // steps puts that loss entirely below a projection of 1.4 read-back px,
    // which is under every pair above. This pins the floor so a later change to
    // the box or the tolerance cannot quietly raise it.
    const pair = kPartlyHiddenFramePairs[0];
    const pointerDx = pair.trace[pair.trace.length - 1][1]
      - pair.trace[pair.trace.length - 2][1];
    const pointerDy = pair.trace[pair.trace.length - 1][2]
      - pair.trace[pair.trace.length - 2][2];
    const pointerLength = Math.hypot(pointerDx, pointerDy);
    const missedProjections: number[] = [];
    for (let magnitude = 1.0; magnitude <= 6.0; magnitude += 0.1) {
      for (let degrees = 0; degrees < 360; degrees += 3) {
        const radians = (degrees * Math.PI) / 180;
        const dx = Math.cos(radians) * magnitude;
        const dy = Math.sin(radians) * magnitude;
        const projection = (dx * pointerDx + dy * pointerDy) / pointerLength;
        if (projection >= -1.4) continue;
        for (let phase = 0; phase < 625; ++phase) {
          // The shape sits at a sub-pixel offset the read-back cannot see; only
          // its rounded box reaches the classifier, on both sides of the pair.
          const offsetX = (phase % 25) / 25;
          const offsetY = Math.floor(phase / 25) / 25;
          const before = {
            ...pair.boundingBox,
            minX: Math.round(pair.boundingBox.minX + offsetX),
            minY: Math.round(pair.boundingBox.minY + offsetY),
          };
          const after = {
            ...pair.boundingBox,
            minX: Math.round(pair.boundingBox.minX + offsetX + dx),
            minY: Math.round(pair.boundingBox.minY + offsetY + dy),
          };
          const moved = dragRegressions(
            [
              partlyHiddenSample(before, pair.earlier),
              partlyHiddenSample(after, {
                ...pair.later,
                centroidX: pair.earlier.centroidX + dx,
                centroidY: pair.earlier.centroidY + dy,
              }),
            ],
            pair.trace,
            1.0,
            2.0,
            150,
          );
          if (moved.length === 0) missedProjections.push(projection);
        }
      }
    }
    expect(
      missedProjections.slice(0, 5),
      `${missedProjections.length} rigid pop-backs projecting past 1.4 read-back px`
        + " were excused by the extent evidence; the quantised box lost a verdict the"
        + " centroid tolerance had already accepted",
    ).toEqual([]);
  });
});

test("composited readback capture keeps exact slots and bounds retained memory", async () => {
  const pending: Array<() => void> = [];
  let drawable = true;
  let encodingAllowed = false;
  const encoded: Array<{ data: Uint8ClampedArray; width: number; height: number }> = [];
  const png = Buffer.from(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVQIHWP4z8DwHwAFgAI/ScLbtAAAAABJRU5ErkJggg==",
    "base64",
  );
  const pixels = new Uint8ClampedArray([255, 0, 0, 255]);
  const context = {
    clearRect() {},
    drawImage() {},
    getImageData: () => ({ data: pixels }),
    putImageData(image: { data: Uint8ClampedArray; width: number; height: number }) {
      expect(encodingAllowed).toBe(true);
      encoded.push({ ...image, data: image.data.slice() });
    },
  };
  const readback = {
    width: 1,
    height: 1,
    getContext: () => context,
    toDataURL() {
      if (!encodingAllowed) throw new Error("PNG encoding must wait until sampling stops");
      expect(encoded.length).toBeGreaterThan(0);
      return `data:image/png;base64,${png.toString("base64")}`;
    },
  };
  const surface = {
    width: 1,
    height: 1,
    getBoundingClientRect: () => ({ left: 0, top: 0, width: 1, height: 1 }),
  };
  const globals = {
    window: {},
    ImageData: class {
      data: Uint8ClampedArray;
      width: number;
      height: number;
      constructor(data: Uint8ClampedArray, width: number, height: number) {
        this.data = data;
        this.width = width;
        this.height = height;
      }
    },
    document: { createElement: () => readback, querySelector: () => drawable ? surface : null },
    requestAnimationFrame: (callback: () => void) => pending.push(callback),
  };
  const previous = new Map(
    Object.keys(globals).map((name) =>
      [name, Object.getOwnPropertyDescriptor(globalThis, name)] as const
    ),
  );
  for (const [name, value] of Object.entries(globals)) {
    Object.defineProperty(globalThis, name, { configurable: true, value });
  }
  try {
    const fakePage = {
      evaluate: async (callback: (argument: unknown) => unknown, argument: unknown) =>
        callback(argument),
    };
    await installCompositedProbe(fakePage as unknown as Page, {
      sampleWidth: 1,
      sampleHeight: 1,
      captureReadbacks: true,
    });
    await startCompositedProbe(fakePage as unknown as Page);
    const probe = (globals.window as unknown as {
      __donnerCompositedProbe: {
        samples: CompositedSample[];
        rawReadbacks: Array<Uint8ClampedArray | null>;
        rawReadbackBytes: number;
        rawReadbackOverflow: boolean;
        stop(): void;
      };
    }).__donnerCompositedProbe;
    pending.shift()!();
    expect(probe.rawReadbacks).toEqual([new Uint8ClampedArray([255, 0, 0, 255])]);
    pixels[0] = 0;
    pixels[1] = 255;
    pending.shift()!();
    expect(probe.rawReadbacks).toEqual([
      new Uint8ClampedArray([255, 0, 0, 255]),
      new Uint8ClampedArray([0, 255, 0, 255]),
    ]);
    expect(probe.rawReadbackBytes).toBe(8);
    await expect(attachCompositedReadbacks(fakePage, test.info(), [0])).rejects.toThrow(/Stop/);
    drawable = false;
    pending.shift()!();
    expect(probe.rawReadbacks[2]).toBeNull();
    expect(probe.rawReadbackOverflow).toBe(false);
    drawable = true;
    probe.rawReadbackBytes = 64 * 1024 * 1024 - 3;
    pending.shift()!();
    expect(probe.rawReadbacks[3]).toBeNull();
    expect(probe.rawReadbackBytes).toBe(64 * 1024 * 1024 - 3);
    expect(probe.rawReadbackOverflow).toBe(true);
    expect(probe.rawReadbacks.length).toBe(probe.samples.length);
    probe.stop();
    await expect(attachCompositedReadbacks(fakePage, test.info(), [0])).rejects.toThrow(
      /memory budget/,
    );
    probe.rawReadbackOverflow = false;
    await expect(attachCompositedReadbacks(fakePage, test.info(), [2])).rejects.toThrow(/Missing/);
    const retainedFirstFrame = probe.rawReadbacks[0];
    probe.rawReadbacks[0] = new Uint8ClampedArray([1, 2]);
    await expect(attachCompositedReadbacks(fakePage, test.info(), [0])).rejects.toThrow(
      /dimensions/,
    );
    probe.rawReadbacks[0] = retainedFirstFrame;
    encodingAllowed = true;
    const evidence = await attachCompositedReadbacks(fakePage, test.info(), [0, 1]);
    expect(evidence.indices).toEqual([0, 1]);
    expect(evidence.bytes).toBe(png.length * 2);
    expect(encoded).toEqual([
      { data: new Uint8ClampedArray([255, 0, 0, 255]), width: 1, height: 1 },
      { data: new Uint8ClampedArray([0, 255, 0, 255]), width: 1, height: 1 },
    ]);
    for (const index of evidence.indices) {
      expect(readFileSync(test.info().outputPath(`composited-frame-${index}.png`))).toEqual(png);
    }
  } finally {
    for (const [name, descriptor] of previous) {
      if (descriptor) Object.defineProperty(globalThis, name, descriptor);
      else Reflect.deleteProperty(globalThis, name);
    }
  }
});
