import assert from "node:assert/strict";
import test from "node:test";
import { presentedPulseWindow } from "./animation-pulse-observation.mjs";
const click = [100, 2, 0, 32, 10, true];
const tick = (time) => [101, 3, 0, 0, time, false];
const snapshot = (entries, generation = "current") => ({ entries, generation });
const read = (value) => presentedPulseWindow(value, "current", 32, 90);
test("a second admitted tick proves an in-window sample was presented", () => {
  assert.deepEqual(read(snapshot([click, tick(10.15), tick(10.2)])), {
    dispatchEpochMs: 100,
    dispatchTime: 10,
    presentedTime: 10.15,
    latestTime: 10.2,
  });
});

test("60 Hz sampling and a delayed proof remain inside the expanded pulse", () => {
  const dispatched = 40.46666666666667;
  const value = snapshot([
    [100, 2, 0, 32, dispatched, true],
    tick(40.583333333333336),
    tick(40.9),
  ]);
  assert.deepEqual(read(value), {
    dispatchEpochMs: 100,
    dispatchTime: dispatched,
    presentedTime: 40.583333333333336,
    latestTime: 40.9,
  });
});

test("one tick and repeated identical ticks do not prove presentation", () => {
  assert.equal(read(snapshot([click, tick(10.15)])), null);
  assert.equal(read(snapshot([click, tick(10.15), tick(10.15)])), null);
});
test("historical or wrong-target clicks cannot satisfy the observation", () => {
  assert.equal(read(snapshot([click, tick(10.15), tick(10.2)], "old")), null);
  assert.equal(read(snapshot([[80, ...click.slice(1)], tick(10.15), tick(10.2)])), null);
  assert.equal(read(snapshot([[100, 2, 0, 99, 10, true], tick(10.15), tick(10.2)])), null);
});
test("expired history and samples before dispatch cannot satisfy it", () => {
  assert.equal(read(snapshot([click, tick(10.15), tick(10.2), tick(10.7)])), null);
  assert.equal(read(snapshot([tick(10.15), tick(10.2), click])), null);
});
test("unchanged or malformed events fail closed", () => {
  assert.equal(read(snapshot([[100, 2, 0, 32, 10, false], tick(10.15), tick(10.2)])), null);
  assert.equal(read(snapshot([null, [100, 2], tick(10.15), tick(10.2)])), null);
  assert.equal(read(null), null);
});

test("reversed sample time and intervening dispatch invalidate the event identity", () => {
  assert.equal(read(snapshot([click, tick(10.3), tick(10.2)])), null);
  assert.equal(
    read(snapshot([click, tick(10.15), [102, 2, 2, 32, 10.16, true], tick(10.2)])),
    null,
  );
});

test("proof and post-hold verification remain within the expanded part of the pulse", () => {
  const value = snapshot([click, tick(10.15), tick(10.43)]);
  assert.notEqual(read(value), null);
  assert.notEqual(presentedPulseWindow(value, "current", 32, 90, 0.5), null);
  assert.equal(
    presentedPulseWindow(snapshot([click, tick(10.15), tick(10.6)]), "current", 32, 90, 0.5),
    null,
  );
  assert.equal(read(snapshot([click, tick(10.09), tick(10.2)])), null);
  assert.equal(read(snapshot([click, tick(10.15), tick(10.5001)])), null);
});
