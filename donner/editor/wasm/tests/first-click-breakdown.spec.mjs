// Checks how the first-click perf metric splits a click's latency, against fixed timelines.

import assert from "node:assert/strict";
import test from "node:test";

import { firstClickBreakdown } from "./first-click-breakdown.mjs";

test("the backlog is the last pre-click submission's completion and handoff what follows", () => {
  const timeline = [
    ["s", 1, 1, 1, 4, 10, 40],
    ["s", 2, 1, 1, 9, 90, 1600],
    ["s", 1, 2, 1, 3, 120, 1620],
    ["s", 1, 3, 1, 3, 400, 1640],
  ];
  assert.deepEqual(
    firstClickBreakdown(timeline, { clickAt: 100, completedAt: 300, presentedAt: 1660 }),
    {
      presentedMs: 1560,
      resultCompletedMs: 200,
      queueBacklogMs: 1500,
      handoffMs: 60,
      submissionsAheadOfClick: 1,
      pipelinesCompilingAtClick: 0,
      firstUsePipelineCompiles: 0,
    },
  );
});

test("an idle queue at the click leaves the whole wait to the result and its handoff", () => {
  const timeline = [["s", 1, 1, 1, 4, 10, 40]];
  const breakdown = firstClickBreakdown(timeline, {
    clickAt: 100,
    completedAt: 180,
    presentedAt: 200,
  });
  assert.equal(breakdown.queueBacklogMs, 0);
  assert.equal(breakdown.handoffMs, 20);
});

test("a pre-click submission still running when the timeline was read leaves the split unknown", () => {
  const timeline = [["s", 1, 1, 1, 4, 10, 0]];
  const breakdown = firstClickBreakdown(timeline, {
    clickAt: 100,
    completedAt: 180,
    presentedAt: 200,
  });
  assert.deepEqual(
    { backlog: breakdown.queueBacklogMs, handoff: breakdown.handoffMs },
    { backlog: null, handoff: null },
  );
});

test("pipelines still compiling at the click and first-use compiles are counted", () => {
  const timeline = [
    ["p", 1, 5, 50, 0],
    ["p", 1, 6, 0, 0],
    ["p", 1, 7, 900, 0],
    ["p", 2, 8, 50, 1],
    ["c", 1, 150],
    ["c", 1, 5000],
  ];
  const breakdown = firstClickBreakdown(timeline, {
    clickAt: 100,
    completedAt: 180,
    presentedAt: 200,
  });
  assert.deepEqual(
    {
      compiling: breakdown.pipelinesCompilingAtClick,
      firstUse: breakdown.firstUsePipelineCompiles,
    },
    { compiling: 2, firstUse: 1 },
  );
});
