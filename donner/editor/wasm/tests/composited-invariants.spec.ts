import { expect, type Page, test } from "@playwright/test";
import { burstZoomStorm, panStream, pinchStream } from "./gesture-streams";
import {
  backingSizeTransitions,
  blackFrameStats,
  type CompositedProbeResult,
  contentMotionFraction,
  installCompositedProbe,
  readDocumentArtWidth,
  readViewportStats,
  startCompositedProbe,
  stopCompositedProbe,
  type ViewportStats,
  visibleDocumentRegion,
} from "./composited-probe";

/**
 * Composited-output invariants for the editor's single-canvas presentation.
 *
 * WHAT THIS SUITE IS FOR
 *
 * A week of presentation regressions shipped past a fully green board. Every
 * one of them was a property of the frames DURING a gesture, and every suite
 * we had asserted on the state AFTER one. This suite closes that gap: it runs
 * realistic gesture streams (see `gesture-streams.ts` for why the shapes
 * matter) while sampling the composited result every animation frame (see
 * `composited-probe.ts` for what a sample contains), and asserts invariants
 * that a flicker violates and a settled check cannot see.
 *
 * Each test below names the escaped regression it guards. If one of these ever
 * goes red, read that comment first: the assertion is a proxy, the comment is
 * the defect.
 *
 * SINGLE-CANVAS OBSERVABLES (single-canvas presenter architecture)
 *
 * The seam these invariants were written against is gone: one canvas, one
 * WebGPU frame, no CSS between document space and the screen. Two of the
 * original six invariants died with it, because their entire subject was the
 * seam:
 *
 *  - "presented pixels and their CSS geometry change atomically" (b): there is
 *    no CSS geometry to disagree with the pixels. Pixels and transform are
 *    produced by the same pass.
 *  - "consecutive epochs present on alternating DOM canvases" (f): there is
 *    one canvas, and no acceptance step for a second one to alternate against.
 *
 * The four that survive moved from ELEMENT observables to CONTENT observables:
 * the canvas fills the window and never moves or resizes, so motion and scale
 * are read from where the document's own chromatic pixels are, not from an
 * element box or a clip inset.
 */

// Shared CI runners execute this suite several times slower than local
// development hardware. Scale acceptance polls so the invariants are verified
// at the same strength without flaking on runner speed.
const kCiTimeScale = process.env.CI ? 4 : 1;
const scaledMs = (ms: number) => ms * kCiTimeScale;

const kBaseUrl = process.env.DONNER_WASM_BASE_URL || "http://127.0.0.1:8000";

test.use({ viewport: { width: 1600, height: 900 } });

/**
 * Boot the editor and start collecting fatal console output.
 *
 * Reimplemented here rather than imported from the presentation suite on
 * purpose: this file must stay landable while that one is being edited, and a
 * coverage lane that cannot run because an unrelated spec is mid-refactor is
 * not coverage.
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
    .poll(() =>
      page.evaluate(() =>
        (window as unknown as { __donnerCanStartWasm?: boolean }).__donnerCanStartWasm
      ), { timeout: scaledMs(30_000) })
    .toBe(true);
  // Playwright's bundled WebKit ships no WebGPU, so the Geode-only package
  // cannot boot there; real-Safari validation covers that engine.
  const hasWebGpu = await page.evaluate(() => "gpu" in navigator);
  test.skip(!hasWebGpu, "Browser does not expose navigator.gpu");
  await expect(page.locator("#status")).toBeHidden({ timeout: scaledMs(20_000) });
  return failures;
}

/**
 * Open the Donner Splash from the carousel and wait until it is really open.
 *
 * "Really open" is three separate facts and every one of them has bitten:
 *
 *  - `data-active-sample-id` only says the carousel accepted the click.
 *  - The document model has to be live. Until it is, the sample carousel is
 *    still the modal on top and the render pane ignores every gesture: a pinch
 *    dispatched here reaches the app, is correctly discarded as modal-captured
 *    input, and the test then measures a load in progress. The layers panel
 *    row count is the page-visible form of "the editor has a document" (it
 *    reads "(no document)" until then).
 *  - The document's pixels have to be on screen, which is what the invariants
 *    below actually sample.
 *
 * The old version polled a whole-canvas chromatic extent, which the editor's
 * own chrome satisfies before the document exists, so all three checks passed
 * vacuously and every gesture below raced the load.
 */
