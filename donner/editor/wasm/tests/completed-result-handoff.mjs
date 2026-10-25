/** Provisional bound for the fixed nominal carousel workload, above its 400-560ms healthy range. */
export const kNominalCompletedResultHandoffLimitMs = 1000;

/** Measure the raw completed-result handoff; retained GPU backlog is not subtracted. */
export function completedResultHandoff(completedAtMs, presentedAtMs) {
  const handoffMs = presentedAtMs - completedAtMs;
  return {
    handoffMs,
    limitMs: kNominalCompletedResultHandoffLimitMs,
    withinLimit: Number.isFinite(completedAtMs) && Number.isFinite(presentedAtMs)
      && handoffMs >= 0 && handoffMs <= kNominalCompletedResultHandoffLimitMs,
  };
}
