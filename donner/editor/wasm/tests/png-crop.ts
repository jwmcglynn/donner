import { PNG } from "pngjs";

import type { CssRegion } from "./canvas-color-stats";

/** Crop a full-page PNG using the CSS coordinates of the captured viewport. */
export function cropCapturedPng(
  png: Buffer,
  captureRegion: CssRegion,
  cropRegion: CssRegion,
): Buffer {
  const values = [
    captureRegion.x,
    captureRegion.y,
    captureRegion.width,
    captureRegion.height,
    cropRegion.x,
    cropRegion.y,
    cropRegion.width,
    cropRegion.height,
  ];
  if (
    !values.every(Number.isFinite) || captureRegion.width <= 0 || captureRegion.height <= 0
    || cropRegion.width <= 0 || cropRegion.height <= 0
  ) {
    throw new Error("PNG crop has invalid CSS bounds");
  }

  const image = PNG.sync.read(png);
  const scaleX = image.width / captureRegion.width;
  const scaleY = image.height / captureRegion.height;
  const x = Math.floor((cropRegion.x - captureRegion.x) * scaleX);
  const y = Math.floor((cropRegion.y - captureRegion.y) * scaleY);
  const width = Math.round(cropRegion.width * scaleX);
  const height = Math.round(cropRegion.height * scaleY);
  if (
    x < 0 || y < 0 || width < 1 || height < 1
    || x + width > image.width || y + height > image.height
  ) {
    throw new Error("PNG crop lies outside the captured viewport");
  }

  const cropped = new PNG({ width, height });
  PNG.bitblt(image, cropped, x, y, width, height, 0, 0);
  return PNG.sync.write(cropped);
}
