/** Hold zero-delay owner tasks only after the fixture has drained input and finished rendering. */
export function installOwnerEventTimerHold() {
  const scope = globalThis;
  if (!scope.__donnerGpuOwner || scope.__donnerHeldEventTimers) {
    throw new Error("the GPU owner must accept exactly one event-task hold");
  }
  const originalSet = scope.setTimeout;
  const originalClear = scope.clearTimeout;
  const held = new Map();
  let nextHandle = 0x60000000;
  let requested = 0;
  scope.setTimeout = (callback, delay = 0, ...args) => {
    if (Number(delay) !== 0) return originalSet.call(scope, callback, delay, ...args);
    const handle = ++nextHandle;
    held.set(handle, { callback, args });
    ++requested;
    return handle;
  };
  scope.clearTimeout = (handle) => {
    if (!held.delete(handle)) originalClear.call(scope, handle);
  };
  scope.__donnerHeldEventTimers = {
    state: () => ({ requested, pending: held.size }),
    release: () => {
      scope.setTimeout = originalSet;
      scope.clearTimeout = originalClear;
      const flushed = held.size;
      for (const { callback, args } of held.values()) {
        originalSet.call(scope, callback, 0, ...args);
      }
      held.clear();
      return { requested, flushed };
    },
  };
  return true;
}

export function readOwnerEventTimerHold() {
  return globalThis.__donnerHeldEventTimers?.state() ?? null;
}

export function releaseOwnerEventTimerHold() {
  const hold = globalThis.__donnerHeldEventTimers;
  if (!hold) throw new Error("the owner event-task hold is not armed");
  const result = hold.release();
  delete globalThis.__donnerHeldEventTimers;
  return result;
}
