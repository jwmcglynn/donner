import { expect, type Page, test } from "@playwright/test";
import { readEditorPixelBounds, readEditorPixelBoundsFromPng } from "./canvas-color-stats";
import {
  attachCompositedReadbacks,
  blackFrameStats,
  type CompositedProbeResult,
  type CompositedSample,
  contentMotionFraction,
  type DragRegression,
  dragRegressions,
  installCompositedProbe,
  presentedFrameOrdering,
  readDocumentArtWidth,
  readViewportStats,
  startCompositedProbe,
  stopCompositedProbe,
  type ViewportStats,
  visibleDocumentRegion,
} from "./composited-probe";
import { confirmBlueBodyRegressions, kOrderingDrag, orderingTrajectoryRegion } from "./drag-ordering";
import { dragStream, pointerClick, waitForAppliedPointer } from "./gesture-streams";
import { expectSampleThumbnailsToSettle } from "./sample-thumbnail-settle";
import { scaledMs } from "./test-timing";

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

/** Wait until the presentation worker is observably idle past its follow-up-render debounce. */
async function waitForPresentationQuiescence(page: Page, message: string): Promise<void> {
  let lastCompletedResults: number | undefined;
  let stableSinceMs = Date.now();
  let lastSnapshot: { completedResults?: unknown; workerBusy?: unknown } = {};
  try {
    await expect
      .poll(
        async () => {
          const snapshot = await page.evaluate(() => ({
            completedResults: (window as unknown as {
              __donnerWorkerStats?: { completedResults?: unknown };
            }).__donnerWorkerStats?.completedResults,
            workerBusy: (window as unknown as {
              __donnerInteractionStats?: { workerBusy?: unknown };
            }).__donnerInteractionStats?.workerBusy,
          }));
          lastSnapshot = snapshot;
          const now = Date.now();
          const valid = typeof snapshot.completedResults === "number"
            && Number.isFinite(snapshot.completedResults)
            && typeof snapshot.workerBusy === "boolean";
          if (!valid) {
            lastCompletedResults = undefined;
            stableSinceMs = now;
            return false;
          }
          if (snapshot.workerBusy || snapshot.completedResults !== lastCompletedResults) {
            lastCompletedResults = snapshot.completedResults;
            stableSinceMs = now;
            return false;
          }
          return now - stableSinceMs >= scaledMs(400);
        },
        {
          message,
          timeout: scaledMs(6_000),
          intervals: [16, 25, 50, 100],
        },
      )
      .toBe(true);
  } catch (error) {
    throw new Error(`${message}; last diagnostics=${JSON.stringify(lastSnapshot)}`, {
      cause: error,
    });
  }
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
  // first Firefox worker frame on a loaded runner. Let the picker's thumbnail lane drain first; the
  // document-worker result below remains the actual proof that the selected sample is live.
  await expectSampleThumbnailsToSettle(page, {
    message: "the sample thumbnail lane must drain before the drag sample replaces it",
    timeout: scaledMs(20_000),
  });
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
  await waitForPresentationQuiescence(
    page,
    "Donner Splash presentation must settle before the drag fixture is measured",
  );
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

const kBlueRectOffset = { x: 408, y: 353 };

async function openBasicShapes(page: Page): Promise<{
  editorBounds: Rect;
  documentRect: Rect;
  blueBounds: Rect;
}> {
  const editorCanvas = page.locator("canvas#canvas");
  const editorBounds = await editorCanvas.boundingBox();
  expect(editorBounds, "the editor canvas is missing").not.toBeNull();
  if (editorBounds === null) throw new Error("editor canvas is missing");
  await expectSampleThumbnailsToSettle(page, {
    message: "the sample thumbnail lane must drain before Basic Shapes replaces it",
    timeout: scaledMs(20_000),
  });
  const before = await page.evaluate(() =>
    (window as unknown as { __donnerWorkerStats?: { completedResults?: number } })
      .__donnerWorkerStats?.completedResults ?? 0
  );
  await page.mouse.click(editorBounds.x + editorBounds.width * 0.76, editorBounds.y + 282);
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
  await waitForPresentationQuiescence(
    page,
    "Basic Shapes presentation must settle before the drag fixture is measured",
  );
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

/** The editor's completed frames over the first five ordering candidates (presentedFrameOrdering). */
function editorFrameOrdering(
  samples: readonly CompositedSample[],
  candidates: readonly DragRegression[],
) {
  return candidates.slice(0, 5).map((candidate) => ({
    sampleIndex: candidate.sampleIndex,
    ...presentedFrameOrdering(samples, candidate),
  }));
}

async function retainedBlueBodyPairs(
  page: Page,
  candidates: readonly DragRegression[],
): Promise<Array<readonly [CompositedSample, CompositedSample] | null>> {
  return page.evaluate((indices) => {
    const probe = window.__donnerCompositedProbe as unknown as {
      running: boolean;
      samples: CompositedSample[];
      rawReadbacks: Array<Uint8ClampedArray | null>;
      rawReadbackWidth: number;
      rawReadbackHeight: number;
      rawReadbackOverflow: boolean;
    } | undefined;
    const matchesBlue = (window as unknown as {
      __donnerMatchesCompositedContentPixel?: (
        red: number,
        green: number,
        blue: number,
        alpha: number,
      ) => boolean;
    }).__donnerMatchesCompositedContentPixel;
    if (probe === undefined || probe.running || probe.rawReadbackOverflow || !matchesBlue) {
      return indices.map(() => null);
    }
    const { rawReadbackWidth: width, rawReadbackHeight: height } = probe;
    if (!(width > 0 && height > 0)) return indices.map(() => null);
    const measure = (
      index: number,
      startY: number,
      referenceHeight: number,
      minimumPixels: number,
    ) => {
      const pixels = probe.rawReadbacks[index];
      const sample = probe.samples[index];
      if (
        !(pixels instanceof Uint8ClampedArray) || pixels.length !== width * height * 4
        || sample === undefined
      ) return null;
      let count = 0;
      let sumX = 0;
      let minX = width;
      let maxX = -1;
      let bottom = -1;
      for (let y = startY; y < height; ++y) {
        for (let x = 0; x < width; ++x) {
          const offset = (y * width + x) * 4;
          if (
            !matchesBlue(
              pixels[offset],
              pixels[offset + 1],
              pixels[offset + 2],
              pixels[offset + 3],
            )
          ) continue;
          ++count;
          sumX += x;
          minX = Math.min(minX, x);
          maxX = Math.max(maxX, x);
          bottom = y;
        }
      }
      if (count < minimumPixels) return null;
      return {
        ...sample,
        coloredPixels: count,
        coloredCentroidX: sumX / count,
        coloredCentroidY: bottom,
        coloredMinX: minX,
        coloredMinY: bottom - referenceHeight + 1,
        coloredWidth: maxX - minX + 1,
        coloredHeight: referenceHeight,
      };
    };
    return indices.map(([previousIndex, sampleIndex]) => {
      const previous = probe.samples[previousIndex];
      if (previous === undefined || previous.coloredHeight <= 0) return null;
      const startY = Math.ceil(previous.coloredMinY + previous.coloredHeight * 0.25);
      const minimumPixels = previous.coloredPixels * 0.25;
      const before = measure(previousIndex, startY, previous.coloredHeight, minimumPixels);
      const after = measure(sampleIndex, startY, previous.coloredHeight, minimumPixels);
      return before === null || after === null ? null : [before, after] as const;
    });
  }, candidates.map(({ predecessorIndex, sampleIndex }) => [predecessorIndex, sampleIndex]));
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
  test("a: browser drag distance follows the measured viewport zoom", async ({ browserName, page }) => {
    test.skip(browserName !== "chromium", "This CSS-pixel measurement runs in the Chromium lane");
    test.setTimeout(scaledMs(90_000));
    const failures = await openEditor(page);
    const { editorBounds, blueBounds } = await openBasicShapes(page);
    const target = { x: editorBounds.x + kBlueRectOffset.x, y: editorBounds.y + kBlueRectOffset.y };
    // The earlier manual shortfall was 50 -> about 43 document units. A
    // controlled 1.15 zoom makes that ratio explicit in a browser gesture.
    // The editor's GLFW scroll callback uses the current pointer position. Move it from the
    // carousel card into the render pane before dispatching the synthetic ctrl-wheel event.
    await page.mouse.move(target.x, target.y);
    await page.evaluate(({ x, y }) => {
      document.querySelector("canvas#canvas")?.dispatchEvent(
        new WheelEvent("wheel", {
          bubbles: true,
          cancelable: true,
          clientX: x,
          clientY: y,
          ctrlKey: true,
          deltaMode: WheelEvent.DOM_DELTA_PIXEL,
          deltaY: -100 * Math.log(1.15),
        }),
      );
    }, target);
    await expect.poll(async () => (await readViewportStats(page))?.zoom ?? 0).toBeGreaterThan(1.14);
    const viewport = await readSettledViewportStats(page);
    expect(viewport.zoom).toBeCloseTo(1.15, 2);
    const documentRect = {
      x: viewport.documentX,
      y: viewport.documentY,
      width: viewport.documentWidth,
      height: viewport.documentHeight,
    };
    let initialBlue: ReturnType<typeof readEditorPixelBoundsFromPng> = null;
    await expect.poll(async () => {
      const shot = await page.screenshot({ clip: documentRect });
      initialBlue = readEditorPixelBoundsFromPng(shot, "basic-blue", documentRect, {
        minX: 0,
        minY: 0,
        maxX: documentRect.width,
        maxY: documentRect.height,
      });
      return initialBlue === null ? 0 : initialBlue.maxX - initialBlue.minX;
    }, { timeout: scaledMs(10_000) }).toBeGreaterThan(blueBounds.width * 1.1);
    if (initialBlue === null) throw new Error("blue shape is missing after zoom");
    const pointerMoves = await page.evaluate(() => {
      const moves: Array<{ x: number; y: number }> = [];
      document.querySelector<HTMLCanvasElement>("canvas#canvas")?.addEventListener(
        "pointermove",
        (event) => {
          moves.push({ x: event.clientX, y: event.clientY });
        },
      );
      (window as unknown as { __dragDistanceProbe?: typeof moves }).__dragDistanceProbe = moves;
      return moves;
    });
    expect(pointerMoves).toEqual([]);

    await pointerClick(page, target);
    await expect.poll(() => selectedCount(page)).toBeGreaterThan(0);
    await page.waitForTimeout(scaledMs(800));
    await page.mouse.move(target.x, target.y);
    await page.mouse.down();
    await expect.poll(async () =>
      page.evaluate(() =>
        (window as unknown as { __donnerInteractionStats?: { dragging?: boolean } })
          .__donnerInteractionStats?.dragging ?? false
      )
    ).toBe(true);
    await page.mouse.move(target.x + 50, target.y + 30);
    await waitForAppliedPointer(page, { x: target.x + 50, y: target.y + 30 }, {
      timeoutMs: scaledMs(5_000),
      message: "drag endpoint",
    });
    await expect.poll(async () =>
      page.evaluate(() =>
        (window as unknown as { __donnerInteractionStats?: { moved?: boolean } })
          .__donnerInteractionStats?.moved ?? false
      )
    ).toBe(true);
    await page.mouse.up();
    const moves = await page.evaluate(() =>
      (window as unknown as { __dragDistanceProbe?: Array<{ x: number; y: number }> })
        .__dragDistanceProbe ?? []
    );
    expect(moves.at(-1)).toEqual({ x: target.x + 50, y: target.y + 30 });

    let finalBlue: ReturnType<typeof readEditorPixelBoundsFromPng> = null;
    await expect.poll(async () => {
      const shot = await page.screenshot({ clip: documentRect });
      finalBlue = readEditorPixelBoundsFromPng(shot, "basic-blue", documentRect, {
        minX: 0,
        minY: 0,
        maxX: documentRect.width,
        maxY: documentRect.height,
      });
      return finalBlue?.minX ?? -1;
    }, { timeout: scaledMs(10_000) }).toBeGreaterThan(initialBlue.minX + 30);
    console.log(
      `drag-distance-evidence ${
        JSON.stringify({ viewport, target, moves, initialBlue, finalBlue })
      }`,
    );
    expect(finalBlue).not.toBeNull();
    if (finalBlue === null) throw new Error("blue shape is missing after drag");
    expect(Math.abs(finalBlue.minX - initialBlue.minX - 50)).toBeLessThan(3);
    expect(Math.abs(finalBlue.minY - initialBlue.minY - 30)).toBeLessThan(3);
    expect(failures).toEqual([]);
  });

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
    const candidates = dragRegressions(result.samples, stream.trace, 1.0, 2.0, scaledMs(150));
    if (candidates.length > 0) {
      const baselineIndex = result.samples.findIndex((sample) =>
        sample.drawOk && sample.coloredWidth > 0
      );
      const indices = [
        baselineIndex,
        ...candidates.slice(0, 5).flatMap((violation) => [
          violation.predecessorIndex,
          violation.sampleIndex,
          violation.sampleIndex + 1,
        ]),
      ];
      const readbacks = await attachCompositedReadbacks(page, test.info(), indices);
      // The editor's own completed frames over each candidate say whether the editor or only the
      // readback went back to an older position; composited-probe.json keeps them per sample.
      const editorFrames = editorFrameOrdering(result.samples, candidates);
      console.log(
        `drag-readback-evidence ${JSON.stringify({ baselineIndex, ...readbacks, editorFrames })}`,
      );
    }
    const bodies = await retainedBlueBodyPairs(page, candidates);
    const violations = confirmBlueBodyRegressions(
      candidates,
      bodies,
      stream.trace,
      scaledMs(150),
    );
    const presentedFrames = new Set(
      result.samples.filter((sample) => sample.drawOk && sample.coloredWidth > 0).map((sample) =>
        `${Math.round(sample.coloredCentroidX)}x${Math.round(sample.coloredCentroidY)}`
      ),
    );
    console.log(
      `drag-frame-monotonicity engine=${browserName} samples=${result.samples.length}`
        + ` presentedFrames=${presentedFrames.size} candidates=${candidates.length}`
        + ` violations=${violations.length}`
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
      `${violations.length} samples presented an OLDER frame than one already shown`
        + (violations.length === 0
          ? ""
          : `; editor completed frames (verdicts in composited-probe-evidence.mjs:`
            + ` editor-frame-went-backwards is an editor bug, readback-only a stale readback):`
            + ` ${JSON.stringify(editorFrameOrdering(result.samples, violations))}`),
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
