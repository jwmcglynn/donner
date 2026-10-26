// Readiness of the GPU bridge's pipeline compiles, and a test-only fault that slows them.
//
// The bridge starts asynchronous pipeline compiles once the editor first builds its pipelines and
// reports how many are still in flight as `pipelineCompilesPending` in each context's
// `__donnerReadGpuObjectStats()`. A browser with a cold shader cache can take seconds to finish
// them, and can hold later GPU work behind them, so an interaction timed against a nominal bound
// starts only once that count has drained.

/**
 * Sums `pipelineCompilesPending` over the GPU statistics read from each browser context.
 *
 * @param {Array<Record<string, unknown> | null | undefined>} statistics One entry per context; a
 *   context without the GPU bridge reports null.
 * @returns {{ pending: number, reporting: number }} The total, and how many contexts reported it.
 *   Zero reporting contexts means the count is unknown, never that nothing is pending.
 */
export function sumPipelineCompilesPending(statistics) {
  let pending = 0;
  let reporting = 0;
  for (const entry of statistics) {
    const value = entry?.pipelineCompilesPending;
    if (typeof value !== "number" || !Number.isFinite(value)) continue;
    pending += value;
    reporting += 1;
  }
  return { pending, reporting };
}

/**
 * Runs in a worker before the editor script: every asynchronous pipeline compile settles no
 * earlier than `compileDelayMs` after it started, and each queue completion requested while such a
 * compile is outstanding is held until `completionDelayMs` has passed or no compile is outstanding,
 * whichever comes first. That models a browser with a cold shader cache, whose compiles outlast the
 * editor's first frame and hold later GPU work behind them, and releases every held completion once
 * the compiles drain, so nothing the fault holds outlives the work it models.
 *
 * Self-contained, so its source can be prepended to the editor script. Returns whether it installed;
 * it installs nothing outside a worker with WebGPU, and only once per context.
 *
 * @param {{ compileDelayMs: number, completionDelayMs: number }} config
 */
export function installPipelineCompileFault(config) {
  const scope = globalThis;
  if (
    typeof scope.WorkerGlobalScope === "undefined" || !(scope instanceof scope.WorkerGlobalScope)
    || typeof scope.GPUDevice === "undefined" || typeof scope.GPUQueue === "undefined"
    || scope.__donnerPipelineCompileFault
  ) {
    return false;
  }
  const setTimer = scope.setTimeout.bind(scope);
  const now = () => scope.performance.now();
  const state = {
    compileDelayMs: config.compileDelayMs,
    completionDelayMs: config.completionDelayMs,
    compilesDelayed: 0,
    compilesPending: 0,
    completionsDelayed: 0,
    lastCompileSettledAtMs: 0,
  };
  // Releases of the completions held now, run once no compile is outstanding.
  let drained = [];
  const at = (deadline) =>
    new Promise((resolve) => setTimer(resolve, Math.max(0, deadline - now())));
  const delayed = (result, deadline, after) =>
    result.then(
      (value) => at(deadline).then(() => (after(), value)),
      (error) =>
        at(deadline).then(() => {
          after();
          throw error;
        }),
    );
  for (const name of ["createRenderPipelineAsync", "createComputePipelineAsync"]) {
    const original = scope.GPUDevice.prototype[name];
    if (typeof original !== "function") continue;
    scope.GPUDevice.prototype[name] = function(descriptor) {
      const result = original.call(this, descriptor);
      state.compilesDelayed += 1;
      state.compilesPending += 1;
      return delayed(result, now() + state.compileDelayMs, () => {
        state.compilesPending -= 1;
        state.lastCompileSettledAtMs = now();
        if (state.compilesPending === 0) {
          const release = drained;
          drained = [];
          release.forEach((resolve) => resolve());
        }
      });
    };
  }
  for (const [owner, name] of [[scope.GPUQueue, "onSubmittedWorkDone"], [scope.GPUBuffer, "mapAsync"]]) {
    const original = owner?.prototype?.[name];
    if (typeof original !== "function") continue;
    owner.prototype[name] = function(...args) {
      const result = original.apply(this, args);
      if (state.compilesPending === 0) return result;
      state.completionsDelayed += 1;
      const released = new Promise((resolve) => {
        setTimer(resolve, state.completionDelayMs);
        drained.push(resolve);
      });
      return result.then(
        (value) => released.then(() => value),
        (error) =>
          released.then(() => {
            throw error;
          }),
      );
    };
  }
  scope.__donnerPipelineCompileFault = { state: () => ({ ...state }) };
  return true;
}

/** Reads the fault's counters in the calling context, or null where it is not installed. */
export function readPipelineCompileFault() {
  return globalThis.__donnerPipelineCompileFault?.state() ?? null;
}

/** The fault's source as a script prologue, with `config` bound. */
export function pipelineCompileFaultPrologue(config) {
  return `;(${installPipelineCompileFault.toString()})(${JSON.stringify(config)});\n`;
}
