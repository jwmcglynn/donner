export function resourceQueuesAreQuiescent(workers, framesInFlight) {
  return workers.length > 0 && framesInFlight === 0
    && workers.every((owner) =>
      !owner.unavailable && Number.isFinite(owner.ageMs) && owner.ageMs >= 0 && owner.ageMs < 5000
    ) && workers.some((owner) => owner.gpu)
    && workers.every((owner) => !owner.gpu || owner.gpu.pendingSubmissions === 0);
}
