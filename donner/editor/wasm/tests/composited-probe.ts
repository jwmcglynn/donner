import { type Page } from "@playwright/test";

/**
 * Per-frame probe over the editor's COMPOSITED output.
 *
 * WHY THIS FILE EXISTS
 *
 * Every presentation regression that escaped the board this week was invisible
 * to the assertions we had, for the same structural reason: the suites
 * observed *end state*. They opened a document, ran a gesture, waited for the
 * worker to settle, and then checked geometry or a screenshot. A flicker is by
 * construction not an end state. A black frame that is gone 16 ms later, a
 * frame presented at the previous epoch's scale, a backing store cleared
 * mid-gesture: all of them settle correctly, so all of them pass.
 *
 * This probe samples the composited result on EVERY animation frame inside a
 * window the test controls, and records enough per sample to tell which of
 * those defects produced it:
 *
 *  - the pixels actually on screen (mean alpha, mean luma, colored pixel
 *    count, and the centroid of the chromatic ones) taken from `#canvas`,
 *  - the canvas backing store size, which is what a resize clears,
 *  - `window.__donnerWorkerStats.completedResults`, which orders the samples
 *    against the render worker's own progress.
 *
 * Pixels plus centroid plus backing size, per frame, is what makes the
 * invariants in `composited-invariants.spec.ts` decidable. Either alone is
 * ambiguous: geometry cannot see a cleared backing store, pixels cannot tell a
 * legitimately empty region from a dropped frame, and a settled check sees
 * neither.
 *
 * SINGLE CANVAS (single-canvas presenter architecture)
 *
 * There is one canvas and no CSS between document space and the screen, so the
 * per-sample observables that used to describe the seam are gone: the presented
 * epoch token (`data-direct-surface-frame`), the accepted-epoch token, which of
 * two DOM canvases was visible, and the clip-path insets that placed the
 * visible region. Motion and scale are now observable only INSIDE the canvas,
 * through `coloredCentroid*` and the content statistics, because the element
 * itself never moves.
 *
 * WHY drawImage AND NOT A SCREENSHOT
 *
 * `page.screenshot()` does not capture the transferred WebGPU canvas in
 * headless Chromium: every pixel of it reads back as the page background, so a
 * screenshot cannot distinguish "document missing" from "document present".
 * Drawing the canvas element into a small 2d canvas inside the page reads the
 * committed frame through the same path the compositor uses, and it is cheap
 * enough to run every rAF. The sample canvas is deliberately tiny; the
 * invariants here are about whether a frame is present and at what scale, not
 * about per-pixel fidelity, which the screenshot-based suites already cover.
 *
 * The probe reports `drawFailures` and per-sample `drawOk` so a run where the
 * read-back path itself is unavailable fails loudly instead of reporting a
 * comfortable stream of transparent frames.
 */

/** One composited sample, taken inside a single animation frame. */
export interface CompositedSample {
  /** `performance.now()` at sample time. */
  t: number;
  /** True when `#canvas` was present and readable for this sample. */
  canvasPresent: boolean;
  /** Canvas element box origin in CSS px. */
  visibleX: number;
  visibleY: number;
  /** Canvas element box size in CSS px. */
  cssWidth: number;
  cssHeight: number;
  /** Canvas backing store size in device px. */
  backingWidth: number;
  backingHeight: number;
  /** `window.__donnerWorkerStats.completedResults`. */
  completedResults: number;
  /** Mean alpha over the sampled region, 0-255. */
  meanAlpha: number;
  /** Mean luma over the sampled region, 0-255, premultiplied by nothing. */
  meanLuma: number;
  /** Pixels that are opaque enough and chromatic enough to be content. */
  coloredPixels: number;
  /**
   * Centroid of those pixels, in read-back pixels, or -1 when there were none.
   *
   * With one canvas this is the observable for ALL document motion: the canvas
   * fills the window and never moves, so `visibleX`/`visibleY` are constant and
   * can say nothing about whether a pan, a zoom, or a dragged object moved,
   * stalled, or jumped backward. The centroid of the document's chromatic
   * pixels can, without knowing anything about how those pixels reached the
   * screen.
   */
  coloredCentroidX: number;
  coloredCentroidY: number;
  /**
   * Width of the chromatic content's bounding box, in read-back pixels, or 0
   * when there was none.
   *
   * This is the SCALE observable. With one canvas the element box is the
   * window and never changes with zoom, so the only place a zoom is visible is
   * in how far apart the document's own pixels are. It saturates once the
   * content fills the sampled region, so it is only meaningful for gestures
   * that keep the document inside the pane.
   */
  coloredWidth: number;
  coloredHeight: number;
  /** Total pixels read back for this sample. */
  sampledPixels: number;
  /** False when the composited read-back did not produce pixels. */
  drawOk: boolean;
}