async function openDonnerSplash(page: Page): Promise<{
  editorBounds: { x: number; y: number; width: number; height: number };
  viewport: ViewportStats;
}> {
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
  return { editorBounds, viewport };
}

/**
 * A square window of `sizeCss` CSS px centred on the presented document.
 *
 * Clamped into the pane so the window never includes the panels around it, and
 * sized so a gesture of a few hundred CSS px keeps document content inside it
 * for the whole sampled run.
 */
function documentCenterWindow(stats: ViewportStats, sizeCss: number): {
  x: number;
  y: number;
  width: number;
  height: number;
} {
  const region = visibleDocumentRegion(stats);
  const width = Math.min(sizeCss, region.width);
  const height = Math.min(sizeCss, region.height);
  return {
    x: region.x + (region.width - width) / 2,
    y: region.y + (region.height - height) / 2,
    width,
    height,
  };
}

/**
 * Extent of the presented document's art, in read-back pixels of the canvas.
 *
 * The geometry is re-read on every call rather than captured once, because a
 * zoom moves the document inside the pane: the scan window has to follow it or
 * a gesture that pushes the art past the old window's edge measures as a
 * smaller one. Only the SCAN moves; the read-back stays in canvas pixels, so
 * the ratio of two calls is the scale the gesture applied.
 */
