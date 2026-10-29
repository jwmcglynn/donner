// The capture gate for screenshots that score presented editor canvas pixels.
//
// A page capture can show a canvas frame whose submitted GPU work has not run yet, and waiting for
// submitted work before the screenshot is not enough while the editor keeps presenting: a newer
// frame can arrive during the screenshot. So each pass holds the app's canvas completions (its
// presentation queue then stops admitting frames after at most three), waits on the browser queue
// until every canvas write has run, and accepts the screenshot only if no canvas frame was
// acquired or written from that wait until the screenshot ended.
//
// The gate bounds how long it holds the app's completions, never how long a capture may take: its
// only time bound is the caller's stall guard. A screenshot too slow to finish under a hold is
// retaken without one (unless the caller keeps every screenshot held) and accepted under the same
// canvas-activity check. A hold under which frames keep arriving is released and armed again.
//
// This module is plain logic over an injected probe, so its policy is tested without a browser.

/**
 * Upper bound on screenshot passes under one completion hold. While the hold is in place the app's
 * held frames never retire, so at most three new frames are admitted and every rejected pass saw
 * at least one of them; a few frames submitted before the hold can also still retire and free
 * slots, so seven passes should suffice. When they do not, the gate records the hold as exhausted,
 * with the activity it saw, and holds again.
 */
export const kMaxCanvasQuiescencePasses = 7;

/**
 * Longest a capture holds the app's canvas completions at once, from arming the hold to the end of
 * the screenshot. Once the app's oldest frame has waited 5 s
 * (`PresentationSubmissionQueue::kCompletionDeadline`), the app stops presenting until that frame's
 * completion arrives, so a hold must end well before that. This bounds the hold only; it is not a deadline for the capture.
 */
export const kMaxCanvasCompletionHoldMs = 3500;

/** Part of the hold the settle wait may use, leaving the rest for the screenshot. */
export const kMaxCanvasSettleHoldMs = 2000;

/** Shortest screenshot timeout worth starting under a hold; less than this holds again. */
export const kMinHeldScreenshotMs = 250;

/**
 * Part of the hold kept back from the screenshot for the worker round trips that still run under
 * it: the caller's own reads around its screenshot, the activity read and the release.
 */
export const kHeldRoundTripReserveMs = 500;

/**
 * Longest the gate waits for the canvas owner to release a hold. A release that does not answer
 * fails the capture instead of leaving the app's completions held without notice.
 */
export const kHoldReleaseBoundMs = 1000;

/**
 * Race \p work against \p untilMs on \p now's clock; null when the bound passes first. A rejection
 * of abandoned work is ignored, since it can arrive after the caller has moved on.
 * @template T
 * @param {Promise<T>} work
 * @param {number} untilMs
 * @param {() => number} now
 * @returns {Promise<T | null>}
 */
async function raceUntil(work, untilMs, now) {
  let timer;
  const expired = new Promise((resolve) => {
    timer = setTimeout(() => resolve(null), Math.max(0, untilMs - now()));
  });
  try {
    return await Promise.race([work, expired]);
  } finally {
    clearTimeout(timer);
    work.catch(() => {});
  }
}

/**
 * Take one screenshot of a quiescent editor canvas.
 *
 * @template T
 * @param {{
 *   hold: () => Promise<{ release: () => Promise<void> }>,
 *   settle: () => Promise<{ canvasSubmits: number, frames: number }>,
 *   activity: () => Promise<{ canvasSubmits: number, frames: number }>,
 * }} probe The canvas owner: hold its completions, wait until the GPU has run every canvas write
 *   (resolving with the activity counters at that point), and read the counters.
 * @param {(timeoutMs: number) => Promise<T>} capture Takes one screenshot within `timeoutMs`.
 * @param {{
 *   untilMs: number,
 *   now?: () => number,
 *   timedOut?: (result: T) => boolean,
 *   isTimeoutError?: (error: unknown) => boolean,
 *   retakeUnheld?: boolean,
 *   passes?: Array<Record<string, unknown>>,
 * }} options `untilMs` is the capture's time bound on `now`'s clock (default `performance.now`),
 *   normally the stall guard. `timedOut` recognizes a capture result that ran out of time, and
 *   `isTimeoutError` a thrown one; any other failure of `capture` or of the probe propagates.
 *   `retakeUnheld: false` keeps every screenshot under a hold, for a canvas that never stops
 *   presenting. Each pass is appended to `passes` when given, so a caller keeps them on failure.
 * @returns {Promise<{ result: T, passes: Array<Record<string, unknown>> }>} The accepted capture
 *   and a record of every pass.
 */
export async function captureQuiescentCanvas(probe, capture, options) {
  const gate = {
    probe,
    capture,
    now: options.now ?? (() => performance.now()),
    timedOut: options.timedOut ?? (() => false),
    isTimeoutError: options.isTimeoutError ?? (() => false),
    untilMs: options.untilMs,
    passes: options.passes ?? [],
  };
  const retakeUnheld = options.retakeUnheld ?? true;
  let unheld = false;
  for (;;) {
    // A held pass needs room for its shortest screenshot before the time bound.
    if (gate.now() >= gate.untilMs - (unheld ? 0 : kMinHeldScreenshotMs)) {
      throw stalledError(gate.passes);
    }
    const attempt = unheld ? await captureUnheld(gate) : await captureUnderHold(gate);
    if (attempt.outcome === "accepted") return { result: attempt.result, passes: gate.passes };
    // A held screenshot that outlasted its hold is retaken without one; every other outcome, and
    // every unheld retake, holds again.
    unheld = !unheld && retakeUnheld && attempt.outcome === "screenshot";
  }
}

