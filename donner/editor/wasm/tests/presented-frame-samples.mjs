/**
 * Observing a held drag through the editor's own per-frame presentation statistics.
 *
 * The composited probe reads the worker-owned WebGPU canvas back on the main thread every frame.
 * In Firefox on the hosted macOS Perf job that sustained readback stalled both the page and the
 * canvas worker in about half the runs, while a single readback or none never did (#1668). There
 * the held-drag case observes, instead, what the editor publishes after each presented frame in
 * `window.__donnerPresentationQueueStats`: how many submitted frames have completed, and which
 * pointer position the completed frame represents. Sampling those reads no pixels.
 *
 * Plain JavaScript with no module-only features beyond `export`, so Playwright can load it from a
 * TypeScript spec.
 */

/**
 * Summarize presented-frame samples taken while a held drag streamed its inputs.
 *
 * @param {Array<{t: number, completedSerial: number, pointerX: number, pointerY: number,
 *   inputRepresented: boolean}>} samples Per-animation-frame samples, in time order.
 * @param {{firstInputAt: number, lastInputAt: number, start: {x: number, y: number},
 *   end: {x: number, y: number}}} stream When the inputs were dispatched, and where the drag
 *   started and ended.
 * @returns {{activeSamples: number, completedFrames: number, representedTravel: number,
 *   travelFraction: number}}
 */
export function presentedDragSummary(samples, stream) {
  const active = samples.filter((sample) =>
    sample.t >= stream.firstInputAt && sample.t <= stream.lastInputAt
  );
  const serials = active.map((sample) => sample.completedSerial).filter(Number.isFinite);
  const completedFrames = serials.length === 0 ? 0 : Math.max(...serials) - Math.min(...serials);
  const dragLength = Math.hypot(stream.end.x - stream.start.x, stream.end.y - stream.start.y);
  let representedTravel = 0;
  for (const sample of active) {
    if (!sample.inputRepresented) continue;
    // Progress along the drag direction, so a represented position behind the start or beside the
    // path does not count as travel.
    const along = ((sample.pointerX - stream.start.x) * (stream.end.x - stream.start.x)
      + (sample.pointerY - stream.start.y) * (stream.end.y - stream.start.y))
      / Math.max(dragLength, 1e-9);
    representedTravel = Math.max(representedTravel, along);
  }
  return {
    activeSamples: active.length,
    completedFrames,
    representedTravel,
    travelFraction: dragLength > 0 ? representedTravel / dragLength : 0,
  };
}