export interface CompositedProbeResult {
  samples: CompositedSample[];
  /** Samples whose composited read-back threw or produced no pixels. */
  drawFailures: number;
  /** Animation frames observed while the probe was running. */
  frames: number;
}

export interface CompositedProbeOptions {
  /** Read-back width in pixels. Small on purpose; see the file comment. */
  sampleWidth?: number;
  /** Read-back height in pixels. */
  sampleHeight?: number;
  /** Alpha at or above which a pixel counts toward `coloredPixels`. */
  minColorAlpha?: number;
  /** Channel spread at or above which a pixel counts as chromatic. */
  minColorSpread?: number;
  /**
   * Restrict the read-back to this viewport-CSS rectangle, intersected with
   * the visible surface region. Defaults to the whole visible region.
   *
   * Needed whenever the observable is one object rather than the document.
   * `coloredCentroid*` over a whole artboard is dominated by everything that
   * is NOT moving: a letter travelling 160 CSS px across the Donner Splash
   * shifts the full-document centroid by well under a read-back pixel, so a
   * perfectly working drag measures as zero motion. Sampling a window around
   * the object makes its motion the dominant term, which is the only way this
   * observable says anything about a single dragged shape.
   */
  sampleRegionCss?: { x: number; y: number; width: number; height: number };
}

declare global {
  interface Window {
    __donnerCompositedProbe?: {
      running: boolean;
      samples: CompositedSample[];
      drawFailures: number;
      frames: number;
      start: () => void;
      stop: () => void;
    };
  }
}

/**
 * Install the probe. Idempotent: re-installing replaces the configuration and
 * clears any previously collected samples.
 *
 * Installation does not start sampling; `startCompositedProbe` does. Keeping
 * them separate lets a test install before the gesture setup (document load,
 * pointer parking) and sample only the window it means to assert about,
 * instead of drowning the interesting frames in load-time ones.
 */
