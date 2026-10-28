import assert from "node:assert/strict";
import test from "node:test";
import { readFileSync } from "node:fs";
import { completedResultHandoff } from "./completed-result-handoff.mjs";

test("the nominal bound admits the documented healthy range and rejects delayed presentation", () => {
  for (const delay of [400, 560, 1000]) {
    assert.equal(completedResultHandoff(100, 100 + delay).withinLimit, true);
  }
  const delayed = completedResultHandoff(100, 1600);
  assert.deepEqual(delayed, { handoffMs: 1500, limitMs: 1000, withinLimit: false });
  assert.equal(completedResultHandoff(100, 90).withinLimit, false);
  assert.equal(completedResultHandoff(undefined, 200).withinLimit, false);
  assert.equal(completedResultHandoff(100, Infinity).withinLimit, false);
});

test("actual consumers use capture completion and the bound lives in the performance lane", () => {
  const smoke = readFileSync(new URL("./smoke.spec.ts", import.meta.url), "utf8");
  const perf = readFileSync(new URL("./browser-responsiveness.perf.ts", import.meta.url), "utf8");
  const gates = readFileSync(new URL("./latency-gates.mjs", import.meta.url), "utf8");
  assert.match(smoke, /phaseTimings\.presentedMs \?\?= state\.worker!\.resultFrameCompletedAtMs!/);
  const firstClick = perf.slice(perf.indexOf('test("first sample click after boot"'));
  assert.match(firstClick, /presentedAt: worker!\.resultFrameCompletedAtMs!/);
  assert.doesNotMatch(firstClick, /presentedAt: presentation!\.lastPresentedAtMs/);
  // The smoke case keeps only ordering: no time bound on the handoff.
  assert.doesNotMatch(smoke, /withinLimit|handoff\.limitMs/);
  assert.match(smoke, /expect\(Number\.isFinite\(handoff\.handoffMs\)\)\.toBe\(true\)/);
  assert.match(smoke, /expect\(presentationFault!\.requested\)\.toBeGreaterThan\(0\)/);
  assert.match(smoke, /expect\(presentationFault!\.flushed\)\.toBeGreaterThan\(0\)/);
  assert.match(smoke, /toBeGreaterThan\(presentationFault!\.releaseRequestedAtMs\)/);
  // The performance lane measures the same span and gates it at the same bound.
  assert.match(gates, /limitMs: kNominalCompletedResultHandoffLimitMs/);
  const handoff = perf.slice(perf.indexOf('test.describe("nominal completed-result handoff"'));
  assert.match(handoff, /worker!\.publishedAtMs - worker!\.pollDelayMs,\s+worker!\.resultFrameCompletedAtMs!/);
  assert.match(handoff, /latencyGate\("completedResultHandoff"/);
});
