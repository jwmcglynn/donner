// Splits the time from a click to the presentation of its result into what the GPU queue still had
// to finish from before the click and what came after, from the bridge's queue timeline.
//
// The timeline is what `__donnerReadGpuTimeline` returns, with every time converted to the page
// clock. A submission entry is ["s", device, serial, buffers, draws, submittedAt, completedAt],
// with completedAt 0 while it has not completed. A pipeline entry is
// ["p", device, createdAt, compiledAt, shared], with compiledAt 0 while its asynchronous compile
// has not settled (or when the browser had none), and a first-use entry ["c", device, at, entryPoint] records
// a pipeline created synchronously where a pass first needed it.

/**
 * @param {Array<Array<string|number>>} timeline Queue timeline on the page clock.
 * @param {{clickAt: number, completedAt: number, presentedAt: number}} times Page-clock times of
 *     the click, of the worker completing the clicked document's raster, and of the frame that
 *     presented it.
 * @returns {{
 *   presentedMs: number,
 *   resultCompletedMs: number,
 *   queueBacklogMs: number | null,
 *   handoffMs: number | null,
 *   submissionsAheadOfClick: number,
 *   pipelinesCompilingAtClick: number | null,
 *   firstUsePipelineCompiles: number,
 * }} Durations relative to the click. queueBacklogMs is how long after the click the GPU retired
 *     the last submission made before it; handoffMs is what presentation took once both that
 *     backlog and the result were done. Both are null when a submission made before the click had
 *     not completed by the time the timeline was read, so the backlog end is unknown.
 */
export function firstClickBreakdown(timeline, { clickAt, completedAt, presentedAt }) {
  let backlogEnd = clickAt;
  let backlogKnown = true;
  let submissionsAheadOfClick = 0;
  // Null when the timeline predates compile tracking, whose pipeline entries carry no compile time.
  let pipelinesCompilingAtClick = null;
  let firstUsePipelineCompiles = 0;
  for (const entry of timeline) {
    if (entry[0] === "s" && entry[5] <= clickAt) {
      const completed = Number(entry[6]);
      if (completed <= 0) {
        backlogKnown = false;
      } else if (completed > clickAt) {
        submissionsAheadOfClick += 1;
        backlogEnd = Math.max(backlogEnd, completed);
      }
    } else if (entry[0] === "p" && entry.length >= 5) {
      pipelinesCompilingAtClick ??= 0;
      const compiled = Number(entry[3]);
      const compiling = entry[2] <= clickAt && entry[4] !== 1
        && (compiled <= 0 || compiled > clickAt);
      if (compiling) pipelinesCompilingAtClick += 1;
    } else if (entry[0] === "c" && entry[2] <= presentedAt) {
      firstUsePipelineCompiles += 1;
    }
  }
  const round = (value) => Math.round(value * 10) / 10;
  const queueBacklogMs = backlogKnown ? round(backlogEnd - clickAt) : null;
  const ready = Math.max(completedAt, backlogEnd);
  return {
    presentedMs: round(presentedAt - clickAt),
    resultCompletedMs: round(completedAt - clickAt),
    queueBacklogMs,
    handoffMs: backlogKnown ? round(Math.max(0, presentedAt - ready)) : null,
    submissionsAheadOfClick,
    pipelinesCompilingAtClick,
    firstUsePipelineCompiles,
  };
}