export async function installCompositedProbe(
  page: Page,
  options: CompositedProbeOptions = {},
): Promise<void> {
  const config = {
    sampleWidth: options.sampleWidth ?? 64,
    sampleHeight: options.sampleHeight ?? 48,
    minColorAlpha: options.minColorAlpha ?? 16,
    minColorSpread: options.minColorSpread ?? 12,
    sampleRegionCss: options.sampleRegionCss ?? null,
  };

  await page.evaluate((config) => {
    const readback = document.createElement("canvas");
    readback.width = config.sampleWidth;
    readback.height = config.sampleHeight;
    const context = readback.getContext("2d", { willReadFrequently: true });
    if (context === null) {
      throw new Error("composited probe: no 2d context for read-back");
    }

    // The editor's other diagnostics are declared as `Window` members by the
    // specs that own them. Read them through a local view instead, so this file
    // stays independent of whichever spec happens to declare them today.
    const diagnostics = window as unknown as {
      __donnerWorkerStats?: { completedResults?: number };
    };

    const sampleOnce = (): CompositedSample => {
      const surface = document.querySelector<HTMLCanvasElement>("canvas#canvas");
      const completedResults = diagnostics.__donnerWorkerStats?.completedResults || 0;
      if (surface === null) {
        return {
          t: performance.now(),
          canvasPresent: false,
          visibleX: 0,
          visibleY: 0,
          cssWidth: 0,
          cssHeight: 0,
          backingWidth: 0,
          backingHeight: 0,
          completedResults,
          meanAlpha: 0,
          meanLuma: 0,
          coloredPixels: 0,
          coloredCentroidX: -1,
          coloredCentroidY: -1,
          coloredWidth: 0,
          coloredHeight: 0,
          sampledPixels: 0,
          drawOk: false,
          attempts: 1,
        };
      }

      const elementBox = surface.getBoundingClientRect();
      let bounds = {
        x: elementBox.left,
        y: elementBox.top,
        width: elementBox.width,
        height: elementBox.height,
        insetLeft: 0,
        insetTop: 0,
      };
      if (config.sampleRegionCss !== null) {
        // Intersect the requested window with the canvas box, keeping the
        // offsets expressed relative to that box so the source rect below
        // stays correct.
        const wanted = config.sampleRegionCss;
        const left = Math.max(bounds.x, wanted.x);
        const top = Math.max(bounds.y, wanted.y);
        const right = Math.min(bounds.x + bounds.width, wanted.x + wanted.width);
        const bottom = Math.min(bounds.y + bounds.height, wanted.y + wanted.height);
        bounds = {
          x: left,
          y: top,
          width: Math.max(0, right - left),
          height: Math.max(0, bottom - top),
          insetLeft: bounds.insetLeft + (left - bounds.x),
          insetTop: bounds.insetTop + (top - bounds.y),
        };
      }
      const elementRect = surface.getBoundingClientRect();
      // Backing pixels per CSS pixel of the ELEMENT box. The visible region is
      // a sub-rectangle of that box, so its source rect scales the same way.
      const scaleX = elementRect.width > 0 ? surface.width / elementRect.width : 0;
      const scaleY = elementRect.height > 0 ? surface.height / elementRect.height : 0;
      const sourceX = bounds.insetLeft * scaleX;
      const sourceY = bounds.insetTop * scaleY;
      const sourceWidth = bounds.width * scaleX;
      const sourceHeight = bounds.height * scaleY;

      const base = {
        t: performance.now(),
        canvasPresent: true,
        visibleX: bounds.x,
        visibleY: bounds.y,
        cssWidth: bounds.width,
        cssHeight: bounds.height,
        backingWidth: surface.width,
        backingHeight: surface.height,
        completedResults,
      };

      if (!(sourceWidth >= 1) || !(sourceHeight >= 1)) {
        return {
          ...base,
          meanAlpha: 0,
          meanLuma: 0,
          coloredPixels: 0,
          coloredCentroidX: -1,
          coloredCentroidY: -1,
          coloredWidth: 0,
          coloredHeight: 0,
          sampledPixels: 0,
          drawOk: false,
          attempts: 1,
        };
      }

      context.clearRect(0, 0, readback.width, readback.height);
      try {
        context.drawImage(
          surface,
          sourceX,
          sourceY,
          sourceWidth,
          sourceHeight,
          0,
          0,
          readback.width,
          readback.height,
        );
      } catch {
        return {
          ...base,
          meanAlpha: 0,
          meanLuma: 0,
          coloredPixels: 0,
          coloredCentroidX: -1,
          coloredCentroidY: -1,
          coloredWidth: 0,
          coloredHeight: 0,
          sampledPixels: 0,
          drawOk: false,
          attempts: 1,
        };
      }

      const pixels = context.getImageData(0, 0, readback.width, readback.height).data;
      const count = pixels.length / 4;
      let alphaSum = 0;
      let lumaSum = 0;
      let colored = 0;
      let coloredX = 0;
      let coloredY = 0;
      let minX = Number.POSITIVE_INFINITY;
      let maxX = Number.NEGATIVE_INFINITY;
      let minY = Number.POSITIVE_INFINITY;
      let maxY = Number.NEGATIVE_INFINITY;
      for (let index = 0; index < pixels.length; index += 4) {
        const r = pixels[index];
        const g = pixels[index + 1];
        const b = pixels[index + 2];
        const a = pixels[index + 3];
        alphaSum += a;
        lumaSum += 0.2126 * r + 0.7152 * g + 0.0722 * b;
        if (a >= config.minColorAlpha) {
          const spread = Math.max(r, g, b) - Math.min(r, g, b);
          if (spread >= config.minColorSpread) {
            colored += 1;
            const pixel = index / 4;
            const x = pixel % readback.width;
            const y = Math.floor(pixel / readback.width);
            coloredX += x;
            coloredY += y;
            minX = Math.min(minX, x);
            maxX = Math.max(maxX, x);
            minY = Math.min(minY, y);
            maxY = Math.max(maxY, y);
          }
        }
      }

      return {
        ...base,
        meanAlpha: alphaSum / count,
        meanLuma: lumaSum / count,
        coloredPixels: colored,
        coloredCentroidX: colored === 0 ? -1 : coloredX / colored,
        coloredCentroidY: colored === 0 ? -1 : coloredY / colored,
        coloredWidth: colored === 0 ? 0 : maxX - minX + 1,
        coloredHeight: colored === 0 ? 0 : maxY - minY + 1,
        sampledPixels: count,
        drawOk: true,
        attempts: 1,
      };
    };

    const probe = {
      running: false,
      samples: [] as CompositedSample[],
      drawFailures: 0,
      frames: 0,
      readbackRetries: 0,
      readbackRescues: 0,
      seenContent: false,
      start(): void {
        probe.samples.length = 0;
        probe.drawFailures = 0;
        probe.frames = 0;
        probe.readbackRetries = 0;
        probe.readbackRescues = 0;
        probe.seenContent = false;
        probe.running = true;
        const tick = (): void => {
          if (!probe.running) {
            return;
          }
          probe.frames += 1;
          let sample = sampleOnce();
          let attempts = 1;
          // Same-task retry for Gecko's empty first read of a just-presented
          // WebGPU canvas. Only after the window has seen content: a blank
          // boot must stay observable, and a genuinely cleared canvas answers
          // empty on every attempt.
          if (probe.seenContent && sample.drawOk && sample.meanAlpha < 40) {
            probe.readbackRetries += 1;
            while (attempts < 4) {
              const again = sampleOnce();
              attempts += 1;
              if (again.drawOk && again.meanAlpha >= 40) {
                sample = again;
                probe.readbackRescues += 1;
                break;
              }
            }
          }
          sample.attempts = attempts;
          if (!sample.drawOk) {
            probe.drawFailures += 1;
          }
          if (sample.drawOk && sample.coloredPixels > 0) {
            probe.seenContent = true;
          }
          probe.samples.push(sample);
          requestAnimationFrame(tick);
        };
        requestAnimationFrame(tick);
      },
      stop(): void {
        probe.running = false;
      },
    };
    window.__donnerCompositedProbe = probe;
  }, config);
}

