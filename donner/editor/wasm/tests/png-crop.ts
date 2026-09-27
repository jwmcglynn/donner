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

/** The generation digits vary with earlier compositor rasterizations. */
export const overlayGenerationMask = { x: 28, y: 1, width: 27, height: 19 } as const;

/** Hide only those digits and their variable label backing in the 640x400 document capture. */
export function normalizeOverlayGeneration(png: Buffer): Buffer {
  const image = PNG.sync.read(png);
  const { x, y, width, height } = overlayGenerationMask;
  if (
    image.width !== 640 || image.height !== 400
    || ![x, y, width, height].every(Number.isInteger)
    || x < 0 || y < 0 || width <= 0 || height <= 0
    || x + width > image.width || y + height > image.height
  ) {
    throw new Error("overlay generation mask lies outside the 640x400 document capture");
  }
  for (let row = y; row < y + height; ++row) {
    for (let col = x; col < x + width; ++col) {
      const offset = (row * image.width + col) * 4;
      image.data[offset] = 247;
      image.data[offset + 1] = 248;
      image.data[offset + 2] = 250;
      image.data[offset + 3] = 255;
    }
  }
  return PNG.sync.write(image);
}
