/**
 * Whether every accepted thumbnail request has reached a terminal state and no lane slot is live.
 * `drained` is deliberately excluded: the editor publishes it only while the picker is visible.
 */
export function sampleThumbnailLaneQuiescent(stats) {
  const serial = sampleThumbnailLaneSerial(stats);
  return serial !== null && serial.requested === serial.completed
    && stats.pending === false && stats.active === false && stats.resultReady === false;
}

/** Monotonic accepted/completed identity used to restart a continuous readiness window. */
export function sampleThumbnailLaneSerial(stats) {
  if (!stats) return null;
  const requested = stats.requested;
  const completed = stats.completed;
  if (
    !Number.isFinite(requested) || requested < 0
    || !Number.isFinite(completed) || completed < 0
  ) {
    return null;
  }
  return { requested, completed };
}
