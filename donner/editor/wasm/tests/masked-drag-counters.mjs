// Per-result compositor counters of the responsiveness lane's masked crown drag.
//
// The recorder copies each published worker result's counters. Bundles that predate them publish
// none, so a baseline run against such a bundle reports the counters as absent instead of treating
// every result as an idle one, and never checks them.

/**
 * Summarize the recorded worker results of one masked crown drag.
 * @param {Array<{ countersPublished: boolean, activeDragResult: boolean | null }>} records
 *   Recorded results, in publication order.
 * @param {{ baselineOnly: boolean }} options `baselineOnly` is true for a run against a historical
 *   bundle (`DONNER_BROWSER_BASELINE=1`), which only measures.
 * @returns {{ countersPublished: boolean, resultCount: number, held: object[], release: object[],
 *   checkHeldResults: boolean }} Whether every result published the counters, the results rendered
 *   for active drag requests, the release results published after the last of them, and whether
 *   the held results must be checked.
 */
export function summarizeMaskedDragResults(records, options) {
  const countersPublished = records.length > 0 && records.every((record) => record.countersPublished);
  const summary = {
    countersPublished,
    resultCount: records.length,
    held: [],
    release: [],
    checkHeldResults: !options.baselineOnly,
  };
  if (!countersPublished) return summary;
  const lastHeld = records.findLastIndex((record) => record.activeDragResult === true);
  summary.held = records.filter((record) => record.activeDragResult === true);
  summary.release = records.slice(lastHeld + 1);
  return summary;
}
