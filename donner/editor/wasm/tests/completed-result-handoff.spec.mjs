import assert from "node:assert/strict";
import test from "node:test";
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
