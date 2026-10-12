import assert from "node:assert/strict";
import { test } from "node:test";
import { PNG } from "pngjs";

import {
  cropCapturedPng,
  normalizeOverlayGeneration,
  overlayGenerationMask,
} from "./png-crop-core.mjs";

function coordinatePng(width, height) {
  const image = new PNG({ width, height });
  for (let y = 0; y < height; ++y) {
    for (let x = 0; x < width; ++x) {
      const offset = (y * width + x) * 4;
      image.data[offset] = x;
      image.data[offset + 1] = y;
      image.data[offset + 2] = x + y;
      image.data[offset + 3] = 255;
    }
  }
  return PNG.sync.write(image);
}

function rgbaAt(image, x, y) {
  const offset = (y * image.width + x) * 4;
  return Array.from(image.data.subarray(offset, offset + 4));
}

test("PNG crop maps a nonzero CSS origin at device pixel ratio two", () => {
  const cropped = PNG.sync.read(
    cropCapturedPng(
      coordinatePng(8, 6),
      { x: 100, y: 50, width: 4, height: 3 },
      { x: 101.5, y: 50.5, width: 2, height: 1.5 },
    ),
  );
  assert.deepEqual({ width: cropped.width, height: cropped.height }, { width: 4, height: 3 });
  assert.deepEqual(rgbaAt(cropped, 0, 0), [3, 1, 4, 255]);
  assert.deepEqual(rgbaAt(cropped, 3, 2), [6, 3, 9, 255]);
});

test("PNG crop rejects invalid and out-of-capture CSS bounds", () => {
  const source = coordinatePng(8, 6);
  const capture = { x: 100, y: 50, width: 4, height: 3 };
  const cases = [
    { crop: { x: Number.NaN, y: 50, width: 1, height: 1 }, error: /invalid CSS bounds/ },
    { crop: { x: 100, y: 50, width: 0, height: 1 }, error: /invalid CSS bounds/ },
    { crop: { x: 99.5, y: 50, width: 1, height: 1 }, error: /outside the captured viewport/ },
    { crop: { x: 103.5, y: 52.5, width: 1, height: 1 }, error: /outside the captured viewport/ },
  ];
  for (const { crop, error } of cases) {
    assert.throws(() => cropCapturedPng(source, capture, crop), error);
  }
});

test("overlay normalization preserves document pixels outside the generation suffix", () => {
  const source = coordinatePng(640, 400);
  const original = PNG.sync.read(source);
  const normalized = PNG.sync.read(normalizeOverlayGeneration(source));
  assert.deepEqual(overlayGenerationMask, { x: 28, y: 1, width: 27, height: 19 });
  for (const [x, y] of [[27, 10], [55, 10], [28, 0], [28, 20], [100, 80]]) {
    assert.deepEqual(
      rgbaAt(normalized, x, y),
      rgbaAt(original, x, y),
      "pixel outside the generation suffix changed",
    );
  }
  assert.deepEqual(rgbaAt(normalized, 28, 1), [247, 248, 250, 255]);
  assert.deepEqual(rgbaAt(normalized, 54, 19), [247, 248, 250, 255]);
  assert.throws(
    () => normalizeOverlayGeneration(coordinatePng(8, 6)),
    /outside the 640x400 document capture/,
  );
});
