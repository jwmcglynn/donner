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
// retaken without one and accepted under the same canvas-activity check.
//
// This module is plain logic over an injected probe, so its policy is tested without a browser.

/**
 * Upper bound on screenshot passes under one completion hold. While the hold is in place the app's
 * held frames never retire, so at most three new frames are admitted and every rejected pass saw
 * at least one of them; a few frames submitted before the hold can also still retire and free
 * slots, so seven is a conservative bound.
 */
export const kMaxCanvasQuiescencePasses = 7;

/**
 * Longest a capture holds the app's canvas completions at once, from arming the hold to the end of
 * the screenshot. The app declares its presentation device lost once its oldest frame has waited
 * 5 s without observed progress (`PresentationSubmissionQueue::kCompletionDeadline`), so a hold
 * must end well before that. This bounds the hold only; it is not a deadline for the capture.
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
  const now = options.now ?? (() => performance.now());
  const timedOut = options.timedOut ?? (() => false);
  const isTimeoutError = options.isTimeoutError ?? (() => false);
  const retakeUnheld = options.retakeUnheld ?? true;
  const { untilMs } = options;
  const passes = options.passes ?? [];
  const stalled = () =>
    new Error(
      `no quiescent canvas screenshot within the capture's time bound: ${
        JSON.stringify(passes.slice(-8))
      }`,
    );

  // One pass: settle, screenshot, compare activity. Its outcome is "accepted", "activity" (a
  // canvas frame arrived meanwhile), or the stage that ran out of time: "settle" or "screenshot".
  const pass = async (record, settleUntilMs, captureUntilMs, minCaptureMs) => {
    record.startedEpochMs = Date.now();
    passes.push(record);
    let settled;
    try {
      settled = await raceUntil(probe.settle(), settleUntilMs, now);
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
    const timeoutMs = captureUntilMs - now();
    if (timeoutMs < minCaptureMs) return { outcome: "settle" };
    record.captureTimeoutMs = Math.round(timeoutMs);
    let result;
    try {
      result = await capture(timeoutMs);
    } catch (error) {
      if (!isTimeoutError(error)) throw error;
      return { outcome: "screenshot" };
    }
    record.screenshotFinishedEpochMs = Date.now();
    if (timedOut(result)) return { outcome: "screenshot" };
    const after = await probe.activity();
    record.after = after;
    if (after.canvasSubmits !== settled.canvasSubmits || after.frames !== settled.frames) {
      return { outcome: "activity" };
    }
    return { outcome: "accepted", result };
  };

  let unheld = false;
  for (;;) {
    if (now() >= untilMs) throw stalled();
    if (unheld) {
      // The previous held screenshot outlasted the hold. Retake it without one; a canvas frame
      // during it still rejects it, and the next attempt holds again.
      unheld = false;
      const unheldPass = await pass({ held: false }, untilMs, untilMs, 1);
      if (unheldPass.outcome === "accepted") return { result: unheldPass.result, passes };
      continue;
    }
    const heldAtMs = now();
    const hold = await probe.hold();
    let outcome = "activity";
    let accepted;
    try {
      for (let index = 1; index <= kMaxCanvasQuiescencePasses; ++index) {
        const heldPass = await pass(
          { held: true, pass: index },
          Math.min(untilMs, heldAtMs + kMaxCanvasSettleHoldMs),
          Math.min(untilMs, heldAtMs + kMaxCanvasCompletionHoldMs - kHeldRoundTripReserveMs),
          kMinHeldScreenshotMs,
        );
        outcome = heldPass.outcome;
        if (outcome === "accepted") {
          accepted = heldPass.result;
          break;
        }
        if (outcome !== "activity") break;
      }
    } catch (error) {
      // Release on every path. A release failure is recorded, but never replaces the pass's own
      // failure.
      await hold.release().catch((releaseError) => {
        passes.push({ holdReleaseError: String(releaseError) });
      });
      throw error;
    }
    await hold.release();
    if (outcome === "accepted") return { result: accepted, passes };
    if (outcome === "activity") {
      throw new Error(
        `the canvas kept presenting new frames under the capture's completion hold: ${
          JSON.stringify(passes.slice(-kMaxCanvasQuiescencePasses))
        }`,
      );
    }
    // The GPU or the screenshot is slow enough that holding on would approach the app's own
    // completion deadline. The hold is released; hold again, or retake the screenshot unheld.
    unheld = retakeUnheld && outcome === "screenshot";
  }
}
