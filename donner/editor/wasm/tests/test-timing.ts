import { expect } from "@playwright/test";
import { kCiTimeScale, kStallGuardMs, scaledMs, stallGuardPollOptions } from "./test-timing.cjs";

// The constants live in test-timing.cjs so the CommonJS Playwright configs share them; see there.
export { kCiTimeScale, kStallGuardMs, scaledMs };

/**
 * Polls `fn` until the chained matcher holds, under the shared stall guard.
 *
 * Use it to wait for an observable state, such as a frame counter advancing or a worker going
 * idle, instead of sleeping or picking a deadline:
 *
 * ```ts
 * await waitForState(() => readPresentedFrames(page), "a frame presents after the drag")
 *   .toBeGreaterThan(before);
 * ```
 *
 * @param fn Reads the current state; may be async.
 * @param label What the wait is for, reported if the stall guard expires.
 */
export function waitForState<T>(fn: () => T | Promise<T>, label: string) {
  return expect.poll(fn, stallGuardPollOptions(label));
}
