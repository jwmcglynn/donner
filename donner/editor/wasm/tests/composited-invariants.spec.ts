import { expect, type Page, test } from "@playwright/test";
import { burstZoomStorm, panStream, pinchStream } from "./gesture-streams";
import {
  backingSizeTransitions,
  blackFrameStats,
  type CompositedProbeResult,
  describeEmptyReadbacks,
  installCompositedProbe,
  motionFraction,
  readVisibleSurfaceWidth,
  startCompositedProbe,
  surfaceAlternationViolations,
  stopCompositedProbe,
} from "./composited-probe";

/**
 * Composited-output invariants for the editor's worker-surface presentation.
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

/** Open the Donner Splash from the carousel and wait for its first epoch. */
async function openDonnerSplash(page: Page): Promise<{
  editorBounds: { x: number; y: number; width: number; height: number };
}> {
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
        message: "Donner Splash must publish an accepted worker presentation epoch",
        timeout: scaledMs(5_000),
        intervals: [16, 25, 50, 100],
      },
    )
    .toBeGreaterThan(0);
  return { editorBounds };
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
 * Without this, a browser that refuses to draw the worker-owned canvas into a
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

/**
 * Precondition for EVERY invariant here: the window is long enough to say
 * something about per-frame behavior.
 *
 * Deliberately says nothing about pixels. Most invariants in this file read
 * only DOM observables - the presented epoch token, the visible surface's CSS
 * geometry, which DOM canvas carries it, the backing-store size - and the probe
 * records those whether or not the composited read-back produced anything.
 * Gating a DOM-only invariant on read-back availability makes it fail for a
 * reason it does not depend on.
 */
function assertProbeSampled(result: CompositedProbeResult, minimumSamples: number): void {
  expect(
    result.samples.length,
    `the probe collected ${result.samples.length} samples, expected at least ${minimumSamples};`
      + " the sampled window was too short to say anything about per-frame behavior",
  ).toBeGreaterThanOrEqual(minimumSamples);
}

/**
 * Extra precondition for the invariants that READ PIXELS back.
 *
 * Call this only from a test whose assertion consumes `meanAlpha`, `meanLuma`,
 * `coloredPixels`, or `coloredCentroid*`. For a DOM-only invariant an empty
 * read-back is irrelevant, not disqualifying.
 */
function assertReadbackUsable(result: CompositedProbeResult): void {
  const usable = result.samples.filter((sample) => sample.drawOk).length;
  expect(
    usable / result.samples.length,
    `only ${usable}/${result.samples.length} samples produced composited pixels;`
      + " the read-back path is unavailable, so these invariants would pass vacuously",
  ).toBeGreaterThan(0.8);
  // The bounded same-task retry rescues the known Gecko first-read-after-
  // promotion race in 100 percent of observed cases (measured to load average
  // 127). Retries that fire WITHOUT rescuing indicate a different, sustained
  // snapshot-unavailability bug - fail loudly with the geometry instead of
  // letting empty samples degrade into the per-invariant bounds, where the
  // failure mode would be untraceable. The counters alone cannot say which
  // mode it was, so print the per-sample element box, backing store, and
  // source rectangle with them.
  if (result.readbackRetries > 0) {
    expect(
      result.readbackRetryRescues,
      `${result.readbackRetries} read-back retries fired but rescued 0 samples - a sustained`
        + " snapshot-unavailability mode, not the known per-handoff race; "
        + describeEmptyReadbacks(result.samples),
    ).toBeGreaterThan(0);
  }
}

/** Distinct visible widths, proof that a zoom stream actually moved the scale. */
function distinctVisibleWidths(result: CompositedProbeResult): number {
  return new Set(
    result.samples.filter((sample) => sample.surfaceId !== "").map((sample) =>
      Math.round(sample.cssWidth)
    ),
  ).size;
}

