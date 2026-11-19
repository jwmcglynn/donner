import { type CompositedSample, type DragRegression, dragRegressions } from "./composited-probe";

interface Rect {
  x: number;
  y: number;
  width: number;
  height: number;
}

export const kOrderingDrag = {
  dx: -140,
  dy: -70,
  reversals: 3,
  reversalAmplitudePx: 90,
} as const;

export const kOldOrderingCssPerSample = { x: 320 / 96, y: 250 / 96 };

export function orderingTrajectoryRegion(
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

/**
 * Confirm a centroid-based ordering candidate using the filled body below the position chip.
 * The chip can cover the blue rectangle's top edge as it enters the artboard corner. Its first
 * appearance changes both the full blue centroid and the quantized top extent, even when the
 * unoccluded body holds its position. A genuine older frame moves the body's side and bottom
 * edges too. Missing or insufficient retained pixels keep the original violation fail-closed.
 */
export function confirmBlueBodyRegressions(
  candidates: readonly DragRegression[],
  bodies: ReadonlyArray<readonly [CompositedSample, CompositedSample] | null>,
  trace: ReadonlyArray<readonly [number, number, number]>,
  maxLatencyMs: number,
): DragRegression[] {
  return candidates.filter((_, index) => {
    const pair = bodies[index];
    return pair === null || dragRegressions(pair, trace, 1.0, 2.0, maxLatencyMs).length > 0;
  });
}
