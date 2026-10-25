import assert from "node:assert/strict";
import test from "node:test";
import vm from "node:vm";
import { installOwnerEventTimerHold, readOwnerEventTimerHold,
  releaseOwnerEventTimerHold } from "./event-presentation-delay.mjs";

test("the presentation fault holds zero-delay tasks while preserving GPU progress pulses", () => {
  const scheduled = [];
  const scope = { __donnerGpuOwner: true,
    setTimeout: (callback, delay, ...args) => scheduled.push({ callback, delay, args }),
    clearTimeout: () => {}, };
  const context = vm.createContext({ globalThis: scope });
  const run = (fn) => vm.runInContext(`(${fn.toString()})()`, context);
  assert.equal(run(installOwnerEventTimerHold), true);
  let events = 0;
  const cancelled = scope.setTimeout(() => { throw new Error("cancelled task ran"); }, 0);
  scope.clearTimeout(cancelled);
  scope.setTimeout((increment) => { events += increment; }, 0, 2);
  scope.setTimeout(() => {}, 8);
  assert.deepEqual(scheduled.map(({ delay }) => delay), [8]);
  assert.equal(run(readOwnerEventTimerHold).pending, 1);
  assert.equal(events, 0);
  const released = run(releaseOwnerEventTimerHold);
  assert.equal(released.flushed, 1);
  assert.deepEqual(scheduled.map(({ delay }) => delay), [8, 0]);
  scheduled[1].callback(...scheduled[1].args);
  assert.equal(events, 2);
  assert.equal(run(readOwnerEventTimerHold), null);
  scope.setTimeout(() => {}, 0);
  assert.deepEqual(scheduled.map(({ delay }) => delay), [8, 0, 0]);
});
