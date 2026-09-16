import assert from "node:assert/strict";
import test from "node:test";
import { installCompositedPixelClassifier } from "./composited-pixel-classifier.mjs";

function classifier(config) {
  const previousWindow = Object.getOwnPropertyDescriptor(globalThis, "window");
  const testWindow = {};
  Object.defineProperty(globalThis, "window", { configurable: true, value: testWindow });
  try {
    installCompositedPixelClassifier(config);
    return testWindow.__donnerMatchesCompositedContentPixel;
  } finally {
    if (previousWindow) Object.defineProperty(globalThis, "window", previousWindow);
    else Reflect.deleteProperty(globalThis, "window");
  }
}

test("yellow content excludes cyan selection chrome after the default gates", () => {
  const matches = classifier({
    minColorAlpha: 64,
    minColorSpread: 60,
    colorMask: "yellow-content",
  });
  assert.equal(matches(240, 190, 10, 255), true);
  assert.equal(matches(10, 190, 220, 255), false);
  assert.equal(matches(240, 190, 10, 63), false);
});

test("the default classifier remains alpha and channel-spread based", () => {
  const matches = classifier({ minColorAlpha: 16, minColorSpread: 12, colorMask: null });
  assert.equal(matches(30, 45, 30, 16), true);
  assert.equal(matches(30, 41, 30, 255), false);
  assert.equal(
    classifier({ minColorAlpha: Number.NaN, minColorSpread: 12, colorMask: null })(
      30,
      45,
      30,
      255,
    ),
    false,
  );
});
