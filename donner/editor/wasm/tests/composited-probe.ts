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
 *    count) taken from the VISIBLE region of the visible worker surface,
 *  - `data-direct-surface-frame` of that surface, which names the epoch whose
 *    pixels are being shown,
 *  - the visible region's CSS geometry, which is the epoch the *layout*
 *    believes it is showing,
 *  - the canvas backing store size, which is what a per-epoch resize clears,
 *  - `window.__donnerAcceptedPresentation.token` and
 *    `window.__donnerWorkerStats.completedResults`, which order the samples
 *    against the worker's own progress.
 *
 * Pixels plus epoch token plus CSS size plus backing size, per frame, is what
 * makes the invariants in `composited-invariants.spec.ts` decidable. Any one of
 * them alone is ambiguous: geometry alone cannot see a cleared backing store,
 * pixels alone cannot tell a legitimately empty region from a dropped epoch,
 * and a settled check cannot see either.
 *
 * WHY drawImage AND NOT A SCREENSHOT
 *
 * `page.screenshot()` does not capture the worker-owned WebGPU canvas in
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
  /** Element id of the worker surface that was visible, or "" if none was. */
  surfaceId: string;
  /** `data-direct-surface-frame` of the visible surface: the presented epoch. */
  frameToken: number;
  /** Visible-region origin in CSS px (element box minus clip-path insets). */
  visibleX: number;
  visibleY: number;
  /** Visible-region size in CSS px. This is the zoom observable. */
  cssWidth: number;
  cssHeight: number;
  /** Canvas backing store size in device px. */
  backingWidth: number;
  backingHeight: number;
  /** `window.__donnerAcceptedPresentation.token`. */
  acceptedToken: number;
  /** `window.__donnerWorkerStats.completedResults`. */
  completedResults: number;
  /** Mean alpha over the sampled region, 0-255. */
  meanAlpha: number;
  /** Mean luma over the sampled region, 0-255, premultiplied by nothing. */
  meanLuma: number;
  /** Pixels that are opaque enough and chromatic enough to be content. */
  coloredPixels: number;
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
  };

  await page.evaluate((config) => {
    const readback = document.createElement("canvas");
    readback.width = config.sampleWidth;
    readback.height = config.sampleHeight;
    const context = readback.getContext("2d", { willReadFrequently: true });
    if (context === null) {
      throw new Error("composited probe: no 2d context for read-back");
    }

    // Reimplemented here rather than imported from a spec: the probe must stay
    // usable when the specs it was extracted from are being edited, and a
    // shared helper that lives in a test file is a coupling that breaks the
    // moment someone reorganizes that file. The rule it encodes is CSS's:
    // `inset()` serializes with 1 to 4 values and expands as
    // top / right / bottom / left, with omitted values mirroring.
    const visibleBounds = (
      el: HTMLCanvasElement,
    ): { x: number; y: number; width: number; height: number; insetLeft: number; insetTop: number } => {
      const rect = el.getBoundingClientRect();
      const clip = getComputedStyle(el).clipPath || "";
      const match = clip.match(/inset\(([^)]*)\)/);
      if (match === null) {
        return {
          x: rect.left,
          y: rect.top,
          width: rect.width,
          height: rect.height,
          insetLeft: 0,
          insetTop: 0,
        };
      }
      const parts = match[1].trim().split(/\s+/).map((part) => parseFloat(part));
      if (parts.length === 0 || parts.some((value) => !Number.isFinite(value))) {
        return {
          x: rect.left,
          y: rect.top,
          width: rect.width,
          height: rect.height,
          insetLeft: 0,
          insetTop: 0,
        };
      }
      const top = parts[0];
      const right = parts.length >= 2 ? parts[1] : parts[0];
      const bottom = parts.length >= 3 ? parts[2] : parts[0];
      const left = parts.length >= 4 ? parts[3] : right;
      return {
        x: rect.left + left,
        y: rect.top + top,
        width: rect.width - left - right,
        height: rect.height - top - bottom,
        insetLeft: left,
        insetTop: top,
      };
    };

    // The editor's other diagnostics are declared as `Window` members by the
    // specs that own them. Read them through a local view instead, so this file
    // stays independent of whichever spec happens to declare them today.
    const diagnostics = window as unknown as {
      __donnerAcceptedPresentation?: { token?: number };
      __donnerWorkerStats?: { completedResults?: number };
    };

    const sampleOnce = (): CompositedSample => {
      const surface = document.querySelector<HTMLCanvasElement>(
        "canvas[data-direct-surface-visible=\"true\"]",
      );
      const acceptedToken = diagnostics.__donnerAcceptedPresentation?.token || 0;
      const completedResults = diagnostics.__donnerWorkerStats?.completedResults || 0;
      if (surface === null) {
        return {
          t: performance.now(),
          surfaceId: "",
          frameToken: 0,
          visibleX: 0,
          visibleY: 0,
          cssWidth: 0,
          cssHeight: 0,
          backingWidth: 0,
          backingHeight: 0,
          acceptedToken,
          completedResults,
          meanAlpha: 0,
          meanLuma: 0,
          coloredPixels: 0,
          sampledPixels: 0,
          drawOk: false,
        };
      }

      const bounds = visibleBounds(surface);
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
        surfaceId: surface.id,
        frameToken: Number(surface.dataset.directSurfaceFrame || 0),
        visibleX: bounds.x,
        visibleY: bounds.y,
        cssWidth: bounds.width,
        cssHeight: bounds.height,
        backingWidth: surface.width,
        backingHeight: surface.height,
        acceptedToken,
        completedResults,
      };

      if (!(sourceWidth >= 1) || !(sourceHeight >= 1)) {
        return {
          ...base,
          meanAlpha: 0,
          meanLuma: 0,
          coloredPixels: 0,
          sampledPixels: 0,
          drawOk: false,
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
          sampledPixels: 0,
          drawOk: false,
        };
      }

      const pixels = context.getImageData(0, 0, readback.width, readback.height).data;
      const count = pixels.length / 4;
      let alphaSum = 0;
      let lumaSum = 0;
      let colored = 0;
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
          }
        }
      }

      return {
        ...base,
        meanAlpha: alphaSum / count,
        meanLuma: lumaSum / count,
        coloredPixels: colored,
        sampledPixels: count,
        drawOk: true,
      };
    };

    const probe = {
      running: false,
      samples: [] as CompositedSample[],
      drawFailures: 0,
      frames: 0,
      start(): void {
        probe.samples.length = 0;
        probe.drawFailures = 0;
        probe.frames = 0;
        probe.running = true;
        const tick = (): void => {
          if (!probe.running) {
            return;
          }
          probe.frames += 1;
          const sample = sampleOnce();
          if (!sample.drawOk) {
            probe.drawFailures += 1;
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

/** One epoch-atomicity violation: pixels and layout disagreed for a window. */
export interface EpochAtomicityViolation {
  /** Index of the sample where the presented epoch token changed. */
  sampleIndex: number;
  /** Epoch token before and after the change. */
  previousFrameToken: number;
  frameToken: number;
  /** Visible CSS size at the token change (still the previous epoch's size). */
  cssWidthAtChange: number;
  cssHeightAtChange: number;
  /** Index of the later sample where the CSS size finally caught up. */
  cssCatchUpIndex: number;
  /** Visible CSS size once it caught up. */
  cssWidthAfter: number;
  cssHeightAfter: number;
  /** Frames the mismatch was on screen. */
  mismatchedFrames: number;
}

/**
 * Find frames where the presented epoch and its CSS geometry were not applied
 * together.
 *
 * A direct WebGPU present commits worker-side immediately; the CSS layout that
 * matches those pixels is applied when the main thread accepts the epoch, one
 * or more task boundaries later. If the two are not atomic, there is a window
 * where epoch N+1 pixels are on screen under epoch N geometry, which is the
 * wrong-scale flicker.
 *
 * The decidable form: when the presented frame token changes at sample `i`,
 * the visible CSS size at `i` must already be the size that epoch presents. If
 * the CSS size instead changes at some LATER sample `j > i` with no further
 * token change in between, samples `i..j-1` showed the new epoch at the old
 * scale. A size change in the SAME sample as the token change is exactly the
 * atomic case and is not a violation.
 */
export function epochAtomicityViolations(
  samples: readonly CompositedSample[],
  sizeToleranceCssPx = 0.5,
): EpochAtomicityViolation[] {
  const violations: EpochAtomicityViolation[] = [];
  const sizeChanged = (a: CompositedSample, b: CompositedSample): boolean =>
    Math.abs(a.cssWidth - b.cssWidth) > sizeToleranceCssPx
    || Math.abs(a.cssHeight - b.cssHeight) > sizeToleranceCssPx;

  for (let index = 1; index < samples.length; ++index) {
    const previous = samples[index - 1];
    const current = samples[index];
    if (current.frameToken === previous.frameToken || current.frameToken === 0) {
      continue;
    }
    if (sizeChanged(previous, current)) {
      // Pixels and geometry moved in the same frame: atomic.
      continue;
    }
    // The token moved without the geometry. That is only a violation if the
    // geometry for this same epoch arrives later; if it never changes, this
    // epoch genuinely presents at the same scale (a pan, or a re-raster at an
    // unchanged zoom) and nothing was ever mismatched.
    for (let ahead = index + 1; ahead < samples.length; ++ahead) {
      const later = samples[ahead];
      if (later.frameToken !== current.frameToken) {
        break;
      }
      if (sizeChanged(current, later)) {
        violations.push({
          sampleIndex: index,
          previousFrameToken: previous.frameToken,
          frameToken: current.frameToken,
          cssWidthAtChange: current.cssWidth,
          cssHeightAtChange: current.cssHeight,
          cssCatchUpIndex: ahead,
          cssWidthAfter: later.cssWidth,
          cssHeightAfter: later.cssHeight,
          mismatchedFrames: ahead - index,
        });
        break;
      }
    }
  }
  return violations;
}

/** One epoch that presented on the same DOM canvas as the epoch before it. */
export interface SurfaceAlternationViolation {
  /** Index of the sample where the presented epoch token changed. */
  sampleIndex: number;
  /** Epoch token before and after the change. */
  previousFrameToken: number;
  frameToken: number;
  /** The DOM canvas both epochs presented on. */
  surfaceId: string;
}

/**
 * Find consecutive epochs that presented on the same DOM canvas.
 *
 * This is the pixel-side half of the wrong-scale flicker, expressed as
 * something the DOM can decide.
 *
 * A direct WebGPU present commits worker-side, immediately, into whichever
 * canvas the worker holds. The CSS layout matching those pixels is applied
 * only when the main thread accepts the epoch, a task boundary later. If the
 * worker presents into the canvas that is currently on screen, every epoch of
 * a gesture has a window where epoch N+1 pixels are composited under epoch N
 * geometry: the document flickers between two scales, and no attribute on the
 * page ever disagrees, because the DOM is self-consistently still epoch N.
 *
 * With two canvases presented into alternately, the epoch being drawn is never
 * the epoch being displayed, so acceptance can flip visibility and geometry in
 * one style flush. The observable consequence, and the assertion here: the
 * visible surface must change canvas on every epoch change.
 *
 * Samples with no visible surface, and repeats of the same epoch, are skipped;
 * only a token change is evidence about where the *next* present landed.
 */
export function surfaceAlternationViolations(
  samples: readonly CompositedSample[],
): SurfaceAlternationViolation[] {
  const violations: SurfaceAlternationViolation[] = [];
  let previous: CompositedSample | undefined;
  for (const sample of samples) {
    if (sample.surfaceId === "" || sample.frameToken === 0) {
      continue;
    }
    if (previous !== undefined && sample.frameToken !== previous.frameToken) {
      if (sample.surfaceId === previous.surfaceId) {
        violations.push({
          sampleIndex: samples.indexOf(sample),
          previousFrameToken: previous.frameToken,
          frameToken: sample.frameToken,
          surfaceId: sample.surfaceId,
        });
      }
    }
    previous = sample;
  }
  return violations;
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

/**
 * Fraction of sampled frames in which the visible region's origin moved.
 *
 * This is the pan observable. A pan that only updates when the worker
 * publishes a new epoch moves in a handful of frames out of hundreds; a pan
 * that tracks the gesture moves in most of them.
 */
export function motionFraction(
  samples: readonly CompositedSample[],
  minDeltaCssPx = 0.25,
): { movedSamples: number; comparedSamples: number; fraction: number } {
  let moved = 0;
  let compared = 0;
  for (let index = 1; index < samples.length; ++index) {
    const previous = samples[index - 1];
    const current = samples[index];
    if (previous.surfaceId === "" || current.surfaceId === "") {
      continue;
    }
    compared += 1;
    if (
      Math.abs(current.visibleX - previous.visibleX) > minDeltaCssPx
      || Math.abs(current.visibleY - previous.visibleY) > minDeltaCssPx
    ) {
      moved += 1;
    }
  }
  return {
    movedSamples: moved,
    comparedSamples: compared,
    fraction: compared === 0 ? 0 : moved / compared,
  };
}

/**
 * Read the visible width of the currently visible worker surface, without the
 * probe running.
 *
 * The surface element spans its cap-sized backing store, so the element box
 * barely changes with zoom. The zoom observable is the VISIBLE width: the
 * element box minus its clip-path insets, which tracks the content raster.
 */
export async function readVisibleSurfaceWidth(page: Page): Promise<number> {
  return page.evaluate(() => {
    const el = document.querySelector<HTMLCanvasElement>(
      "canvas[data-direct-surface-visible=\"true\"]",
    );
    if (el === null) {
      return 0;
    }
    const rect = el.getBoundingClientRect();
    const match = (getComputedStyle(el).clipPath || "").match(/inset\(([^)]*)\)/);
    if (match === null) {
      return rect.width;
    }
    const parts = match[1].trim().split(/\s+/).map((part) => parseFloat(part));
    if (parts.length === 0 || parts.some((value) => !Number.isFinite(value))) {
      return rect.width;
    }
    const right = parts.length >= 2 ? parts[1] : parts[0];
    const left = parts.length >= 4 ? parts[3] : right;
    return rect.width - left - right;
  });
}
