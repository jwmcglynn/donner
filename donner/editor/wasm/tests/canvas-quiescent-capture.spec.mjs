import assert from "node:assert/strict";
import test from "node:test";
import {
  captureQuiescentCanvas,
  kHeldRoundTripReserveMs,
  kMaxCanvasCompletionHoldMs,
  kMaxCanvasQuiescencePasses,
  kMinHeldScreenshotMs,
} from "./canvas-quiescent-capture.mjs";

const kHeldShotMs = kMaxCanvasCompletionHoldMs - kHeldRoundTripReserveMs;

class FakeTimeout extends Error {}

/**
 * A scripted canvas owner on a manual clock. `script` lists one step per pass; each step names the
 * frames that arrive while its screenshot runs and how long the settle and the screenshot take.
 */
function fakeOwner(script) {
  const clock = { ms: 0 };
  const counters = { canvasSubmits: 10, frames: 5 };
  const log = [];
  let holds = 0;
  let step = 0;
  const current = () => script[Math.min(step, script.length - 1)];
  const probe = {
    hold: async () => {
      ++holds;
      log.push(`hold ${holds}`);
      return { release: async () => log.push(`release ${holds}`) };
    },
    settle: async () => {
      const { settleMs = 0, settleError } = current();
      if (settleError) throw new Error(settleError);
      if (settleMs === Infinity) return new Promise(() => {});
      clock.ms += settleMs;
      return { ...counters };
    },
    activity: async () => ({ ...counters }),
  };
  const capture = async (timeoutMs) => {
    const { screenshotMs = 10, framesDuring = 0 } = current();
    ++step;
    log.push(`shot timeout=${Math.round(timeoutMs)}`);
    if (screenshotMs > timeoutMs) {
      clock.ms += timeoutMs;
      throw new FakeTimeout("screenshot timed out");
    }
    clock.ms += screenshotMs;
    counters.frames += framesDuring;
    counters.canvasSubmits += framesDuring;
    return `image ${step}`;
  };
  const options = {
    untilMs: 30_000,
    now: () => clock.ms,
    isTimeoutError: (error) => error instanceof FakeTimeout,
  };
  return { probe, capture, options, log, clock };
}

test("a quiescent screenshot is accepted under one hold", async () => {
  const owner = fakeOwner([{}]);
  const { result, passes } = await captureQuiescentCanvas(owner.probe, owner.capture, owner.options);
  assert.equal(result, "image 1");
  assert.equal(passes.length, 1);
  assert.deepEqual(owner.log, ["hold 1", `shot timeout=${kHeldShotMs}`, "release 1"]);
});

test("a screenshot during which a canvas frame arrives is retaken", async () => {
  const owner = fakeOwner([{ framesDuring: 1 }, { framesDuring: 1 }, {}]);
  const { result, passes } = await captureQuiescentCanvas(owner.probe, owner.capture, owner.options);
  assert.equal(result, "image 3");
  assert.deepEqual(passes.map((pass) => pass.held), [true, true, true]);
  assert.equal(owner.log.filter((line) => line.startsWith("hold")).length, 1);
});

test("a canvas that keeps presenting under the hold holds again and never passes", async () => {
  const owner = fakeOwner([{ framesDuring: 1, screenshotMs: 100 }]);
  await assert.rejects(
    captureQuiescentCanvas(owner.probe, owner.capture, owner.options),
    /kept presenting new frames under \d+ completion holds/,
  );
  const holds = owner.log.filter((line) => line.startsWith("hold")).length;
  assert.ok(holds > 1, "an exhausted hold is released and armed again");
  assert.equal(
    owner.log.filter((line) => line.startsWith("release")).length,
    holds,
    "every hold is released",
  );
  assert.ok(owner.clock.ms >= 30_000 - kMinHeldScreenshotMs, "only the time bound ends it");
});

test("frames that outrun one hold do not fail a capture that quiesces under the next", async () => {
  const script = Array.from({ length: kMaxCanvasQuiescencePasses }, () => ({ framesDuring: 1 }));
  const owner = fakeOwner([...script, {}]);
  const { result, passes } = await captureQuiescentCanvas(owner.probe, owner.capture, owner.options);
  assert.equal(result, `image ${kMaxCanvasQuiescencePasses + 1}`);
  assert.equal(passes.filter((pass) => pass.holdExhausted).length, 1);
  assert.equal(owner.log.filter((line) => line.startsWith("hold")).length, 2);
});

