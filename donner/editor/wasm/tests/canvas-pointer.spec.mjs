import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";

import { firstClickBreakdown } from "./first-click-breakdown.mjs";

test("pointer dispatch timestamps the input handler after queued evaluation work", async () => {
  const source = readFileSync(new URL("./canvas-pointer.ts", import.meta.url), "utf8");
  const start = source.indexOf("page.evaluate(") + "page.evaluate(".length;
  const end = source.indexOf("}, { ...point, click });", start) + 1;
  assert.ok(start >= "page.evaluate(".length && end > start, "the actual browser dispatch callback");
  const callbackSource = source.slice(start, end).replace(
    'document.querySelector("canvas#canvas")!',
    'document.querySelector("canvas#canvas")',
  );
  const earlierEvaluationAt = 100;
  let now = earlierEvaluationAt + 50;
  let inputAt;
  const handlers = new Map();
  const canvas = {
    addEventListener(type, handler) { handlers.set(type, handler); },
    removeEventListener(type, handler) {
      if (handlers.get(type) === handler) handlers.delete(type);
    },
    dispatchEvent(event) {
      now += event.type === "mousemove" ? 400 : 3;
      if (event.type === "mousedown") inputAt = now;
      handlers.get(event.type)?.(event);
    },
  };
  const dispatch = new Function("document", "performance", "MouseEvent", "window",
    `return (${callbackSource});`)(
      { querySelector: () => canvas },
      { now: () => now },
      class { constructor(type) { this.type = type; } },
      {},
    );
  const clickAt = dispatch({ x: 10, y: 20, click: true });
  assert.equal(clickAt, inputAt, "queued work and mouse movement must not predate the click stamp");
  assert.equal(handlers.size, 0, "the temporary timestamp listener is removed");
  const perf = readFileSync(new URL("./browser-responsiveness.perf.ts", import.meta.url), "utf8");
  const experiment = perf.indexOf('test("first sample click after boot"');
  const dispatchStart = perf.indexOf("  const clickAt =", experiment);
  const dispatchEnd = perf.indexOf("  await expect", dispatchStart);
  assert.ok(experiment >= 0 && dispatchStart > experiment && dispatchEnd > dispatchStart);
  const measure = new Function("page", "canvas", "dispatchCanvasPointer",
    `return async () => {${perf.slice(dispatchStart, dispatchEnd)} return clickAt; };`)(
      { evaluate: async () => earlierEvaluationAt },
      { x: 0, y: 0, width: 100 },
      async () => clickAt,
    );
  assert.equal(await measure(), inputAt,
    "the actual first-click experiment must consume the dispatch timestamp, not an earlier evaluation");
  const split = firstClickBreakdown([["s", 1, 1, 1, 4, 300, 600]], {
    clickAt, completedAt: 580, presentedAt: 620,
  });
  assert.deepEqual(
    { clickMs: split.presentedMs, backlogMs: split.queueBacklogMs, handoffMs: split.handoffMs },
    { clickMs: 67, backlogMs: 47, handoffMs: 20 },
  );
});
