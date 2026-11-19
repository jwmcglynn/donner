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

/**
 * Tiles a result rasterized other than by a damage patch.
 * @param {{ cachedTileCount: number, immediateTileCount: number, damagePatchTileCount: number }}
 *   result Recorded result.
 * @returns {number}
 */
export function unpatchedTileCount(result) {
  return result.cachedTileCount + result.immediateTileCount - result.damagePatchTileCount;
}

/**
 * How many held results took the masked-child fast path.
 * @param {{ held: Array<{ fastPathFrames: number }> }} summary Summary from
 *   `summarizeMaskedDragResults`.
 * @returns {number}
 */
export function heldFastPathResultCount(summary) {
  return summary.held.filter((result) => result.fastPathFrames > 0).length;
}

/** The counters the held result shape reads; each must be a finite number. */
const kShapeCounters = [
  "slowPathFramesWithDirty",
  "fastPathFrames",
  "noDirtyFrames",
  "damagePatchTileCount",
  "cachedTileCount",
  "immediateTileCount",
];

/**
 * What breaks the held masked-child result shape, one line per failure; empty when it holds.
 * @param {{ countersPublished: boolean, held: object[] }} summary Summary from
 *   `summarizeMaskedDragResults`.
 * @returns {string[]}
 */
export function heldMaskedDragShapeFailures(summary) {
  if (!summary.countersPublished) return ["the candidate bundle must publish per-result counters"];
  const failures = [];
  if (heldFastPathResultCount(summary) === 0) {
    failures.push("the held drag must produce fast-path results");
  }
  summary.held.forEach((result, index) => {
    const label = `held result ${index} (completed result ${result.completedResults})`;
    // A counter the editor renamed or stopped publishing reads as NaN or null: name it instead of
    // comparing it.
    const unreadable = kShapeCounters.filter((name) => !Number.isFinite(result[name]));
    if (unreadable.length > 0) {
      failures.push(`${label}: unreadable counters ${unreadable.join(", ")}`);
      return;
    }
    const problems = [];
    if (result.slowPathFramesWithDirty !== 0) {
      problems.push(`${result.slowPathFramesWithDirty} slow-path frames`);
    }
    if (!(result.fastPathFrames + result.noDirtyFrames > 0)) {
      problems.push("neither the fast path nor a clean frame");
    }
    if (!(result.damagePatchTileCount <= 1)) {
      problems.push(`${result.damagePatchTileCount} damage-patched tiles`);
    }
    if (unpatchedTileCount(result) !== 0) {
      problems.push(`${unpatchedTileCount(result)} tiles rasterized without a damage patch`);
    }
    if (problems.length > 0) failures.push(`${label}: ${problems.join(", ")}`);
  });
  return failures;
}