/** The failure once the time bound leaves no room for another pass. */
function stalledError(passes) {
  const exhausted = passes.filter((pass) => pass.holdExhausted).length;
  return new Error(
    `no quiescent canvas screenshot within the capture's time bound`
      + (exhausted > 0
        ? ` (the canvas kept presenting new frames under ${exhausted} completion holds)`
        : "")
      + `: ${JSON.stringify(passes.slice(-8))}`,
  );
}

/**
 * One pass: settle, screenshot, compare activity. Its outcome is "accepted", "activity" (a canvas
 * frame arrived meanwhile), or the stage that ran out of time: "settle" or "screenshot".
 */
async function runPass(gate, record, bounds) {
  const { probe, now } = gate;
  record.startedEpochMs = Date.now();
  gate.passes.push(record);
  let settled;
  try {
    settled = await raceUntil(probe.settle(), bounds.settleUntilMs, now);
  } catch (error) {
    // Without a hold, canvas writes can keep arriving for longer than the settle waits. The next
    // attempt holds again, and a failure that is not about new frames recurs there.
    if (record.held) throw error;
    record.settleError = String(error);
    return { outcome: "activity" };
  }
  if (settled === null) return { outcome: "settle" };
  record.settled = settled;
  record.settledEpochMs = Date.now();
  const timeoutMs = bounds.captureUntilMs - now();
  if (timeoutMs < bounds.minCaptureMs) return { outcome: "settle" };
  record.captureTimeoutMs = Math.round(timeoutMs);
  let result;
  try {
    result = await gate.capture(timeoutMs);
  } catch (error) {
    if (!gate.isTimeoutError(error)) throw error;
    return { outcome: "screenshot" };
  }
  record.screenshotFinishedEpochMs = Date.now();
  if (gate.timedOut(result)) return { outcome: "screenshot" };
  // Bounded like the rest of the pass: an owner that stops answering ends this hold, and a read
  // that answers only after the bound is never used to accept the screenshot.
  const after = await raceUntil(probe.activity(), bounds.activityUntilMs, now);
  if (after === null) {
    record.activityUnanswered = true;
    return { outcome: "settle" };
  }
  record.after = after;
  if (after.canvasSubmits !== settled.canvasSubmits || after.frames !== settled.frames) {
    return { outcome: "activity" };
  }
  return { outcome: "accepted", result };
}

/** Retake the screenshot without a hold, after a held one outlasted its hold. */
function captureUnheld(gate) {
  const { untilMs } = gate;
  return runPass(gate, { held: false }, {
    settleUntilMs: untilMs,
    captureUntilMs: untilMs,
    minCaptureMs: 1,
    activityUntilMs: untilMs,
  });
}

/** Release \p hold within kHoldReleaseBoundMs, or fail. */
async function releaseHold(gate, hold) {
  const released = await raceUntil(
    hold.release().then(() => true),
    gate.now() + kHoldReleaseBoundMs,
    gate.now,
  );
  if (released === null) {
    throw new Error("the canvas owner did not release the capture's completion hold");
  }
}

/**
 * Up to kMaxCanvasQuiescencePasses passes under one hold of the app's canvas completions, released
 * on every path. A release failure is recorded, but never replaces a pass's own failure.
 */
async function captureUnderHold(gate) {
  const { now, untilMs, passes } = gate;
  const heldAtMs = now();
  const passesBeforeHold = passes.length;
  const hold = await gate.probe.hold();
  const bounds = {
    settleUntilMs: Math.min(untilMs, heldAtMs + kMaxCanvasSettleHoldMs),
    captureUntilMs: Math.min(
      untilMs,
      heldAtMs + kMaxCanvasCompletionHoldMs - kHeldRoundTripReserveMs,
    ),
    minCaptureMs: kMinHeldScreenshotMs,
    activityUntilMs: Math.min(untilMs, heldAtMs + kMaxCanvasCompletionHoldMs),
  };
  let attempt = { outcome: "activity" };
  try {
    for (let index = 1; index <= kMaxCanvasQuiescencePasses; ++index) {
      attempt = await runPass(gate, { held: true, pass: index }, bounds);
      if (attempt.outcome !== "activity") break;
    }
  } catch (error) {
    await releaseHold(gate, hold).catch((releaseError) => {
      passes.push({ holdReleaseError: String(releaseError) });
    });
    throw error;
  }
  await releaseHold(gate, hold);
  if (attempt.outcome === "activity") {
    // More frames arrived under this hold than it can admit. Hold again; the time bound still
    // ends a canvas that never stops presenting, and no screenshot is accepted meanwhile.
    recordExhaustedHold(gate, passesBeforeHold, heldAtMs);
  }
  return attempt;
}

/**
 * Record what an exhausted hold admitted, so a recurrence shows whether the three-frame admission
 * model holds.
 */
function recordExhaustedHold(gate, passesBeforeHold, heldAtMs) {
  const held = gate.passes.slice(passesBeforeHold).filter((pass) => pass.settled && pass.after);
  const first = held[0];
  const last = held.at(-1);
  gate.passes.push({
    holdExhausted: true,
    heldMs: Math.round(gate.now() - heldAtMs),
    framesAdmitted: last && first ? last.after.frames - first.settled.frames : null,
    canvasSubmitsAdmitted: last && first
      ? last.after.canvasSubmits - first.settled.canvasSubmits
      : null,
  });
}
