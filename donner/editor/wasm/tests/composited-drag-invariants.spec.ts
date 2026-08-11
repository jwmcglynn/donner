import { expect, type Page, test } from "@playwright/test";
import { readEditorPixelBounds } from "./canvas-color-stats";
import {
  blackFrameStats,
  type CompositedProbeResult,
  contentMotionFraction,
  dragRegressions,
  installCompositedProbe,
  startCompositedProbe,
  stopCompositedProbe,
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

/**
 * The visible region of the presented document, in CSS px.
 *
 * The surface element spans its cap-sized backing store, so its raw element
 * box is much larger than the document; the clip-path insets are what bound
 * the pixels the user can see.
 */
async function visibleDocumentRect(page: Page): Promise<Rect> {
  const rect = await page.evaluate(() => {
    const el = document.querySelector<HTMLCanvasElement>(
      "canvas[data-direct-surface-visible=\"true\"]",
    );
    if (el === null) {
      return null;
    }
    const box = el.getBoundingClientRect();
    const match = (getComputedStyle(el).clipPath || "").match(/inset\(([^)]*)\)/);
    if (match === null) {
      return { x: box.left, y: box.top, width: box.width, height: box.height };
    }
    const parts = match[1].trim().split(/\s+/).map((part) => parseFloat(part));
    if (parts.length === 0 || parts.some((value) => !Number.isFinite(value))) {
      return { x: box.left, y: box.top, width: box.width, height: box.height };
    }
    const top = parts[0];
    const right = parts.length >= 2 ? parts[1] : parts[0];
    const bottom = parts.length >= 3 ? parts[2] : parts[0];
    const left = parts.length >= 4 ? parts[3] : right;
    return {
      x: box.left + left,
      y: box.top + top,
      width: box.width - left - right,
      height: box.height - top - bottom,
    };
  });
  expect(rect, "no visible document surface").not.toBeNull();
  if (rect === null) {
    throw new Error("no visible document surface");
  }
  return rect;
}

/** Open the Donner Splash from the carousel and wait for its first frame. */
async function openDonnerSplash(page: Page): Promise<{ editorBounds: Rect; documentRect: Rect }> {
  const editorCanvas = page.locator("canvas#canvas");
  const editorBounds = await editorCanvas.boundingBox();
  expect(editorBounds, "the editor canvas is missing").not.toBeNull();
  if (editorBounds === null) {
    throw new Error("editor canvas is missing");
  }
  await page.mouse.click(editorBounds.x + editorBounds.width * 0.24, editorBounds.y + 282);
  await expect(editorCanvas).toHaveAttribute("data-active-sample-id", "donner-splash");
  await expect(page.locator("canvas[data-direct-surface-visible=\"true\"]")).toBeVisible();
  await expect
    .poll(
      () =>
        page.evaluate(() =>
          (window as unknown as { __donnerAcceptedPresentation?: { token?: number } })
            .__donnerAcceptedPresentation?.token || 0
        ),
      {
        message: "Donner Splash must publish a presented document frame",
        timeout: scaledMs(5_000),
        intervals: [16, 25, 50, 100],
      },
    )
    .toBeGreaterThan(0);
  // The debounced canvas-size commit lands after the first frame; sampling
  // before it settles measures the load, not the gesture.
  await page.waitForTimeout(scaledMs(1_500));
  return { editorBounds, documentRect: await visibleDocumentRect(page) };
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
  test("g: a shape drag never presents a frame older than one already shown", async ({ browserName, page }) => {
    // GUARDS: the drag pop-back. While dragging a shape in a stock browser the
    // object intermittently snaps to a position it already left. The
    // presentation-ordering half of that is a presented frame label moving
    // backward, which is what this asserts; the pixel half is (j).
    //
    // Measured at 204c60176 on stock Chrome and stock Firefox over a 2-3 s
    // reversing drag: zero backward labels in every run. Enforced from now on
    // so a future pipeline cannot reintroduce out-of-order presentation.
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

    await installCompositedProbe(page);
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

    const violations: Array<{ index: number; from: number; to: number }> = [];
    let lastToken = 0;
    for (const [index, sample] of result.samples.entries()) {
      if (sample.frameToken !== 0 && sample.frameToken < lastToken) {
        violations.push({ index, from: lastToken, to: sample.frameToken });
      }
      if (sample.frameToken !== 0) {
        lastToken = sample.frameToken;
      }
    }
    const presentedFrames = new Set(
      result.samples.map((sample) => sample.frameToken).filter((token) => token !== 0),
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
    // presented document region that reads back empty, and a frame in which no
    // document surface was on screen at all. The second is the one a pixel-only
    // check misses, because with nothing presented there is nothing to read
    // back and the sample is simply skipped.
    //
    // Measured at 204c60176 on stock Chrome and stock Firefox: zero of either,
    // across the click, the press, and the first 400 ms of motion. Enforced at
    // that observation. NOTE for whoever sees this go red: a flash produced
    // purely by compositor-level timing - the element hidden for one browser
    // frame while both canvases are momentarily unpresentable - is visible to
    // the surface-absence half of this test but NOT to the read-back half,
    // which samples canvas contents rather than the composited page.
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
      .filter(({ sample }) => sample.surfaceId === "");
    console.log(
      `drag-black-frames engine=${browserName} samples=${result.samples.length}`
        + ` black=${stats.blackSamples} fraction=${stats.fraction.toFixed(4)}`
        + ` longestRun=${stats.longestRun} noSurface=${blankSurfaceSamples.length}`
        + ` pointerEvents=${stream.pointerEvents}`,
    );
    expect(
      blankSurfaceSamples.map(({ index }) => index).slice(0, 5),
      `${blankSurfaceSamples.length} sampled frames had NO document surface on screen during a`
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

  // FIXME(design-0064): RED at 204c60176 and left red on purpose.
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
  // that Design 0064 deletes rather than repairs, and the operator's direction
  // is not to patch it in isolation. This test is written against the correct
  // behavior and flips to enforced when the 0064 phases land. If it goes GREEN
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

    const regressions = dragRegressions(result.samples, stream.trace);
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

    const baked = await page.evaluate(() =>
      (window as unknown as {
        __donnerAcceptedPresentation?: { selectionChromeBaked?: boolean };
      }).__donnerAcceptedPresentation?.selectionChromeBaked ?? null
    );
    console.log(
      `click-select-outline engine=${browserName} before=${beforePixels} after=${afterPixels}`
        + ` delta=${afterPixels - beforePixels} selectionChromeBaked=${baked}`,
    );
    expect(failures).toEqual([]);
  });
});