test.describe("composited output invariants", () => {
  test("a: a zoom storm never shows an empty visible region", async ({ browserName, page }) => {
    // GUARDS: the backing-store-clear flicker. A per-epoch canvas resize clears
    // the backing store, and the clear landed on a canvas that was still
    // visible, so ~18% of frames under a burst storm composited a fully
    // transparent document region. Geometry stayed perfect throughout, which is
    // why every geometry-based assertion stayed green; only the pixels moved.
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

    assertProbeSampled(result, kMinimumProbeSamples);
    assertReadbackUsable(result);
    // An ignored storm proves nothing: require that the presented scale moved.
    expect(
      distinctVisibleWidths(result),
      `the zoom storm never changed the presented scale; stream=${JSON.stringify(stream)}`,
    ).toBeGreaterThan(1);

    const stats = blackFrameStats(result.samples, 40);
    // Name the empty frames rather than only counting them: whether they sit
    // on an epoch boundary, on a backing-store resize, or in the middle of a
    // stable epoch is what separates a read-back race from a real clear.
    const blackDetail = stats.blackSampleIndices.slice(0, 4).map((index) => {
      const sample = result.samples[index];
      const previous = result.samples[index - 1];
      return {
        index,
        meanAlpha: Number(sample.meanAlpha.toFixed(1)),
        frameToken: sample.frameToken,
        frameTokenChanged: previous !== undefined && previous.frameToken !== sample.frameToken,
        cssWidth: Number(sample.cssWidth.toFixed(1)),
        backing: `${sample.backingWidth}x${sample.backingHeight}`,
        surfaceId: sample.surfaceId,
      };
    });
    console.log(
      `composited-black-frames engine=${browserName} samples=${result.samples.length}`
        + ` black=${stats.blackSamples} fraction=${stats.fraction.toFixed(4)}`
        + ` longestRun=${stats.longestRun} wheels=${stream.activeWheelEvents}`
        + ` readbackRetries=${result.readbackRetries}/${result.readbackRetryRescues}`
        + ` detail=${JSON.stringify(blackDetail)}`,
    );
    // Enforced on every engine, at the same bound.
    //
    // This used to be Chromium-only. Gecko reported 1-3 near-empty samples per
    // storm and long empty runs on CI, and the lane could not tell a Gecko
    // presentation gap from a Gecko read-back limitation from outside. It is
    // the read-back: the first main-thread `drawImage` snapshot of a canvas
    // the worker has just presented into can return transparent while the
    // canvas is fine, and a second `drawImage` in the same task returns the
    // real pixels (see "WHY THE READ-BACK RETRIES" in `composited-probe.ts`
    // for the standalone measurement, 128 of 128 recovered, Chromium 0 of
    // 6327). The probe now retries, `readbackRetries` above reports how often
    // that fired, and the engines are held to one bound again.
    expect(
      stats.fraction,
      `${stats.blackSamples}/${stats.consideredSamples} composited samples had an empty visible`
        + ` region (mean alpha < 40); longest run ${stats.longestRun} starting at sample`
        + ` ${stats.longestRunStart}; black samples ${JSON.stringify(blackDetail)}`,
    ).toBeLessThanOrEqual(0.01);
    // Sustained blackout. Gecko's widened allowance came from the same
    // read-back artifact, not from its frame rate: a slower engine produces
    // FEWER consecutive samples across a given wall-clock gap, not more, so
    // there was never a throughput reason for the two engines to differ here.
    expect(
      stats.longestRun,
      `the document was missing for ${stats.longestRun} consecutive composited frames`,
    ).toBeLessThanOrEqual(2);
    expect(failures).toEqual([]);
  });

  test("b: presented pixels and their CSS geometry change atomically", async ({
    browserName,
    page,
  }) => {
    // GUARDS: the wrong-scale flicker. A direct WebGPU present commits
    // worker-side immediately, while the CSS layout matching those pixels is
    // applied when the main thread accepts the epoch, one or more task
    // boundaries later. When the two are not atomic the document is briefly on
    // screen at the previous epoch's scale. A settled check sees the correct
    // final scale and passes.
    //
    // The decidable form: a presented frame-token change whose visible CSS size
    // only catches up in a LATER sample is a violation. A size change in the
    // same sample as the token change is the atomic case.
    //
    // SCOPE, measured at 78fcea1e3 (18 epochs, 74 samples, zero violations on
    // Chromium): this invariant covers the DOM side of the swap only.
    // `data-direct-surface-frame` is written by the main thread when it accepts
    // an epoch, in the same style flush that writes the clip and the element
    // box, so the attribute and the geometry cannot drift apart by
    // construction. What it does catch is a presenter that splits those two
    // writes across frames, which is the shape any future refactor of the
    // acceptance path is most likely to reintroduce, and it is why the
    // assertion is worth having enforced from now on.
    //
    // The other half of the wrong-scale flicker - the window between the worker
    // committing epoch N+1 pixels and the main thread accepting them, during
    // which the DOM is self-consistently still epoch N while the pixels on
    // screen are not - is covered by test f below, which asserts the worker
    // never presents into the canvas that is currently visible.
    test.setTimeout(scaledMs(120_000));
    const failures = await openEditor(page);
    const { editorBounds } = await openDonnerSplash(page);
    const at = await parkPointerOverPane(page, editorBounds);

    await installCompositedProbe(page);
    await startCompositedProbe(page);
    // A paced fractional pinch, not a burst: the mismatch window is one or two
    // frames wide, so the gesture has to still be arriving while the sampler
    // runs. The coarse in-one-task shape settles between bursts and hides it.
    const stream = await pinchStream(page, at, {
      totalScale: 1.6,
      durationMs: scaledMs(700),
      hz: 90,
    });
    // Let the last epoch land so its geometry is inside the sampled window.
    await page.waitForTimeout(scaledMs(400));
    const result = await stopCompositedProbe(page);

    // (b) reads the presented epoch label and the visible surface's CSS
    // geometry, both DOM state the probe records regardless of what the
    // read-back returned, so it takes the sample-count precondition only.
    assertProbeSampled(result, kMinimumProbeSamples);
    expect(
      distinctVisibleWidths(result),
      `the pinch stream never changed the presented scale; stream=${JSON.stringify(stream)}`,
    ).toBeGreaterThan(1);

    // Live-viewport placement (the zoom-motion fix) re-places the SAME
    // epoch's layout continuously while a gesture is active, so "the size for
    // this token changed in a later sample" is normal operation, not a
    // violation - the original lookahead detector predates that design and
    // mis-fires on sparse samplers (first seen on the CI runner at ~4 Hz
    // effective sampling). The DOM-side invariant that survives live
    // placement: the visible surface's epoch token never moves BACKWARD - a
    // presenter regression that re-shows an older epoch's pixels is exactly
    // the stale-frame class this suite exists for. The pixel-vs-CSS pairing
    // itself is enforced structurally by test f (the worker never presents
    // into the visible canvas).
    const violations = [] as { index: number; from: number; to: number }[];
    let lastToken = 0;
    for (const [index, sample] of result.samples.entries()) {
      if (sample.frameToken !== 0 && sample.frameToken < lastToken) {
        violations.push({ index, from: lastToken, to: sample.frameToken });
      }
      if (sample.frameToken !== 0) {
        lastToken = sample.frameToken;
      }
    }
    // Which surface elements presented during the window. A presenter that
    // alternates between the two DOM canvases reports both here; one that
    // presents into a single canvas reports one. That is not asserted (either
    // is a legitimate implementation), but it is the first thing worth knowing
    // when this invariant goes red.
    const surfaces = Array.from(
      new Set(result.samples.map((sample) => sample.surfaceId).filter((id) => id !== "")),
    );
    console.log(
      `composited-epoch-atomicity engine=${browserName} samples=${result.samples.length}`
        + ` violations=${violations.length} wheels=${stream.activeWheelEvents}`
        + ` encodedScale=${stream.encodedScale.toFixed(3)}`
        + ` surfaces=${JSON.stringify(surfaces)}`
        + ` epochs=${new Set(result.samples.map((sample) => sample.frameToken)).size}`,
    );
    expect(
      violations.slice(0, 5),
      `${violations.length} samples presented an OLDER epoch than one already shown (first few`
        + ` shown)`,
    ).toEqual([]);
    expect(failures).toEqual([]);
  });

  test("c: a zoom storm resizes the canvas backing store at most twice", async ({
    browserName,
    page,
  }) => {
    // GUARDS: per-epoch resizes that clear the backing store. The black frames
    // in (a) are the symptom; this is the cause, and it is the cheaper, more
    // stable signal. A presenter that sizes its canvas once per zoom level
    // resizes a bounded number of times across a storm; one that resizes per
    // accepted epoch resizes dozens of times and clears the visible surface on
    // each one.
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

    // (c) reads the canvas backing-store size, DOM state independent of the
    // read-back.
    assertProbeSampled(result, kMinimumProbeSamples);
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
        + ` ${JSON.stringify(transitions)}`,
    ).toBeLessThanOrEqual(2);
    expect(failures).toEqual([]);
  });

  test("d: a pan stream moves the presented surface on most frames", async ({
    browserName,
    page,
  }) => {
    // GUARDS: pan shipped completely broken with a green board. It never
    // requested a worker epoch, and placement was pinned to the epoch viewport
    // so the surface could not move between epochs. The old assertion polled
    // the surface's bounding box once after the gesture and only required that
    // it had moved at all, which a pan that jumps once at the end also
    // satisfies. Motion is a per-frame property, so assert it per frame.
    test.setTimeout(scaledMs(120_000));
    const failures = await openEditor(page);
    const { editorBounds } = await openDonnerSplash(page);
    const at = await parkPointerOverPane(page, editorBounds);

    await installCompositedProbe(page);
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

    // Motion is read from the surface's CSS geometry, not its pixels, so this
    // test's usability bar is "a surface was present", not "the read-back
    // produced pixels".
    expect(
      result.samples.length,
      `the probe collected ${result.samples.length} samples during the pan`,
    ).toBeGreaterThanOrEqual(20);
    const presentSamples = result.samples.filter((sample) => sample.surfaceId !== "").length;
    expect(
      presentSamples / result.samples.length,
      `only ${presentSamples}/${result.samples.length} sampled frames had a visible worker`
        + " surface at all; motion cannot be measured across frames with nothing presented",
    ).toBeGreaterThan(0.8);
    const motion = motionFraction(result.samples);
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
      `the presented surface moved in only ${motion.movedSamples}/${motion.comparedSamples}`
        + " sampled frames during a continuous pan; an epoch-pinned pan looks exactly like this",
    ).toBeGreaterThanOrEqual(motionBound);
    expect(failures).toEqual([]);
  });

  // Measured RED at 78fcea1e3 on Chromium: 18 epochs across a 700 ms pinch, all
  // of them presented on `donner-document-canvas`, 17 alternation violations.
  // At that revision the direct WebGPU path registered a single DOM surface, so
  // every present overwrote the pixels currently on screen. The fix registers
  // both `#donner-document-canvas` and `#donner-document-canvas-back` as worker
  // surfaces and presents into the slot the main thread has not accepted.
  test("f: consecutive epochs present on alternating DOM canvases", async ({
    browserName,
    page,
  }) => {
    // GUARDS: the pixel side of the wrong-scale flicker, which test b cannot
    // see. A direct WebGPU present commits worker-side immediately; the CSS
    // that matches those pixels lands when the main thread accepts the epoch,
    // one or more task boundaries later. Presenting into the canvas that is
    // currently on screen puts epoch N+1 pixels under epoch N geometry for
    // that whole window - visible flicker between two scales on every epoch of
    // a pinch - while every attribute on the page stays self-consistent, so no
    // DOM assertion can catch it.
    //
    // The fix is to present into the canvas that is NOT visible and let
    // acceptance flip visibility and geometry together. Its decidable
    // consequence: the visible canvas must change on every epoch change. A
    // presenter pinned to one canvas reports a violation per epoch.
    test.setTimeout(scaledMs(120_000));
    const failures = await openEditor(page);
    const { editorBounds } = await openDonnerSplash(page);
    const at = await parkPointerOverPane(page, editorBounds);

    await installCompositedProbe(page);
    await startCompositedProbe(page);
    const stream = await pinchStream(page, at, {
      totalScale: 1.6,
      durationMs: scaledMs(700),
      hz: 90,
    });
    await page.waitForTimeout(scaledMs(400));
    const result = await stopCompositedProbe(page);

    // (f) reads which DOM canvas carried each epoch, independent of the
    // read-back.
    assertProbeSampled(result, kMinimumProbeSamples);
    const epochs = new Set(
      result.samples.map((sample) => sample.frameToken).filter((token) => token !== 0),
    );
    // An ignored gesture proves nothing: the storm has to have produced epochs
    // for the alternation to say anything at all.
    expect(
      epochs.size,
      `the pinch stream produced too few epochs to test alternation;`
        + ` stream=${JSON.stringify(stream)}`,
    ).toBeGreaterThan(2);

    const surfaces = Array.from(
      new Set(result.samples.map((sample) => sample.surfaceId).filter((id) => id !== "")),
    );
    const violations = surfaceAlternationViolations(result.samples);
    console.log(
      `composited-surface-alternation engine=${browserName} samples=${result.samples.length}`
        + ` epochs=${epochs.size} violations=${violations.length}`
        + ` surfaces=${JSON.stringify(surfaces)}`,
    );
    expect(
      surfaces.length,
      `the presenter used a single DOM canvas (${JSON.stringify(surfaces)}), so every epoch`
        + ` overwrote the pixels that were on screen`,
    ).toBe(2);
    expect(
      violations.slice(0, 5),
      `${violations.length} epoch changes presented on the canvas that was already visible`
        + ` (first few shown)`,
    ).toEqual([]);
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
    // The observable is the VISIBLE width (element box minus clip-path insets),
    // because the surface element spans its cap-sized backing store and its raw
    // element box barely changes with zoom.
    test.setTimeout(scaledMs(60_000));
    const failures = await openEditor(page);
    const { editorBounds } = await openDonnerSplash(page);
    const at = await parkPointerOverPane(page, editorBounds);

    const beforeWidth = await readVisibleSurfaceWidth(page);
    expect(beforeWidth, "the presented surface has no visible width").toBeGreaterThan(0);

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
      .poll(async () => (await readVisibleSurfaceWidth(page)) / beforeWidth, {
        message: "the pinch never scaled the presented document surface",
        timeout: scaledMs(5_000),
        intervals: [16, 25, 50, 100],
      })
      .toBeGreaterThan(kTargetScale * 0.93);
    const ratio = (await readVisibleSurfaceWidth(page)) / beforeWidth;
    console.log(
      `composited-pinch-parity engine=${browserName} encoded=${stream.encodedScale.toFixed(4)}`
        + ` applied=${ratio.toFixed(4)} target=${kTargetScale}`,
    );
    expect(
      ratio,
      `a ${kTargetScale}x pinch applied ${ratio.toFixed(3)}x to the presented content`,
    ).toBeLessThan(kTargetScale * 1.07);
    expect(failures).toEqual([]);
  });
});
