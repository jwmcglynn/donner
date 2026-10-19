import assert from "node:assert/strict";
import test from "node:test";
import {
  gatedPulseFrame,
  presentedPulseCompletion,
  queuedDispatchCaughtUp,
} from "./animation-pulse-observation.mjs";
const click = [100, 2, 0, 32, 10, true];
const tick = (time) => [101, 3, 0, 0, time, false];
const snapshot = (entries, generation = "current") => ({ entries, generation });
const decide = (rendered, value, notBefore = 90) =>
  gatedPulseFrame(rendered, value, "current", 32, notBefore);

test("frames before the expanded pulse ask for another raster step", () => {
  assert.deepEqual(decide([], snapshot([])), { action: "step" });
  assert.deepEqual(decide([9.5], snapshot([click])), { action: "step" });
  assert.deepEqual(decide([9.5, 10, 10.05], snapshot([click, tick(10.05)])), { action: "step" });
  // A click before the probe began is not this probe's dispatch.
  assert.deepEqual(decide([10.2], snapshot([click]), 101), { action: "step" });
});

test("a single expanded frame is retained once a later sample proves its presentation", () => {
  const observation = {
    dispatchEpochMs: 100,
    dispatchTarget: 32,
    dispatchTime: 10,
    renderedTime: 10.25,
  };
  assert.deepEqual(decide([10, 10.25], snapshot([click, tick(10.25)])), {
    action: "await-presentation",
    ...observation,
  });
  assert.deepEqual(decide([10, 10.25], snapshot([click, tick(10.25), tick(10.683)])), {
    action: "capture",
    ...observation,
    proofTime: 10.683,
  });
});

test("the newest rasterized frame decides, not the newest admitted sample", () => {
  // The admitted 10.9 sample is never rasterized while the raster yield stays held.
  assert.deepEqual(
    decide([10.067, 10.417], snapshot([click, tick(10.067), tick(10.417), tick(10.9)])),
    {
      action: "capture",
      dispatchEpochMs: 100,
      dispatchTarget: 32,
      dispatchTime: 10,
      renderedTime: 10.417,
      proofTime: 10.9,
    },
  );
});

test("window bounds are inclusive and a frame past the window is terminal", () => {
  const atZero = [100, 2, 0, 32, 0, true];
  assert.equal(decide([0.1], snapshot([atZero, tick(0.1), tick(0.2)])).action, "capture");
  assert.equal(decide([0.5], snapshot([atZero, tick(0.5), tick(0.6)])).action, "capture");
  assert.equal(decide([0.09], snapshot([atZero, tick(0.09), tick(0.2)])).action, "step");
  assert.equal(decide([0.501], snapshot([atZero, tick(0.501)])).action, "overshoot");
  assert.deepEqual(decide([10.05, 10.55], snapshot([click, tick(10.05), tick(10.55)])), {
    action: "overshoot",
    dispatchEpochMs: 100,
    dispatchTarget: 32,
    dispatchTime: 10,
    renderedTime: 10.55,
  });
});

test("a presentation proof must follow the rasterized frame", () => {
  assert.equal(
    decide([10.3], snapshot([click, tick(10.2), tick(10.3)])).action,
    "await-presentation",
  );
});

test("a proof must be strictly later than the rasterized frame", () => {
  assert.equal(
    decide([10.3], snapshot([click, tick(10.3), tick(10.3)])).action,
    "await-presentation",
  );
});

test("reversed or non-finite timeline samples fail closed", () => {
  assert.equal(decide([10.2], snapshot([click, tick(10.3), tick(10.2)])).action, "invalid");
  assert.equal(decide([10.2], snapshot([click, tick(9.9)])).action, "invalid");
  assert.equal(decide([10.2], snapshot([click, tick(Number.NaN)])).action, "invalid");
});

test("stale, malformed or interrupted traces fail closed", () => {
  assert.equal(decide([10.2], snapshot([click, tick(10.3)], "old")).action, "invalid");
  assert.equal(decide([10.2], null).action, "invalid");
  assert.equal(decide([Number.NaN], snapshot([click])).action, "invalid");
  assert.equal(decide(null, snapshot([click])).action, "invalid");
  assert.equal(decide([10.2], snapshot([click, [100, 2]])).action, "invalid");
  assert.equal(
    decide([10.2], snapshot([click, [102, 2, 2, 32, 10.1, true], tick(10.3)])).action,
    "invalid",
  );
});

test("an unknown target accepts the first changed click", () => {
  assert.equal(
    gatedPulseFrame([10.2], snapshot([click, tick(10.2), tick(10.3)]), "current", null, 90).action,
    "capture",
  );
});

test("wrong-target or unchanged clicks are not this probe's dispatch", () => {
  assert.deepEqual(decide([10.2], snapshot([[100, 2, 0, 99, 10, true], tick(10.3)])), {
    action: "step",
  });
  assert.deepEqual(decide([10.2], snapshot([[100, 2, 0, 32, 10, false], tick(10.3)])), {
    action: "step",
  });
});

test("completion requires a presented sample after the entire pulse", () => {
  const complete = (value) => presentedPulseCompletion(value, "current", 32, 90);
  assert.equal(complete(snapshot([click, tick(10.2), tick(10.3)])), null);
  assert.equal(complete(snapshot([click, tick(10.59), tick(10.7)])), null);
  assert.equal(complete(snapshot([click, tick(10.7)])), null);
  assert.equal(complete(snapshot([click, tick(10.7), tick(10.7)])), null);
  assert.deepEqual(complete(snapshot([click, tick(10.7), tick(10.8)])), {
    dispatchEpochMs: 100,
    dispatchTime: 10,
    presentedTime: 10.7,
    latestTime: 10.8,
  });
});

test("completion rejects stale generations, wrong targets and intervening input", () => {
  const complete = (value) => presentedPulseCompletion(value, "current", 32, 90);
  assert.equal(complete(snapshot([click, tick(10.7), tick(10.8)], "old")), null);
  assert.equal(complete(snapshot([[100, 2, 0, 99, 10, true], tick(10.7), tick(10.8)])), null);
  assert.equal(
    complete(snapshot([click, tick(10.7), [102, 2, 0, 32, 10.75, true], tick(10.8)])),
    null,
  );
  assert.equal(complete(snapshot([click, tick(10.8), tick(10.7)])), null);
});

test("a queued click must dispatch at least one pulse after the held stale frame", () => {
  assert.deepEqual(queuedDispatchCaughtUp([20, 21.2], 22.25), {
    caughtUp: true,
    heldTime: 21.2,
    dispatchTime: 22.25,
  });
  assert.equal(queuedDispatchCaughtUp([21.2], 21.8).caughtUp, true);
  // Dispatching at the stale frame's time, or inside its pulse, is the catch-up regression.
  assert.equal(queuedDispatchCaughtUp([21.2], 21.2).caughtUp, false);
  assert.equal(queuedDispatchCaughtUp([21.2], 21.7).caughtUp, false);
  for (const held of [[], undefined, [Number.NaN]]) {
    assert.deepEqual(queuedDispatchCaughtUp(held, 22), {
      caughtUp: false,
      reason: "no held frame time",
    });
  }
  assert.equal(queuedDispatchCaughtUp([21.2], Number.NaN).caughtUp, false);
});
