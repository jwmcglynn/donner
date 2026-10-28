/**
 * Startup phase marks for browser cases that open the editor.
 *
 * A case that ends at its deadline is reported by the wait it was in when the deadline arrived.
 * When the time went somewhere else, that report names the wrong thing: a case that spent all but
 * a second of its budget before it ever navigated still fails "waiting for the first frame",
 * because that is the first wait the remaining second could not cover. These marks record when
 * each startup phase began, on the Node clock in epoch milliseconds, so the startup waits can name
 * a pre-navigation stall as one and the failure evidence can show where the budget went.
 *
 * Diagnostics only: recording a mark is one clock read, and nothing here waits or decides whether
 * a case passes.
 *
 * Plain JavaScript with no module-only features beyond `export`, so Playwright can load it from a
 * TypeScript spec and Node's test runner can load it directly.
 */

/** Startup phases in the order a case passes through them. */
export const kStartupPhases = Object.freeze([
  // The case's first beforeEach hook, before any test-scoped fixture (the page) is set up.
  "testStart",
  // The case body reached openEditor, so its fixtures were ready.
  "openEditor",
  "gotoStart",
  "gotoEnd",
  "wasmStartAllowed",
  "firstFrameWaitStart",
  "firstFramePresented",
]);

/**
 * Share of the case's budget that may pass before navigation before the startup waits name the
 * stall. Every observed run reached navigation within a few seconds of the 30 s budget.
 */
export const kPreNavigationStallShare = 0.5;

const seconds = (ms) => `${(ms / 1000).toFixed(1)} s`;

/**
 * Where a case's startup time went, from its phase marks.
 *
 * @param {Record<string, number>} marks Phase name to epoch milliseconds.
 * @param {number} budgetMs The case's timeout.
 * @param {{navigationTimeOrigin?: number | null, now?: number}} [options] The page's
 *   `performance.timeOrigin` when the page answered, and the time of the report.
 * @returns {{budgetMs: number, sinceTestStartMs: Record<string, number>,
 *   navigationTimeOriginMs: number | null, preNavigationMs: number | null,
 *   preNavigationStall: boolean, reportedAtMs: number | null}}
 */
export function startupPhaseReport(marks, budgetMs, options = {}) {
  const start = marks.testStart;
  const since = (time) =>
    Number.isFinite(start) && Number.isFinite(time) ? Math.round((time - start) * 10) / 10 : null;
  const sinceTestStartMs = {};
  for (const phase of kStartupPhases) {
    const value = since(marks[phase]);
    if (value !== null) sinceTestStartMs[phase] = value;
  }
  const reportedAtMs = since(options.now ?? Date.now());
  // A case that never navigated spent everything up to the report before navigation.
  const preNavigationMs = sinceTestStartMs.gotoStart ?? reportedAtMs;
  return {
    budgetMs,
    sinceTestStartMs,
    navigationTimeOriginMs: since(options.navigationTimeOrigin ?? null),
    preNavigationMs,
    preNavigationStall: preNavigationMs !== null
      && preNavigationMs >= budgetMs * kPreNavigationStallShare,
    reportedAtMs,
  };
}

/**
 * The sentence that names a pre-navigation stall, or null when navigation started in time.
 *
 * @param {ReturnType<typeof startupPhaseReport>} report
 * @returns {string | null}
 */
export function describePreNavigationStall(report) {
  if (!report.preNavigationStall) return null;
  const { sinceTestStartMs: phases } = report;
  const split = phases.openEditor === undefined
    ? "openEditor was never reached"
    : `test start to openEditor ${seconds(phases.openEditor)}`
      + (phases.gotoStart === undefined
        ? ", navigation never started"
        : `, openEditor to navigation ${seconds(phases.gotoStart - phases.openEditor)}`);
  return `pre-navigation stall: ${seconds(report.preNavigationMs)} of the`
    + ` ${seconds(report.budgetMs)} test budget passed before navigation started (${split})`;
}

/**
 * A startup wait's message: the wait's own message, led by the pre-navigation stall when one
 * consumed the budget the wait is about to run in.
 *
 * @param {Record<string, number>} marks Phase name to epoch milliseconds.
 * @param {number} budgetMs The case's timeout.
 * @param {string} waitMessage What the wait expects.
 * @param {number} [now] When the wait starts, epoch milliseconds.
 * @returns {string}
 */
export function startupWaitMessage(marks, budgetMs, waitMessage, now = Date.now()) {
  const report = startupPhaseReport(marks, budgetMs, { now });
  const stall = describePreNavigationStall(report);
  if (stall === null || report.reportedAtMs === null) return waitMessage;
  return `${stall}; ${seconds(Math.max(0, budgetMs - report.reportedAtMs))} of the budget was`
    + ` left when this wait started: ${waitMessage}`;
}
