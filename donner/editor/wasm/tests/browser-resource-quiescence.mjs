export function resourceQueuesAreQuiescent(workers, framesInFlight) {
  const known = workers.filter((owner) => owner.gpu);
  return known.length > 0
    && known.every((owner) =>
      !owner.unavailable && (owner.ageMs ?? Infinity) < 5000 && owner.gpu.pendingSubmissions === 0
    ) && (framesInFlight ?? 0) === 0;
}
