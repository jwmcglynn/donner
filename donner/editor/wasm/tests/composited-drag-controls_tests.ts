import { describe, test } from "node:test";
import { readFileSync } from "node:fs";
import path from "node:path";
import { expect, type Page } from "@playwright/test";
import {
  attachCompositedReadbacks,
  type CompositedSample,
  dragRegressions,
  installCompositedPixelClassifier,
  installCompositedProbe,
  startCompositedProbe,
} from "./composited-probe";
import { confirmBlueBodyRegressions, kOldOrderingCssPerSample, kOrderingDrag, orderingTrajectoryRegion } from "./drag-ordering";

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
describe("dragRegressions classifier", () => {
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

  test("a position chip entering the blue top edge is not an older frame", () => {
    // Measured from the retained Firefox CI frame pair. The chip first covered the top-left
    // blue pixels between samples, while the unoccluded filled body did not move backwards.
    const trace = Array.from(
      { length: 32 },
      (_, index) => [index * 10, 100 - index * 0.25, 200 - index * 0.25] as const,
    );
    const before = {
      ...sampleAt(200, 62.8753488372093),
      coloredCentroidY: 43.54,
      coloredPixels: 2150,
      coloredMinX: 40,
      coloredMinY: 21,
      coloredWidth: 47,
      coloredHeight: 46,
    };
    const after = {
      ...sampleAt(260, 63.26093514328809),
      coloredCentroidY: 45.52187028657617,
      coloredPixels: 1989,
      coloredMinX: 40,
      coloredMinY: 22,
      coloredWidth: 46,
      coloredHeight: 46,
    };
    const candidates = dragRegressions([before, after], trace, 1.0, 2.0, 150);
    expect(candidates.map((candidate) => candidate.sampleIndex)).toEqual([1]);
    const beforeBody = {
      ...before,
      coloredPixels: 1641,
      coloredCentroidX: 62.945,
      coloredCentroidY: 66,
      coloredMinY: 21,
      coloredWidth: 47,
    };
    const afterBody = {
      ...after,
      coloredPixels: 1649,
      coloredCentroidX: 62.407,
      coloredCentroidY: 67,
      coloredMinY: 22,
      coloredWidth: 46,
    };
    expect(confirmBlueBodyRegressions(candidates, [[beforeBody, afterBody]], trace, 150))
      .toEqual([]);

    // A rigid stale frame moves the unoccluded filled body as well. It must still fail at the
    // same one-pixel ordering tolerance even when the chip is present in another part of a frame.
    const stale = {
      ...after,
      coloredCentroidX: before.coloredCentroidX + 4,
      coloredCentroidY: before.coloredCentroidY + 4,
      coloredMinX: before.coloredMinX + 4,
      coloredMinY: before.coloredMinY + 4,
      coloredWidth: before.coloredWidth,
    };
    const staleBody = {
      ...beforeBody,
      t: after.t,
      coloredCentroidX: beforeBody.coloredCentroidX + 4,
      coloredCentroidY: beforeBody.coloredCentroidY + 4,
      coloredMinX: beforeBody.coloredMinX + 4,
      coloredMinY: beforeBody.coloredMinY + 4,
    };
    const staleCandidates = dragRegressions([before, stale], trace, 1.0, 2.0, 150);
    expect(
      confirmBlueBodyRegressions(staleCandidates, [[beforeBody, staleBody]], trace, 150)
        .map((candidate) => candidate.sampleIndex),
    ).toEqual([1]);
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
  const attached: string[] = [];
  const output = {
    outputPath: (file: string) => path.join(process.env.TEST_TMPDIR!, file),
    attach: async (name: string) => { attached.push(name); },
  };
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
    await expect(attachCompositedReadbacks(fakePage, output, [0])).rejects.toThrow(/Stop/);
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
    await expect(attachCompositedReadbacks(fakePage, output, [0])).rejects.toThrow(
      /memory budget/,
    );
    probe.rawReadbackOverflow = false;
    await expect(attachCompositedReadbacks(fakePage, output, [2])).rejects.toThrow(/Missing/);
    const retainedFirstFrame = probe.rawReadbacks[0];
    probe.rawReadbacks[0] = new Uint8ClampedArray([1, 2]);
    await expect(attachCompositedReadbacks(fakePage, output, [0])).rejects.toThrow(
      /dimensions/,
    );
    probe.rawReadbacks[0] = retainedFirstFrame;
    encodingAllowed = true;
    const evidence = await attachCompositedReadbacks(fakePage, output, [0, 1]);
    expect(attached).toEqual(["composited-frame-0", "composited-frame-1"]);
    expect(evidence.indices).toEqual([0, 1]);
    expect(evidence.bytes).toBe(png.length * 2);
    expect(encoded).toEqual([
      { data: new Uint8ClampedArray([255, 0, 0, 255]), width: 1, height: 1 },
      { data: new Uint8ClampedArray([0, 255, 0, 255]), width: 1, height: 1 },
    ]);
    for (const index of evidence.indices) {
      expect(readFileSync(output.outputPath(`composited-frame-${index}.png`))).toEqual(png);
    }
  } finally {
    for (const [name, descriptor] of previous) {
      if (descriptor) Object.defineProperty(globalThis, name, descriptor);
      else Reflect.deleteProperty(globalThis, name);
    }
  }
});
