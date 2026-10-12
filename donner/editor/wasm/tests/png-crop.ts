import {
  cropCapturedPng as crop,
  normalizeOverlayGeneration as normalize,
  overlayGenerationMask as generationMask,
} from "./png-crop-core.mjs";

import type { CssRegion } from "./canvas-color-stats";

/** Crop a full-page PNG using the CSS coordinates of the captured viewport. */
export function cropCapturedPng(
  png: Buffer,
  captureRegion: CssRegion,
  cropRegion: CssRegion,
): Buffer {
  return crop(png, captureRegion, cropRegion);
}

/** The generation digits vary with earlier compositor rasterizations. */
export const overlayGenerationMask = generationMask as {
  readonly x: 28;
  readonly y: 1;
  readonly width: 27;
  readonly height: 19;
};

/** Hide only those digits and their variable label backing in the 640x400 document capture. */
export function normalizeOverlayGeneration(png: Buffer): Buffer {
  return normalize(png);
}