/** Begin sampling. Clears any samples from a previous window. */
export async function startCompositedProbe(page: Page): Promise<void> {
  await page.evaluate(() => {
    const probe = window.__donnerCompositedProbe;
    if (probe === undefined) {
      throw new Error("composited probe: install before start");
    }
    probe.start();
  });
}

/** Stop sampling and return the collected window. */
export async function stopCompositedProbe(page: Page): Promise<CompositedProbeResult> {
  return page.evaluate(() => {
    const probe = window.__donnerCompositedProbe;
    if (probe === undefined) {
      throw new Error("composited probe: install before stop");
    }
    probe.stop();
    return {
      samples: probe.samples.slice(),
      drawFailures: probe.drawFailures,
      frames: probe.frames,
      readbackRetries: probe.readbackRetries,
      readbackRescues: probe.readbackRescues,
    };
  });
}

/** Black-frame accounting over a sampled window. */
export interface BlackFrameStats {
  /** Samples whose visible region was effectively empty. */
  blackSamples: number;
  /** Samples considered (those with a usable read-back). */
  consideredSamples: number;
  /** `blackSamples / consideredSamples`, or 0 when nothing was considered. */
  fraction: number;
  /** Longest consecutive run of black samples. */
  longestRun: number;
  /** Index of the first sample in the longest run, or -1. */
  longestRunStart: number;
  /**
   * The black samples themselves, so a failure names the frames it is talking
   * about. A bare fraction is not diagnosable: whether the empty frames sit on
   * an epoch boundary, on a backing-store resize, or in the middle of a stable
   * epoch is the whole difference between a read-back race and a real clear.
   */
  blackSampleIndices: number[];
}

/**
 * A frame is "black" here when the visible region carries almost no coverage.
 *
 * Mean alpha is the discriminator rather than luma because the defect this
 * guards is a CLEARED backing store, which reads back fully transparent, not
 * dark. A dark-but-present document (the Splash artboard has large dark
 * regions) keeps a high mean alpha and is correctly not counted.
 */
