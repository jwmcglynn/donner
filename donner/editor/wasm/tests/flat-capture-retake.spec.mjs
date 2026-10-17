import assert from "node:assert/strict";
import test from "node:test";
import { isEditorBackgroundImage, retakeFlatCapture } from "./flat-capture-retake.mjs";

function fixture(images) {
  let clock = 0;
  const captured = [], retained = [];
  return {
    captured,
    retained,
    options: {
      deadline: 20,
      now: () => clock,
      capture: async (remaining) => {
        captured.push(remaining);
        clock += 3;
        return images.shift();
      },
      isFlat: (value) => value === "flat",
      retain: async (image, attempt) => {
        retained.push([image, attempt]);
        clock += 2;
      },
    },
  };
}
test("nonuniform image without an orb goes directly to the original oracle", async () => {
  const f = fixture(["missing-orb"]);
  assert.equal(await retakeFlatCapture(f.options), "missing-orb");
  assert.deepEqual(f.captured, [20]);
  assert.deepEqual(f.retained, []);
});
test("one retake retains both images and includes evidence time in the original budget", async () => {
  const f = fixture(["flat", "valid"]);
  assert.equal(await retakeFlatCapture(f.options), "valid");
  assert.deepEqual(f.captured, [20, 15]);
  assert.deepEqual(f.retained, [["flat", 0], ["valid", 1]]);
});
test("two flat images fail without a third capture", async () => {
  const f = fixture(["flat", "flat", "valid"]);
  await assert.rejects(retakeFlatCapture(f.options), /remained entirely/);
  assert.equal(f.captured.length, 2);
});
test("capture errors propagate without retry", async () => {
  const f = fixture([]);
  const error = new Error("capture timeout");
  f.options.capture = async () => {
    throw error;
  };
  await assert.rejects(retakeFlatCapture(f.options), (actual) => actual === error);
  assert.deepEqual(f.retained, []);
});
test("evidence exhausting the budget prevents a retake", async () => {
  const f = fixture(["flat", "valid"]);
  f.options.deadline = 5;
  await assert.rejects(retakeFlatCapture(f.options), /deadline/);
  assert.equal(f.captured.length, 1);
});
test("late second capture fails even if its image is valid", async () => {
  const f = fixture(["flat", "valid"]);
  f.options.deadline = 9;
  await assert.rejects(retakeFlatCapture(f.options), /deadline/);
  assert.equal(f.captured.length, 2);
});

test("only exact opaque page background qualifies", () => {
  assert.equal(isEditorBackgroundImage({ channels: 3, data: [16, 19, 23, 16, 19, 23] }), true);
  assert.equal(isEditorBackgroundImage({ channels: 4, data: [16, 19, 23, 255] }), true);
  for (const data of [[16, 19, 23, 254], [16, 19, 24, 255], [0, 0, 0, 255], []]) {
    assert.equal(isEditorBackgroundImage({ channels: 4, data }), false);
  }
  assert.equal(isEditorBackgroundImage({ channels: 3, data: [16, 19, 23, 16, 20, 23] }), false);
});

test("classification cannot overrun the deadline and return success", async () => {
  const f = fixture(["valid"]);
  let clock = 0;
  f.options.now = () => clock;
  f.options.isFlat = () => {
    clock = 21;
    return false;
  };
  await assert.rejects(retakeFlatCapture(f.options), /deadline/);
  assert.equal(f.captured.length, 1);
});
