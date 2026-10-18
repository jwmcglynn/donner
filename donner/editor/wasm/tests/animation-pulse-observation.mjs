/** A later admitted sample proves the preceding pulse sample reached presentation. */
export function presentedPulseWindow(
  snapshot,
  generation,
  target,
  notBeforeEpochMs,
  maximumElapsed = 0.5,
) {
  return presentedSampleAfterClick(
    snapshot,
    generation,
    target,
    notBeforeEpochMs,
    0.1,
    maximumElapsed,
  );
}

/** A later admitted tick proves a frame after the 600 ms pulse reached presentation. */
export function presentedPulseCompletion(snapshot, generation, target, notBeforeEpochMs) {
  return presentedSampleAfterClick(snapshot, generation, target, notBeforeEpochMs, 0.6, Infinity);
}

function presentedSampleAfterClick(
  snapshot,
  generation,
  target,
  notBeforeEpochMs,
  minimumElapsed,
  maximumElapsed,
) {
  if (snapshot?.generation !== generation || !Array.isArray(snapshot.entries)) return null;
  const rows = snapshot.entries;
  const clickIndex = rows.findIndex((row) =>
    Array.isArray(row) && row.length === 6
    && Number.isFinite(row[0]) && row[0] >= notBeforeEpochMs
    && row[1] === 2 && row[2] === 0 && row[3] === target
    && Number.isFinite(row[4]) && row[5] === true
  );
  if (clickIndex < 0) return null;
  const click = rows[clickIndex];
  const samples = [];
  let previousTime = click[4];
  for (const row of rows.slice(clickIndex + 1)) {
    if (!Array.isArray(row) || row.length !== 6) return null;
    // Another dispatched event changes the observation's event identity.
    if (row[1] === 2) return null;
    if (row[1] !== 3) continue;
    if (!Number.isFinite(row[4]) || row[4] < previousTime) return null;
    previousTime = row[4];
    samples.push(row);
  }
  if (samples.length < 2) return null;
  const latest = samples.at(-1)[4];
  if (latest - click[4] > maximumElapsed) return null;
  for (let index = 0; index + 1 < samples.length; ++index) {
    const presented = samples[index][4];
    if (
      presented - click[4] >= minimumElapsed && presented - click[4] <= maximumElapsed
      && samples[index + 1][4] > presented
    ) {
      return {
        dispatchEpochMs: click[0],
        dispatchTime: click[4],
        presentedTime: presented,
        latestTime: latest,
      };
    }
  }
  return null;
}