export function blackFrameStats(
  samples: readonly CompositedSample[],
  meanAlphaThreshold = 40,
): BlackFrameStats {
  let blackSamples = 0;
  let considered = 0;
  let longestRun = 0;
  let longestRunStart = -1;
  let run = 0;
  let runStart = -1;
  const blackSampleIndices: number[] = [];
  samples.forEach((sample, index) => {
    if (!sample.drawOk) {
      run = 0;
      runStart = -1;
      return;
    }
    considered += 1;
    if (sample.meanAlpha < meanAlphaThreshold) {
      blackSamples += 1;
      blackSampleIndices.push(index);
      if (run === 0) {
        runStart = index;
      }
      run += 1;
      if (run > longestRun) {
        longestRun = run;
        longestRunStart = runStart;
      }
    } else {
      run = 0;
      runStart = -1;
    }
  });
  return {
    blackSamples,
    consideredSamples: considered,
    fraction: considered === 0 ? 0 : blackSamples / considered,
    longestRun,
    longestRunStart,
    blackSampleIndices,
  };
}

/** Distinct backing-store sizes observed, in order of first appearance. */
export function backingSizeTransitions(
  samples: readonly CompositedSample[],
): Array<{ sampleIndex: number; width: number; height: number }> {
  const transitions: Array<{ sampleIndex: number; width: number; height: number }> = [];
  for (let index = 0; index < samples.length; ++index) {
    const sample = samples[index];
    if (sample.backingWidth === 0 && sample.backingHeight === 0) {
      continue;
    }
    const last = transitions[transitions.length - 1];
    if (
      last === undefined || last.width !== sample.backingWidth
      || last.height !== sample.backingHeight
    ) {
      transitions.push({
        sampleIndex: index,
        width: sample.backingWidth,
        height: sample.backingHeight,
      });
    }
  }
  return transitions;
}

/** One frame in which the presented content moved against the gesture. */
export interface DragRegression {
  /** Index of the sample whose content position regressed. */
  sampleIndex: number;
  /** Presented centroid movement between the two samples, read-back px. */
  presentedDx: number;
  presentedDy: number;
  /** Pointer movement over the same interval, CSS px. */
  pointerDx: number;
  pointerDy: number;
}

/**
 * Find frames in which the presented content moved AGAINST the drag.
 *
 * This is the decidable form of "the shape pops back to a previous position".
 *
 * A pop-back cannot be tested as "the centroid decreased", because a real drag
 * reverses direction and the content is supposed to follow it. It also cannot
 * be tested as "the centroid did not move", because a stall is a throughput
 * problem, not a correctness one. What is never correct is the presented
 * position moving opposite to where the pointer went over the same interval:
 * that is an older drag position reaching the screen after a newer one.
 *
 * The pointer trace supplies the direction. For each consecutive sample pair,
 * the pointer displacement over `[previous.t, current.t]` gives the gesture's
 * instantaneous direction; a presented displacement whose projection onto that
 * direction is negative by more than `toleranceReadbackPx` is a regression.
 * Intervals in which the pointer barely moved are skipped, because a direction
 * derived from sub-pixel motion is noise.
 *
 * Note the units differ on purpose: the presented displacement is in read-back
 * pixels and the pointer displacement is in CSS pixels. Only the SIGN of the
 * projection is used, so the scale factor between them cannot change the
 * verdict; the tolerance is applied to the presented magnitude alone.
 */
export function dragRegressions(
  samples: readonly CompositedSample[],
  pointerTrace: ReadonlyArray<readonly [number, number, number]>,
  toleranceReadbackPx = 1.0,
  minPointerDeltaCssPx = 2.0,
): DragRegression[] {
  const pointerAt = (t: number): { x: number; y: number } | null => {
    let best: readonly [number, number, number] | null = null;
    for (const point of pointerTrace) {
      if (point[0] > t) {
        break;
      }
      best = point;
    }
    return best === null ? null : { x: best[1], y: best[2] };
  };

  const regressions: DragRegression[] = [];
  let previous: CompositedSample | undefined;
  for (const [index, sample] of samples.entries()) {
    if (sample.coloredCentroidX < 0 || !sample.drawOk) {
      continue;
    }
    if (previous !== undefined) {
      const before = pointerAt(previous.t);
      const after = pointerAt(sample.t);
      if (before !== null && after !== null) {
        const pointerDx = after.x - before.x;
        const pointerDy = after.y - before.y;
        const pointerLength = Math.hypot(pointerDx, pointerDy);
        if (pointerLength >= minPointerDeltaCssPx) {
          const presentedDx = sample.coloredCentroidX - previous.coloredCentroidX;
          const presentedDy = sample.coloredCentroidY - previous.coloredCentroidY;
          const projection = (presentedDx * pointerDx + presentedDy * pointerDy) / pointerLength;
          if (projection < -toleranceReadbackPx) {
            regressions.push({
              sampleIndex: index,
              presentedDx,
              presentedDy,
              pointerDx,
              pointerDy,
            });
          }
        }
      }
    }
    previous = sample;
  }
  return regressions;
}

