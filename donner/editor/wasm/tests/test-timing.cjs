// Shared timing constants for the browser suites and their Playwright configs.
//
// Correctness tests here assert ordering and causality: frame, serial and event counts, not how
// many milliseconds something took. A wait for the browser or the editor worker to reach a state
// is a stall guard, not a latency budget. It is long enough never to bind while the editor is
// still making progress on a slow or loaded host, and short enough that a real hang fails the
// case with a message instead of running into the per-test timeout. Latency budgets belong in
// the performance lane (`browser-responsiveness.perf.ts`).
//
// This file is CommonJS so the Playwright configs, which are CommonJS, can load it with
// `require`; the specs load it through `test-timing.ts`.

/**
 * How long a wait for the browser or editor to reach a state may run before the case fails.
 * Matches the C++ stall guard (`kHangGuard` in donner/base/tests/StallGuard.h).
 */
const kStallGuardMs = 30_000;

/**
 * Multiplier applied to the suites' remaining wall-clock bounds when `CI` is set. Shared CI
 * runners execute the suites several times slower than local development hardware.
 *
 * Retained only until each remaining `scaledMs` call site becomes a count-based assertion or a
 * `kStallGuardMs` wait. Do not add new users: a correctness check should not depend on how fast
 * the host is.
 */
const kCiTimeScale = process.env.CI ? 4 : 1;

/**
 * Scales a wall-clock bound by `kCiTimeScale`.
 *
 * @param {number} ms Bound on local development hardware, in milliseconds.
 * @returns {number} The bound to use on this host.
 */
function scaledMs(ms) {
  return ms * kCiTimeScale;
}

/**
 * The stall guard for web-first assertions in a config whose cases time out after
 * `testTimeoutMs`: `kStallGuardMs`, capped at two thirds of the per-test timeout so a stalled
 * assertion fails with its own message before the test's timeout ends the case without one.
 *
 * @param {number} testTimeoutMs The config's per-test timeout, in milliseconds.
 * @returns {number} The `expect.timeout` for that config, in milliseconds.
 */
function stallGuardFor(testTimeoutMs) {
  return Math.min(kStallGuardMs, Math.floor((testTimeoutMs * 2) / 3));
}

module.exports = { kCiTimeScale, kStallGuardMs, scaledMs, stallGuardFor };