test("a screenshot slower than the hold is retaken without one, with no wall deadline", async () => {
  // The screenshot takes far longer than a hold may last, as on a loaded runner. The first pass
  // gives up its hold; the retake has the rest of the stall guard and is accepted on activity.
  const owner = fakeOwner([{ screenshotMs: 12_000 }]);
  const { result, passes } = await captureQuiescentCanvas(owner.probe, owner.capture, owner.options);
  assert.equal(result, "image 2");
  assert.deepEqual(passes.map((pass) => pass.held), [true, false]);
  assert.deepEqual(owner.log.slice(0, 3), ["hold 1", `shot timeout=${kHeldShotMs}`, "release 1"]);
  assert.ok(owner.clock.ms > 12_000, "the accepted capture finished long after any old deadline");
});

test("a frame during an unheld retake sends the next attempt back under a hold", async () => {
  const owner = fakeOwner([{ screenshotMs: 5000 }, { screenshotMs: 5000, framesDuring: 1 }, {}]);
  const { result, passes } = await captureQuiescentCanvas(owner.probe, owner.capture, owner.options);
  assert.equal(result, "image 3");
  assert.deepEqual(passes.map((pass) => pass.held), [true, false, true]);
});

test("a settle that outlasts its share of the hold releases the hold and holds again", async () => {
  // The first settle never answers; the gate stops waiting for it at the hold's settle share (a
  // real timer of that length) instead of holding the app's completions any longer.
  const owner = fakeOwner([{}]);
  let settles = 0;
  const settle = owner.probe.settle;
  owner.probe.settle = () => (++settles === 1 ? new Promise(() => {}) : settle());
  const { result } = await captureQuiescentCanvas(owner.probe, owner.capture, owner.options);
  assert.equal(result, "image 1");
  assert.equal(settles, 2);
  assert.deepEqual(owner.log.filter((line) => !line.startsWith("shot")), [
    "hold 1",
    "release 1",
    "hold 2",
    "release 2",
  ]);
});

test("only the stall guard ends a capture that never quiesces", async () => {
  const owner = fakeOwner([{ screenshotMs: 40_000 }]);
  await assert.rejects(
    captureQuiescentCanvas(owner.probe, owner.capture, owner.options),
    /no quiescent canvas screenshot within the capture's time bound/,
  );
  assert.ok(owner.clock.ms >= 30_000);
});

test("probe and capture failures propagate without a screenshot being scored", async () => {
  const rejected = fakeOwner([{ settleError: "the surface frame probe is not installed" }]);
  await assert.rejects(
    captureQuiescentCanvas(rejected.probe, rejected.capture, rejected.options),
    /probe is not installed/,
  );
  assert.equal(rejected.log.at(-1), "release 1");
  const failing = fakeOwner([{}]);
  await assert.rejects(
    captureQuiescentCanvas(failing.probe, async () => {
      throw new Error("screenshot failed");
    }, failing.options),
    /screenshot failed/,
  );
});

test("a capture result that reports its own timeout counts as a timed-out screenshot", async () => {
  const owner = fakeOwner([{}]);
  let calls = 0;
  const { result, passes } = await captureQuiescentCanvas(owner.probe, async (timeoutMs) => {
    ++calls;
    owner.clock.ms += calls === 1 ? timeoutMs : 10;
    return { usable: calls > 1 };
  }, { ...owner.options, timedOut: (capture) => !capture.usable });
  assert.deepEqual(result, { usable: true });
  assert.deepEqual(passes.map((pass) => pass.held), [true, false]);
});

test("a caller that never retakes unheld keeps every screenshot under a hold", async () => {
  const owner = fakeOwner([{ screenshotMs: 5000 }, { screenshotMs: 5000 }, {}]);
  const passes = [];
  const { result } = await captureQuiescentCanvas(owner.probe, owner.capture, {
    ...owner.options,
    retakeUnheld: false,
    passes,
  });
  assert.equal(result, "image 3");
  assert.deepEqual(passes.map((pass) => pass.held), [true, true, true]);
  assert.equal(owner.log.filter((line) => line.startsWith("hold")).length, 3);
});

test("a failed release never replaces the capture's own failure", async () => {
  const owner = fakeOwner([{}]);
  owner.probe.hold = async () => ({
    release: async () => {
      throw new Error("release failed");
    },
  });
  const passes = [];
  await assert.rejects(
    captureQuiescentCanvas(owner.probe, async () => {
      throw new Error("screenshot failed");
    }, { ...owner.options, passes }),
    /screenshot failed/,
  );
  assert.deepEqual(passes.at(-1), { holdReleaseError: "Error: release failed" });
  // Without another failure, the release failure itself fails the capture.
  const released = fakeOwner([{}]);
  released.probe.hold = owner.probe.hold;
  await assert.rejects(
    captureQuiescentCanvas(released.probe, released.capture, released.options),
    /release failed/,
  );
});
