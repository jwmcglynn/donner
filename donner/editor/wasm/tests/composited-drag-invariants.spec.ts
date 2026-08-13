import { expect, type Page, test } from "@playwright/test";
import { readEditorPixelBounds } from "./canvas-color-stats";
import {
  blackFrameStats,
  type CompositedProbeResult,
  type CompositedSample,
  contentMotionFraction,
  dragRegressions,
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
  await expect
    .poll(
      () =>
        page.evaluate(() =>
          (window as unknown as { __donnerCanStartWasm?: boolean }).__donnerCanStartWasm
        ),
      { timeout: scaledMs(30_000) },
    )
    .toBe(true);
  const hasWebGpu = await page.evaluate(() => "gpu" in navigator);
  test.skip(!hasWebGpu, "Browser does not expose navigator.gpu");
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
  await page.mouse.click(editorBounds.x + editorBounds.width * 0.24, editorBounds.y + 282);
  await expect(editorCanvas).toHaveAttribute("data-active-sample-id", "donner-splash");
  await expect(editorCanvas).toBeVisible();
  await expect
    .poll(
      () =>
        page.evaluate(() =>
          (window as unknown as { __donnerLayerThumbnailStats?: { rowCount?: number } })
            .__donnerLayerThumbnailStats?.rowCount ?? 0
        ),
      {
        message: "Donner Splash must become the editor's live document",
        timeout: scaledMs(20_000),
        intervals: [16, 25, 50, 100],
      },
    )
    .toBeGreaterThan(0);
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
    // GUARDS: the drag pop-back. While dragging a shape in a stock browser the
    // object intermittently snaps to a position it already left.
    //
    // Measured at 204c60176 on stock Chrome and stock Firefox over a 2-3 s
    // reversing drag: zero out-of-order presentations in every run. Enforced
    // from now on so a future pipeline cannot reintroduce it.
    //
    // This used to assert on a presented frame LABEL moving backward, with the
    // pixel half deferred to (j). The single-canvas replacement deleted the label with the epoch
    // acceptance flow, and the pixel measurement absorbs it: content moving
    // against the pointer is the same defect, observed one step closer to the
    // user, and a monotone label never ruled it out anyway.
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

    // Sample a window around the dragged letter, with (j)'s thresholds and for
    // (j)'s reason: a centroid over the whole canvas is dominated by everything
    // that is not moving. The letter travels 140 CSS px out of a 1600 px canvas
    // sampled into 64 read-back columns, which is two columns of motion against
    // an editor's worth of stationary chrome, so the presented position
    // quantized to whole read-back pixels barely changes and the ordering check
    // has almost nothing to order. Measured across the same working drag: 15
    // distinct presented positions out of 537 samples over the whole canvas,
    // 68 with this window.
    await installCompositedProbe(page, {
      sampleRegionCss: letterTravelRegion(stem, -140, -70),
      sampleWidth: 96,
      sampleHeight: 96,
      // The Splash artboard background is #10131e, whose channel spread is 14 -
      // just above the probe's default chromatic threshold of 12. With the
      // default the background counts as content, outnumbers the letter, and
      // pins the centroid at the window's center no matter what the letter
      // does. Raising the bar keeps only strongly chromatic pixels, which on
      // this artboard is the letter itself.
      minColorAlpha: 64,
      minColorSpread: 60,
    });
    await startCompositedProbe(page);
    const stream = await dragStream(page, stem, {
      durationMs: scaledMs(1_800),
      dx: -140,
      dy: -70,
      hz: 90,
      reversals: 3,
      reversalAmplitudePx: 90,
    });
    await page.waitForTimeout(scaledMs(400));
    const result = await stopCompositedProbe(page);

    assertProbeUsable(result, kMinimumProbeSamples);

    // The single-canvas replacement removed the presented-epoch token, so ordering is no longer
    // observable as a counter on the page. What "older frame after a newer one"
    // means to the user is unchanged, and it is directly measurable: the
    // presented content moving against the pointer. `dragRegressions` is that
    // measurement, and it is strictly stronger than the counter was, because a
    // monotone counter could still carry stale pixels.
    // The reversal-latency excuse window must scale with the same CI factor as
    // every timeout in this suite: on a loaded CI runner the presentation lags
    // the pointer by several hundred ms, so a 150 ms horizon misses reversals
    // the presented content is legitimately still finishing (observed live:
    // a violation whose presented delta tracked the pre-reversal direction
    // with the turn ~2 samples beyond the unscaled horizon).
    const violations = dragRegressions(result.samples, stream.trace, 1.0, 2.0, scaledMs(150));
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
    // Gecko-at-CI carve-out, same as composited-invariants a/b/d: Playwright
    // Firefox on shared CI runners shows a drawImage-readback artifact against
    // worker-owned WebGPU canvases (long runs of empty samples while DOM
    // observables prove the document is presenting). The readback-dependent
    // assertions in this test are meaningless there; the DOM-based drag
    // invariants (g, and h's surface-absence half via the enforced suites)
    // keep Gecko coverage. Tracked: Gecko readback diagnosis in the Design
    // 0062 follow-ups; remove when it lands.
    test.skip(
      browserName === "firefox" && Boolean(process.env.CI),
      "Gecko CI readback artifact - see tracked diagnosis",
    );
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
    const result = await stopCompositedProbe(page);

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
    // Gecko-at-CI carve-out, same as composited-invariants a/b/d: Playwright
    // Firefox on shared CI runners shows a drawImage-readback artifact against
    // worker-owned WebGPU canvases (long runs of empty samples while DOM
    // observables prove the document is presenting). The readback-dependent
    // assertions in this test are meaningless there; the DOM-based drag
    // invariants (g, and h's surface-absence half via the enforced suites)
    // keep Gecko coverage. Tracked: Gecko readback diagnosis in the Design
    // 0062 follow-ups; remove when it lands.
    test.skip(
      browserName === "firefox" && Boolean(process.env.CI),
      "Gecko CI readback artifact - see tracked diagnosis",
    );
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
    const result = await stopCompositedProbe(page);

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
    const result = await stopCompositedProbe(page);

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
// after a CI flake showed the classifier counting pipeline latency at a drag
// REVERSAL as an out-of-order frame (presented content finishing the old
// direction for one sample after the pointer turned around).
test.describe("dragRegressions classifier (pure)", () => {
  /** Sample with only the fields the classifier reads. */
  function sampleAt(t: number, centroidX: number): CompositedSample {
    return {
      t,
      drawOk: true,
      coloredCentroidX: centroidX,
      coloredCentroidY: 50,
    } as unknown as CompositedSample;
  }

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
      const lagged = laggedT < 300 ? 100 + (laggedT / 10) * 3 : 100 + 90 - ((laggedT - 300) / 10) * 3;
      samples.push(sampleAt(t, lagged));
    }
    expect(dragRegressions(samples, trace)).toEqual([]);
  });

  test("a slow pipeline's reversal lag is excused only under a widened horizon", () => {
    // The CI shape: presentation lags the pointer by ~300 ms on a loaded
    // runner, so after the t=600 reversal the content keeps moving the old
    // way for several samples. A 150 ms horizon cannot see the turn and
    // reports latency as a violation; the CI-scaled horizon the specs pass
    // covers it. Both classifications are pinned.
    const trace = reversingTrace(1200, 600);
    const samples: CompositedSample[] = [];
    for (let i = 0; i < 36; ++i) {
      const t = 300 + i * 30;
      const laggedT = t - 300;
      const lagged = laggedT < 600 ? 100 + (laggedT / 10) * 3 : 100 + 180 - ((laggedT - 600) / 10) * 3;
      samples.push(sampleAt(t, lagged));
    }
    expect(dragRegressions(samples, trace, 1.0, 2.0, 600).length).toBe(0);
    expect(dragRegressions(samples, trace, 1.0, 2.0, 150).length).toBeGreaterThan(0);
  });

  test("the reversal excuse expires with the horizon: a later stale frame is still reported", () => {
    // Same reversal at t=300, but the stale frame arrives at t=480, long after
    // the pointer's turn has left the excuse's 150 ms horizon. By then the
    // pointer direction before and inside the interval agree (both leftward),
    // so a presented step back toward an already-left position must be
    // reported even though a reversal exists earlier in the drag.
    const trace = reversingTrace(600, 300);
    const samples: CompositedSample[] = [];
    for (let i = 0; i < 18; ++i) {
      const t = 60 + i * 30;
      const laggedT = t - 60;
      const lagged = laggedT < 300 ? 100 + (laggedT / 10) * 3 : 100 + 90 - ((laggedT - 300) / 10) * 3;
      samples.push(sampleAt(t, lagged));
    }
    // Sample 14 (t=480) re-presents a position 20 px to the RIGHT of the
    // lagged trajectory: a position the leftward drag already left.
    samples[14] = sampleAt(samples[14].t, samples[14].coloredCentroidX + 20);
    const violations = dragRegressions(samples, trace);
    expect(violations.map((v) => v.sampleIndex)).toEqual([14]);
  });
});
