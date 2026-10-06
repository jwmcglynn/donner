/**
 * Observing a held drag through the editor's own per-frame presentation statistics.
 *
 * The composited probe reads the worker-owned WebGPU canvas back on the main thread every frame.
 * In Firefox on the hosted macOS Perf job that sustained readback stalled both the page and the
 * canvas worker in about half the runs, while a single readback or none never did (#1668). There
 * the held-drag case observes, instead, what the editor publishes when a submitted frame completes
 * in `window.__donnerPresentationQueueStats`: the completed frame's presentation frame id, and
 * which pointer position that frame represents. Sampling those reads no pixels.
 *
 * Frames are counted by their presentation frame id, which the render coordinator assigns from a
 * monotonically increasing counter, never by GPU submission serial: one editor frame completes
 * several submissions, so serials keep climbing while presentation is stalled.
 *
 * Plain JavaScript with no module-only features beyond `export`, so Playwright can load it from a
 * TypeScript spec.
 */

/** What the held-drag case requires of the frames completed while its 60 inputs stream. */
export const kHeldDragPresentationBounds = Object.freeze({
  /** Changes of the completed presentation frame id seen during the input stream. */
  presentedFrames: 12,
  /** Of those, frames that represent the pointer further along the drag than any before. */
  advancingFrames: 6,
  /** How far along the drag the furthest represented pointer reached, as a fraction of it. */
  travelFraction: 0.15,
});

/**
 * Summarize presented-frame samples taken while a held drag streamed its inputs.
 *
 * @param {Array<{t: number, frameId: number, pointerX: number, pointerY: number,
 *   inputRepresented: boolean}>} samples Per-animation-frame samples, in time order.
 * @param {{firstInputAt: number, lastInputAt: number, start: {x: number, y: number},
 *   end: {x: number, y: number}}} stream When the inputs were dispatched, and where the drag
 *   started and ended.
 * @returns {{activeSamples: number, presentedFrames: number, advancingFrames: number,
 *   representedTravel: number, travelFraction: number}}
 */
export function presentedDragSummary(samples, stream) {
  const active = samples.filter((sample) =>
    sample.t >= stream.firstInputAt && sample.t <= stream.lastInputAt
  );
  const dx = stream.end.x - stream.start.x;
  const dy = stream.end.y - stream.start.y;
  const dragLength = Math.hypot(dx, dy);
  // Progress along the drag direction, so a represented position behind the start or beside the
  // path does not count as travel.
  const along = (sample) =>
    ((sample.pointerX - stream.start.x) * dx + (sample.pointerY - stream.start.y) * dy)
    / Math.max(dragLength, 1e-9);

  let presentedFrames = 0;
  let advancingFrames = 0;
  let representedTravel = 0;
  let previousFrameId;
  for (const sample of active) {
    if (!Number.isFinite(sample.frameId)) continue;
    const travel = sample.inputRepresented ? along(sample) : 0;
    if (previousFrameId !== undefined && sample.frameId !== previousFrameId) {
      presentedFrames += 1;
      if (travel > representedTravel) advancingFrames += 1;
    }
    representedTravel = Math.max(representedTravel, travel);
    previousFrameId = sample.frameId;
  }
  return {
    activeSamples: active.length,
    presentedFrames,
    advancingFrames,
    representedTravel,
    travelFraction: dragLength > 0 ? representedTravel / dragLength : 0,
  };
}

/**
 * The bounds a summary misses, in the order the bounds list them.
 *
 * @param {ReturnType<typeof presentedDragSummary>} summary
 * @param {typeof kHeldDragPresentationBounds} bounds
 * @returns {Array<{metric: string, value: number, minimum: number}>}
 */
export function presentedDragFailures(summary, bounds) {
  return Object.entries(bounds)
    .filter(([metric, minimum]) => !(summary[metric] >= minimum))
    .map(([metric, minimum]) => ({ metric, value: summary[metric], minimum }));
}