async function splashArtWidth(page: Page): Promise<number> {
  return readDocumentArtWidth(page, visibleDocumentRegion(await readSettledViewportStats(page)));
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
 * The render pane classifies wheel input only while it is the hovered window,
 * so every stream has to be preceded by a real pointer park over the pane.
 */
async function parkPointerOverPane(
  page: Page,
  editorBounds: { x: number; y: number },
): Promise<{ x: number; y: number }> {
  const point = { x: editorBounds.x + 480, y: editorBounds.y + 320 };
  await page.mouse.move(point.x, point.y);
  return point;
}

/**
 * Fail loudly when the composited read-back path itself did not work.
 *
 * Without this, a browser that refuses to draw the transferred canvas into a
 * 2d context reports a long, calm run of transparent frames, and every
 * invariant below passes for the wrong reason. A coverage lane that can only
 * pass is worse than no lane.
 */
// CI runners pay ~10x our reference machine's cost per probe sample (the
// drawImage readback of the worker canvas dominates), so sampling there is
// cost-bound rather than window-bound: longer scaled gesture windows do not
// buy proportionally more samples. The floor drops on CI; violation detection
// is per-transition, and a dozen samples across a full gesture still spans
// every epoch class the invariants examine.
const kMinimumProbeSamples = process.env.CI ? 12 : 20;

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

/**
 * Distinct presented document widths, proof that a zoom stream moved the scale.
 *
 * This is a precondition, so it reads the editor's presented geometry rather
 * than pixels; the invariant it guards stays on the pixels. Pixels cannot
 * answer it here for two independent reasons: the storm is symmetric and ends
 * at the scale it started from, so a before/after pixel comparison sees
 * nothing, and the whole-canvas chromatic extent this used to read is the
 * EDITOR's extent, which a zoom never changes at all - it reported the storm
 * as scale-moving only while the editor was still painting itself in.
 * Bucketed to whole CSS pixels so publishing noise cannot manufacture values.
 */
function distinctPresentedDocumentWidths(result: CompositedProbeResult): number {
  return new Set(
    result.samples.filter((sample) => sample.presentedDocumentWidth > 0).map((sample) =>
      Math.round(sample.presentedDocumentWidth)
    ),
  ).size;
}

test.describe("composited output invariants", () => {
  test("a: a zoom storm never shows an empty visible region", async ({ browserName, page }) => {
    // Gecko carve-out (#927): on the single-canvas build the app presents
    // every UI frame, and Gecko cannot sample that. The page rAF starves to a
    // handful of probe samples per gesture window (each drawImage of the
    // continuously-presented WebGPU canvas appears to synchronize against the
    // presenter), and the samples that land intermittently read empty in runs
    // the same-task retry cannot rescue. Chromium samples the same build
    // cleanly and enforces every pixel invariant; manual Firefox validation
    // shows the document presenting normally. Closing #927 means diagnosing
    // the Gecko readback behavior, not adding another proxy observable.
    test.skip(
      browserName === "firefox",
      "Gecko cannot sample a continuously-presented WebGPU canvas (#927)",
    );
    // GUARDS: the backing-store-clear flicker. A per-epoch document-canvas
    // resize cleared the backing store while that canvas was still visible, so
    // ~18% of frames under a burst storm composited a fully transparent
    // document region. Geometry stayed perfect throughout, which is why every
    // geometry-based assertion stayed green; only the pixels moved. the single-canvas architecture
    // removed the per-epoch resize (the canvas is the window), so this now
    // guards the class rather than that instance: any storm frame that reaches
    // the compositor with no document in it.
    test.setTimeout(scaledMs(120_000));
    const failures = await openEditor(page);
    const { editorBounds } = await openDonnerSplash(page);
    const at = await parkPointerOverPane(page, editorBounds);

    await installCompositedProbe(page);
    await startCompositedProbe(page);
    const stream = await burstZoomStorm(page, at, {
      bursts: 3,
      notchesPerBurst: 6,
      settleMs: scaledMs(200),
    });
    const result = await stopCompositedProbe(page);

    assertProbeUsable(result, kMinimumProbeSamples);
    // An ignored storm proves nothing: require that the presented scale moved.
    expect(
      distinctPresentedDocumentWidths(result),
      `the zoom storm never changed the presented scale; stream=${JSON.stringify(stream)}`,
    ).toBeGreaterThan(1);

    const stats = blackFrameStats(result.samples, 40);
    // Name the empty frames rather than only counting them: whether they sit on
    // a backing-store resize, on a content-scale change, or in the middle of a
    // stable stretch is what separates a read-back race from a real clear.
    const blackDetail = stats.blackSampleIndices.slice(0, 4).map((index) => {
      const sample = result.samples[index];
      const previous = result.samples[index - 1];
      return {
        index,
        meanAlpha: Number(sample.meanAlpha.toFixed(1)),
        contentWidth: sample.coloredWidth,
        contentWidthChanged: previous !== undefined && previous.coloredWidth !== sample.coloredWidth,
        backing: `${sample.backingWidth}x${sample.backingHeight}`,
      };
    });
    console.log(
      `composited-black-frames engine=${browserName} samples=${result.samples.length}`
        + ` black=${stats.blackSamples} fraction=${stats.fraction.toFixed(4)}`
        + ` longestRun=${stats.longestRun} wheels=${stream.activeWheelEvents}`
        + ` detail=${JSON.stringify(blackDetail)}`,
    );
    // The 1% fraction bound is enforced on Chromium only.
    //
    // Measured on Gecko at 78fcea1e3, this same window reports 1-3 near-empty
    // samples out of 25-45 (fraction 0.04-0.11), reproducibly at the same two
    // places: the first zoom-out epoch of a burst (mean alpha 2.5, visible
    // width 892) and the fully zoomed-out epoch (mean alpha 0, visible width
    // 89.5). Both carry valid geometry, an unchanged backing store, and a
    // frame token that did not move, so they are not the per-epoch clear this
    // test is named for. They are either a Gecko-specific presentation gap or
    // a limitation of reading a worker-owned WebGPU canvas back through
    // drawImage under Gecko, and telling those apart needs a Gecko diagnosis
    // this lane cannot do from the outside. Until that is resolved, Gecko
    // keeps the sustained-blackout bound below (which the named regression
    // fails outright) and reports its fraction without asserting on it, rather
    // than the lane carrying a bound tuned to noise. Do not widen the Chromium
    // bound to make Gecko fit.
    if (browserName !== "firefox") {
      expect(
        stats.fraction,
        `${stats.blackSamples}/${stats.consideredSamples} composited samples had an empty visible`
          + ` region (mean alpha < 40); longest run ${stats.longestRun} starting at sample`
          + ` ${stats.longestRunStart}; black samples ${JSON.stringify(blackDetail)}`,
      ).toBeLessThanOrEqual(0.01);
    }
    // Sustained blackout, enforced on every engine. Gecko services roughly a
    // tenth of Chromium's animation frames here, so the same run length is a
    // much longer wall-clock window there; allow one more sample rather than
    // pretending the two are the same measurement.
    const runBound = browserName === "firefox" ? 3 : 2;
    expect(
      stats.longestRun,
      `the document was missing for ${stats.longestRun} consecutive composited frames`,
    ).toBeLessThanOrEqual(runBound);
    expect(failures).toEqual([]);
  });

  test("c: a zoom storm never resizes the canvas backing store", async ({
    browserName,
    page,
  }) => {
    // GUARDS: resizes that clear the backing store. The black frames in (a) are
    // the symptom; this is the cause, and it is the cheaper, more stable
    // signal.
    //
    // The bound is re-derived for the single canvas and is now ZERO, not two.
    // The old document canvas was sized from the raster the worker produced, so
    // a zoom legitimately resized it (the previous presenter sized it at the
    // viewport's raster backing cap precisely to bound that to a couple of
    // reconfigures per storm). This canvas is the window: its backing store is
    // a function of window size and device pixel ratio, neither of which a zoom
    // gesture touches. Any resize during a storm means something has started
    // sizing the canvas from content again, which is the defect.
    test.setTimeout(scaledMs(120_000));
    const failures = await openEditor(page);
    const { editorBounds } = await openDonnerSplash(page);
    const at = await parkPointerOverPane(page, editorBounds);

    await installCompositedProbe(page);
    await startCompositedProbe(page);
    const stream = await burstZoomStorm(page, at, {
      bursts: 3,
      notchesPerBurst: 6,
      settleMs: scaledMs(200),
    });
    const result = await stopCompositedProbe(page);

    assertProbeUsable(result, kMinimumProbeSamples);
    const transitions = backingSizeTransitions(result.samples);
    // The first entry is the size at the start of the window, not a change.
    const changes = Math.max(0, transitions.length - 1);
    console.log(
      `composited-backing-stability engine=${browserName} samples=${result.samples.length}`
        + ` changes=${changes} sizes=${JSON.stringify(transitions)}`
        + ` wheels=${stream.activeWheelEvents}`,
    );
    expect(
      changes,
      `the canvas backing store changed size ${changes} times during the storm:`
        + ` ${JSON.stringify(transitions)}. The canvas is the window; a zoom must not resize it.`,
    ).toBe(0);
    expect(failures).toEqual([]);
  });

  test("d: a pan stream moves the presented document on most frames", async ({
    browserName,
    page,
  }) => {
    // Gecko carve-out (#927): on the single-canvas build the app presents
    // every UI frame, and Gecko cannot sample that. The page rAF starves to a
    // handful of probe samples per gesture window (each drawImage of the
    // continuously-presented WebGPU canvas appears to synchronize against the
    // presenter), and the samples that land intermittently read empty in runs
    // the same-task retry cannot rescue. Chromium samples the same build
    // cleanly and enforces every pixel invariant; manual Firefox validation
    // shows the document presenting normally. Closing #927 means diagnosing
    // the Gecko readback behavior, not adding another proxy observable.
    test.skip(
      browserName === "firefox",
      "Gecko cannot sample a continuously-presented WebGPU canvas (#927)",
    );
    // GUARDS: pan shipped completely broken with a green board. It never
    // requested a worker epoch, and placement was pinned to the epoch viewport
    // so the document could not move between epochs. The old assertion polled
    // the surface's bounding box once after the gesture and only required that
    // it had moved at all, which a pan that jumps once at the end also
    // satisfies. Motion is a per-frame property, so assert it per frame.
    //
    // The observable is the CONTENT centroid, not an element box: with one
    // canvas nothing on the page moves during a pan, so element geometry can no
    // longer see the difference between a tracking pan and a frozen one.
    test.setTimeout(scaledMs(120_000));
    const failures = await openEditor(page);
    const { editorBounds, viewport } = await openDonnerSplash(page);
    const at = await parkPointerOverPane(page, editorBounds);

    // Sample a window over the document, at a resolution that can resolve one
    // frame of pan.
    //
    // Two separate things made the whole-canvas version blind. The canvas is
    // the editor: menu bar, tool rail and side panels are chromatic, outnumber
    // the document's pixels and never move, so they drag the centroid toward a
    // fixed point. And the read-back is 64 columns wide, so a whole-canvas
    // sample is 25 CSS px per column while one frame of this pan is under 2 -
    // an eighth of the 0.25-column motion threshold, which turns a perfectly
    // tracking pan into a mostly-stationary one (measured 0.15 against a bound
    // of 0.40 on hardware Chromium, on a build whose pan was fine).
    //
    // A 280 CSS px window read back at 96 columns is 2.9 CSS px per column, so
    // one frame of pan clears the threshold on its own. Measured on hardware
    // Chromium across the same gesture: 0.42 at a 420 px window, 0.65 at 280,
    // which is the ~0.59 the bound below was calibrated against. The chromatic
    // thresholds are raised for the same reason as the drag suite's: the Splash
    // artboard background is #10131e, spread 14, so at the default of 12 the
    // background counts as content and pins the centroid at the window's centre
    // no matter what the document does.
    const panSampleRegion = documentCenterWindow(viewport, 280);
    await installCompositedProbe(page, {
      sampleRegionCss: panSampleRegion,
      sampleWidth: 96,
      sampleHeight: 96,
      minColorAlpha: 64,
      minColorSpread: 60,
    });
    await startCompositedProbe(page);
    // No momentum tail and no trailing settle here, unlike the storm tests.
    // The invariant is "the surface tracks the gesture", so the sampled window
    // has to be the gesture. A decaying tail and a quiet settle contribute
    // frames in which a perfectly working pan correctly does not move, which
    // would turn the bound into a statement about how long the tail is. The
    // tail's own defect class (a resize committed once input goes quiet) is
    // covered by (a) and (c), which do include it.
    // Playwright's Firefox services animation frames at roughly a tenth of
    // Chromium's rate while the Wasm editor is running, so a 700 ms window
    // yields under ten samples there: too few to say anything about a
    // per-frame fraction. Lengthen the gesture rather than lowering the bar,
    // and hold the total pan DISTANCE fixed while doing it, so the slow engine
    // does not scroll the document out of the pane and turn "did not move"
    // into "was not there".
    const panDurationMs = scaledMs(browserName === "firefox" ? 2_800 : 700);
    const kPanDistanceCssPx = 294;
    const stream = await panStream(page, at, {
      dxPerSec: 0,
      dyPerSec: (kPanDistanceCssPx * 1000) / panDurationMs,
      durationMs: panDurationMs,
      hz: 90,
      momentumMs: 0,
    });
    const result = await stopCompositedProbe(page);

    // Motion is now read from the pixels, so the read-back path has to work for
    // this test to say anything at all.
    assertProbeUsable(result, kMinimumProbeSamples);
    const motion = contentMotionFraction(result.samples);
    console.log(
      `composited-pan-motion engine=${browserName} samples=${result.samples.length}`
        + ` moved=${motion.movedSamples}/${motion.comparedSamples}`
        + ` fraction=${motion.fraction.toFixed(3)} wheels=${stream.activeWheelEvents}`
        + ` totalDeltaY=${stream.totalDeltaY.toFixed(1)}`,
    );
    // Chromium updates placement about as often as it services animation
    // frames, so a working pan moves in most sampled frames (measured ~0.59).
    // Gecko services animation frames faster than it applies placement here,
    // so the same working pan measures 0.29-0.39 across runs with the motion
    // spread evenly through the window. Both bounds sit far above what the
    // guarded defect produces: an epoch-pinned pan moves only when the worker
    // publishes, which is a handful of frames out of ninety (~0.03). The Gecko
    // bound keeps headroom against that measured spread rather than sitting on
    // the low end of it.
    const motionBound = browserName === "firefox" ? 0.2 : 0.4;
    expect(
      motion.fraction,
      `the presented document moved in only ${motion.movedSamples}/${motion.comparedSamples}`
        + " sampled frames during a continuous pan; an epoch-pinned pan looks exactly like this",
    ).toBeGreaterThanOrEqual(motionBound);
    expect(failures).toEqual([]);
  });

  test("e: a discriminated pinch to 1.25x scales the content by 1.25", async ({
    browserName,
    page,
  }) => {
    // GUARDS: the pinch gain gap. A ctrl-flagged wheel with a fractional delta
    // is a synthesized trackpad pinch carrying `deltaY = -100 * ln(scale)`; the
    // input bridge has to discriminate it from a real ctrl+scroll and apply the
    // desktop zoom calibration. When that path regressed, a 1.25x gesture
    // applied 10.49x, which no settled screenshot noticed because the document
    // was still a correctly rendered document, just at the wrong zoom.
    //
    // The observable is the width of the document's own chromatic content.
    // There is no element whose box tracks the zoom: the canvas is the window.
    test.setTimeout(scaledMs(60_000));
    const failures = await openEditor(page);
    const { editorBounds } = await openDonnerSplash(page);
    const at = await parkPointerOverPane(page, editorBounds);

    const beforeWidth = await splashArtWidth(page);
    expect(beforeWidth, "the presented document has no measurable width").toBeGreaterThan(0);

    const kTargetScale = 1.25;
    // One event: `durationMs` of a single 90 Hz tick, no momentum tail, no
    // jitter. The whole gesture is one discriminated wheel, so the applied gain
    // is attributable to exactly that event.
    const stream = await pinchStream(page, at, {
      totalScale: kTargetScale,
      durationMs: 1000 / 90,
      hz: 90,
      momentumMs: 0,
      jitterPx: 0,
    });
    expect(
      stream.activeWheelEvents,
      "the parity check must dispatch exactly one discriminated pinch wheel",
    ).toBe(1);
    expect(stream.encodedScale).toBeCloseTo(kTargetScale, 3);

    await expect
      .poll(async () => (await splashArtWidth(page)) / beforeWidth, {
        message: "the pinch never scaled the presented document",
        timeout: scaledMs(5_000),
        intervals: [16, 25, 50, 100],
      })
      .toBeGreaterThan(kTargetScale * 0.91);
    const ratio = (await splashArtWidth(page)) / beforeWidth;
    console.log(
      `composited-pinch-parity engine=${browserName} encoded=${stream.encodedScale.toFixed(4)}`
        + ` applied=${ratio.toFixed(4)} target=${kTargetScale}`,
    );
    // The band is the instrument's precision, not gesture tolerance: the art's
    // extent is measured on a 320-pixel-wide read-back of the canvas, so each
    // edge quantizes to about 1% of the extent and the before/after RATIO
    // stacks up to four edge errors. Measured on a correct pinch: 1.2556 on
    // both engines, byte-identical, 0.4% off target. 9% keeps every defect
    // this test exists for out of reach: the weak-gain regression read 10.49x
    // and the double-gain regression 2x.
    expect(
      ratio,
      `a ${kTargetScale}x pinch applied ${ratio.toFixed(3)}x to the presented content`,
    ).toBeLessThan(kTargetScale * 1.09);
    expect(failures).toEqual([]);
  });
});
