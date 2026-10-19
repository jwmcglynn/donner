// The fixture pulse is expanded far enough for the pixel oracle between 100 and 500 ms after the
// click is dispatched.
const kPulseWindow = { minimumElapsed: 0.1, maximumElapsed: 0.5 };
/** The fixture's click pulse lasts 600 ms of document time. */
const kPulseDurationSeconds = 0.6;

/**
 * Choose the next action of a pulse probe whose raster worker is suspended at a held task yield.
 *
 * `rendered` lists the document time of every frame the raster worker has rasterized, in order.
 * While its yield stays held the worker cannot rasterize another frame, so the newest entry is
 * the last frame that can reach presentation. The editor samples the timeline only after the
 * previous frame is presented, so a later admitted sample proves that frame was presented.
 *
 * Returns `{action: "step"}` while the newest frame precedes the expanded pulse,
 * `{action: "await-presentation"}` while an expanded frame awaits its presentation proof,
 * `{action: "capture"}` once that frame is the presented one, or a terminal `"overshoot"` or
 * `"invalid"` action when no expanded frame can be retained for this click. A `null` target
 * accepts the first changed click on any element.
 */
export function gatedPulseFrame(rendered, snapshot, generation, target, notBeforeEpochMs) {
  if (!Array.isArray(rendered) || !rendered.every(Number.isFinite)) {
    return { action: "invalid", reason: "malformed raster render trace" };
  }
  if (snapshot?.generation !== generation || !Array.isArray(snapshot.entries)) {
    return { action: "invalid", reason: "stale pointer trace" };
  }
  const rows = snapshot.entries;
  const clickIndex = findClick(rows, target, notBeforeEpochMs);
  if (clickIndex < 0) return { action: "step" };
  const click = rows[clickIndex];
  const samples = samplesAfterClick(rows, clickIndex);
  if (samples.reason) return { action: "invalid", reason: samples.reason };
  return classifyRenderedFrame(click, rendered.at(-1), samples.times);
}

// Timeline sample times admitted after the click, or the reason the trace cannot be trusted.
function samplesAfterClick(rows, clickIndex) {
  const times = [];
  let previousTime = rows[clickIndex][4];
  for (const row of rows.slice(clickIndex + 1)) {
    if (!Array.isArray(row) || row.length !== 6) return { reason: "malformed pointer trace" };
    if (row[1] === 2) return { reason: "another event was dispatched" };
    if (row[1] !== 3) continue;
    if (!Number.isFinite(row[4]) || row[4] < previousTime) {
      return { reason: "non-finite or reversed timeline sample" };
    }
    previousTime = row[4];
    times.push(row[4]);
  }
  return { times };
}

function classifyRenderedFrame(click, renderedTime, sampleTimes) {
  const dispatchTime = click[4];
  if (renderedTime === undefined || renderedTime - dispatchTime < kPulseWindow.minimumElapsed) {
    return { action: "step" };
  }
  const observation = {
    dispatchEpochMs: click[0],
    dispatchTarget: click[3],
    dispatchTime,
    renderedTime,
  };
  if (renderedTime - dispatchTime > kPulseWindow.maximumElapsed) {
    return { action: "overshoot", ...observation };
  }
  const proofTime = sampleTimes.find((time) => time > renderedTime);
  if (proofTime === undefined) return { action: "await-presentation", ...observation };
  return { action: "capture", ...observation, proofTime };
}

function findClick(rows, target, notBeforeEpochMs) {
  return rows.findIndex((row) =>
    Array.isArray(row) && row.length === 6
    && Number.isFinite(row[0]) && row[0] >= notBeforeEpochMs
    && row[1] === 2 && row[2] === 0 && (target === null || row[3] === target)
    && Number.isFinite(row[4]) && row[5] === true
  );
}

/** A later admitted tick proves a frame after the 600 ms pulse reached presentation. */
export function presentedPulseCompletion(snapshot, generation, target, notBeforeEpochMs) {
  return presentedSampleAfterClick(
    snapshot,
    generation,
    target,
    notBeforeEpochMs,
    kPulseDurationSeconds,
    Infinity,
  );
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
  const clickIndex = findClick(rows, target, notBeforeEpochMs);
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

/**
 * Whether a click queued behind a raster stall was dispatched on the caught-up preview clock: at
 * least one pulse after the held stale frame, rather than at that frame's document time.
 */
export function queuedDispatchCaughtUp(heldRenderedTimes, dispatchTime) {
  if (
    !Array.isArray(heldRenderedTimes) || heldRenderedTimes.length === 0
    || !heldRenderedTimes.every(Number.isFinite) || !Number.isFinite(dispatchTime)
  ) {
    return { caughtUp: false, reason: "no held frame time" };
  }
  const heldTime = heldRenderedTimes.at(-1);
  return { caughtUp: dispatchTime - heldTime >= kPulseDurationSeconds, heldTime, dispatchTime };
}