/**
 * Fraction of sampled frames in which the presented content's centroid moved.
 *
 * With one canvas this is the motion observable for every gesture, pan and
 * zoom included: the canvas element fills the window and never moves, so the
 * only place motion is visible is in the pixels.
 */
export function contentMotionFraction(
  samples: readonly CompositedSample[],
  minDeltaReadbackPx = 0.25,
): { movedSamples: number; comparedSamples: number; fraction: number } {
  let moved = 0;
  let compared = 0;
  let previous: CompositedSample | undefined;
  for (const sample of samples) {
    if (sample.coloredCentroidX < 0 || !sample.drawOk) {
      continue;
    }
    if (previous !== undefined) {
      compared += 1;
      if (
        Math.abs(sample.coloredCentroidX - previous.coloredCentroidX) > minDeltaReadbackPx
        || Math.abs(sample.coloredCentroidY - previous.coloredCentroidY) > minDeltaReadbackPx
      ) {
        moved += 1;
      }
    }
    previous = sample;
  }
  return {
    movedSamples: moved,
    comparedSamples: compared,
    fraction: compared === 0 ? 0 : moved / compared,
  };
}

/**
 * Read the width of the document's chromatic content, in read-back pixels,
 * without the probe running.
 *
 * The single-canvas replacement for the old visible-surface width. There is no
 * element whose box tracks the zoom any more: the canvas is the window. What
 * still tracks it is the content, so a zoom is measured as how far apart the
 * document's own chromatic pixels are.
 *
 * Reads through the same `drawImage` path and the same chromatic-pixel
 * predicate the probe uses, so a ratio taken before and after a gesture is
 * directly comparable to `CompositedSample.coloredWidth`.
 */
export async function readContentWidth(
  page: Page,
  options: { sampleWidth?: number; sampleHeight?: number; minColorAlpha?: number; minColorSpread?: number } = {},
): Promise<number> {
  const config = {
    sampleWidth: options.sampleWidth ?? 256,
    sampleHeight: options.sampleHeight ?? 192,
    minColorAlpha: options.minColorAlpha ?? 16,
    minColorSpread: options.minColorSpread ?? 12,
  };
  return page.evaluate((config) => {
    const surface = document.querySelector<HTMLCanvasElement>("canvas#canvas");
    if (surface === null) {
      return 0;
    }
    const readback = document.createElement("canvas");
    readback.width = config.sampleWidth;
    readback.height = config.sampleHeight;
    const context = readback.getContext("2d", { willReadFrequently: true });
    if (context === null) {
      return 0;
    }
    try {
      context.drawImage(surface, 0, 0, readback.width, readback.height);
    } catch {
      return 0;
    }
    const pixels = context.getImageData(0, 0, readback.width, readback.height).data;
    let minX = Number.POSITIVE_INFINITY;
    let maxX = Number.NEGATIVE_INFINITY;
    for (let index = 0; index < pixels.length; index += 4) {
      if (pixels[index + 3] < config.minColorAlpha) {
        continue;
      }
      const r = pixels[index];
      const g = pixels[index + 1];
      const b = pixels[index + 2];
      if (Math.max(r, g, b) - Math.min(r, g, b) < config.minColorSpread) {
        continue;
      }
      const x = (index / 4) % readback.width;
      minX = Math.min(minX, x);
      maxX = Math.max(maxX, x);
    }
    return maxX < minX ? 0 : maxX - minX + 1;
  }, config);
}
