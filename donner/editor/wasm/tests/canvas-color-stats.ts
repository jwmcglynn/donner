import type { Locator, Page } from "@playwright/test";
import { inflateSync } from "node:zlib";

export interface CssRegion {
  x: number;
  y: number;
  width: number;
  height: number;
}

export interface CanvasColorStats {
  samples: number;
  coloredPixels: number;
  nonBlackPixels: number;
  maxChannel: number;
  region: CssRegion;
}

interface PngImage {
  width: number;
  height: number;
  channels: number;
  data: Uint8Array;
}

export interface PixelBounds {
  minX: number;
  minY: number;
  maxX: number;
  maxY: number;
  pixels: number;
}

export interface PngPixelDifferenceStats {
  comparedPixels: number;
  changedPixels: number;
  changedBounds: PixelBounds | null;
  changedPixelsAbove8: number;
  maxChannelDelta: number;
  totalChannelDelta: number;
}

export interface TextStyleGlyphStats {
  backgroundPixels: number;
  glyphPixels: number;
}

export interface SplashSurfaceCoverageStats {
  checkerboardPixels: number;
  darkBackgroundPixels: number;
  samples: number;
}

export interface SplashCompositeFrameStats extends SplashSurfaceCoverageStats {
  teal: PixelBounds | null;
  yellow: PixelBounds | null;
}

export interface SplashCompositeFrameCapture {
  png: Buffer;
  stats: SplashCompositeFrameStats;
}

export function readSplashCompositeFrameStatsFromPng(
  png: Buffer,
  letterSearchBounds: Omit<PixelBounds, "pixels">,
): SplashCompositeFrameStats {
  const image = decodePng(png);
  let checkerboardPixels = 0;
  let darkBackgroundPixels = 0;
  for (let offset = 0; offset < image.data.length; offset += image.channels) {
    const red = image.data[offset];
    const green = image.data[offset + 1];
    const blue = image.data[offset + 2];
    const alpha = image.channels === 4 ? image.data[offset + 3] : 255;
    if (alpha < 200) {
      continue;
    }
    const maxRgb = Math.max(red, green, blue);
    const minRgb = Math.min(red, green, blue);
    if (minRgb >= 215 && maxRgb - minRgb <= 18) {
      ++checkerboardPixels;
    }
    if (red >= 20 && red <= 72 && green >= 20 && green <= 72 && blue >= red + 5 && blue <= 105) {
      ++darkBackgroundPixels;
    }
  }
  return {
    checkerboardPixels,
    darkBackgroundPixels,
    samples: image.width * image.height,
    teal: findPixelBounds(image, "selection-teal", letterSearchBounds),
    yellow: findPixelBounds(image, "splash-yellow", letterSearchBounds),
  };
}

export type EditorPixelTarget = "basic-blue" | "selection-teal" | "splash-yellow";

export function readEditorPixelBoundsFromPng(
  png: Buffer,
  target: EditorPixelTarget,
  cssSize: Pick<CssRegion, "width" | "height">,
  searchBounds: Omit<PixelBounds, "pixels">,
): PixelBounds | null {
  const image = decodePng(png);
  const scaleX = image.width / cssSize.width;
  const scaleY = image.height / cssSize.height;
  const bounds = findPixelBounds(image, target, {
    minX: Math.floor(searchBounds.minX * scaleX),
    minY: Math.floor(searchBounds.minY * scaleY),
    maxX: Math.ceil(searchBounds.maxX * scaleX),
    maxY: Math.ceil(searchBounds.maxY * scaleY),
  });
  return bounds === null
    ? null
    : {
      minX: bounds.minX / scaleX,
      minY: bounds.minY / scaleY,
      maxX: bounds.maxX / scaleX,
      maxY: bounds.maxY / scaleY,
      pixels: bounds.pixels,
    };
}

function paethPredictor(left: number, up: number, upLeft: number): number {
  const estimate = left + up - upLeft;
  const leftDistance = Math.abs(estimate - left);
  const upDistance = Math.abs(estimate - up);
  const upLeftDistance = Math.abs(estimate - upLeft);
  if (leftDistance <= upDistance && leftDistance <= upLeftDistance) {
    return left;
  }
  return upDistance <= upLeftDistance ? up : upLeft;
}

function decodePng(buffer: Buffer): PngImage {
  const signature = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
  if (!buffer.subarray(0, signature.length).equals(signature)) {
    throw new Error("screenshot is not a PNG");
  }

  let width = 0;
  let height = 0;
  let channels = 0;
  const idatChunks: Buffer[] = [];
  for (let offset = signature.length; offset < buffer.length;) {
    const chunkLength = buffer.readUInt32BE(offset);
    const type = buffer.toString("ascii", offset + 4, offset + 8);
    const dataOffset = offset + 8;
    const dataEnd = dataOffset + chunkLength;
    const data = buffer.subarray(dataOffset, dataEnd);
    offset = dataEnd + 4;

    if (type === "IHDR") {
      width = data.readUInt32BE(0);
      height = data.readUInt32BE(4);
      const bitDepth = data[8];
      const colorType = data[9];
      const compression = data[10];
      const filter = data[11];
      const interlace = data[12];
      if (
        bitDepth !== 8
        || compression !== 0
        || filter !== 0
        || interlace !== 0
        || (colorType !== 2 && colorType !== 6)
      ) {
        throw new Error(`unsupported PNG format: depth=${bitDepth} color=${colorType}`);
      }
      channels = colorType === 6 ? 4 : 3;
    } else if (type === "IDAT") {
      idatChunks.push(data);
    } else if (type === "IEND") {
      break;
    }
  }

  if (width <= 0 || height <= 0 || channels <= 0 || idatChunks.length === 0) {
    throw new Error("PNG is missing image data");
  }

  const bytesPerRow = width * channels;
  const raw = inflateSync(Buffer.concat(idatChunks));
  const output = new Uint8Array(height * bytesPerRow);
  let sourceOffset = 0;
  for (let y = 0; y < height; ++y) {
    const filter = raw[sourceOffset++];
    const rowOffset = y * bytesPerRow;
    const previousRowOffset = rowOffset - bytesPerRow;
    for (let x = 0; x < bytesPerRow; ++x) {
      const value = raw[sourceOffset++];
      const left = x >= channels ? output[rowOffset + x - channels] : 0;
      const up = y > 0 ? output[previousRowOffset + x] : 0;
      const upLeft = y > 0 && x >= channels ? output[previousRowOffset + x - channels] : 0;
      let reconstructed = value;
      if (filter === 1) {
        reconstructed += left;
      } else if (filter === 2) {
        reconstructed += up;
      } else if (filter === 3) {
        reconstructed += Math.floor((left + up) / 2);
      } else if (filter === 4) {
        reconstructed += paethPredictor(left, up, upLeft);
      } else if (filter !== 0) {
        throw new Error(`unsupported PNG row filter ${filter}`);
      }
      output[rowOffset + x] = reconstructed & 0xff;
    }
  }

  return { width, height, channels, data: output };
}

export function readPngPixelDifferenceStats(
  beforeBuffer: Buffer,
  afterBuffer: Buffer,
  region?: CssRegion,
): PngPixelDifferenceStats {
  const before = decodePng(beforeBuffer);
  const after = decodePng(afterBuffer);
  if (
    before.width !== after.width
    || before.height !== after.height
    || before.channels !== after.channels
  ) {
    throw new Error(
      `PNG dimensions differ: ${before.width}x${before.height}x${before.channels}`
        + ` vs ${after.width}x${after.height}x${after.channels}`,
    );
  }

  const minX = Math.max(0, Math.floor(region?.x ?? 0));
  const minY = Math.max(0, Math.floor(region?.y ?? 0));
  const maxX = Math.min(
    before.width,
    Math.ceil((region?.x ?? 0) + (region?.width ?? before.width)),
  );
  const maxY = Math.min(
    before.height,
    Math.ceil((region?.y ?? 0) + (region?.height ?? before.height)),
  );
  let changedPixels = 0;
  let changedPixelsAbove8 = 0;
  let maxChannelDelta = 0;
  let totalChannelDelta = 0;
  let changedMinX = before.width;
  let changedMinY = before.height;
  let changedMaxX = -1;
  let changedMaxY = -1;
  for (let y = minY; y < maxY; ++y) {
    for (let x = minX; x < maxX; ++x) {
      const offset = (y * before.width + x) * before.channels;
      let pixelChanged = false;
      let pixelMaxDelta = 0;
      for (let channel = 0; channel < before.channels; ++channel) {
        const delta = Math.abs(before.data[offset + channel] - after.data[offset + channel]);
        maxChannelDelta = Math.max(maxChannelDelta, delta);
        pixelMaxDelta = Math.max(pixelMaxDelta, delta);
        totalChannelDelta += delta;
        pixelChanged ||= delta !== 0;
      }
      if (pixelChanged) {
        ++changedPixels;
        changedPixelsAbove8 += pixelMaxDelta > 8 ? 1 : 0;
        changedMinX = Math.min(changedMinX, x);
        changedMinY = Math.min(changedMinY, y);
        changedMaxX = Math.max(changedMaxX, x);
        changedMaxY = Math.max(changedMaxY, y);
      }
    }
  }

  return {
    comparedPixels: Math.max(0, maxX - minX) * Math.max(0, maxY - minY),
    changedPixels,
    changedBounds: changedPixels > 0
      ? {
        minX: changedMinX,
        minY: changedMinY,
        maxX: changedMaxX,
        maxY: changedMaxY,
        pixels: changedPixels,
      }
      : null,
    changedPixelsAbove8,
    maxChannelDelta,
    totalChannelDelta,
  };
}

export function readCssPngPixelDifferenceStats(
  beforeBuffer: Buffer,
  afterBuffer: Buffer,
  screenshotCssSize: Pick<CssRegion, "width" | "height">,
  region?: CssRegion,
): PngPixelDifferenceStats {
  const image = decodePng(beforeBuffer);
  const scaleX = image.width / screenshotCssSize.width;
  const scaleY = image.height / screenshotCssSize.height;
  return readPngPixelDifferenceStats(
    beforeBuffer,
    afterBuffer,
    region === undefined
      ? undefined
      : {
        x: region.x * scaleX,
        y: region.y * scaleY,
        width: region.width * scaleX,
        height: region.height * scaleY,
      },
  );
}

function measureImage(image: PngImage, region: CssRegion): CanvasColorStats {
  let coloredPixels = 0;
  let nonBlackPixels = 0;
  let maxChannel = 0;
  for (let offset = 0; offset < image.data.length; offset += image.channels) {
    const red = image.data[offset];
    const green = image.data[offset + 1];
    const blue = image.data[offset + 2];
    const alpha = image.channels === 4 ? image.data[offset + 3] : 255;
    const maxRgb = Math.max(red, green, blue);
    const minRgb = Math.min(red, green, blue);
    maxChannel = Math.max(maxChannel, maxRgb);
    if (alpha > 0 && maxRgb > 12) {
      nonBlackPixels += 1;
    }
    if (alpha > 0 && maxRgb > 50 && maxRgb - minRgb > 20) {
      coloredPixels += 1;
    }
  }

  return {
    samples: image.width * image.height,
    coloredPixels,
    nonBlackPixels,
    maxChannel,
    region,
  };
}

function matchesEditorPixelTarget(
  target: EditorPixelTarget,
  red: number,
  green: number,
  blue: number,
  alpha: number,
): boolean {
  if (alpha < 180) {
    return false;
  }
  if (target === "basic-blue") {
    return blue > 170 && blue > green + 45 && green > red + 25;
  }
  if (target === "splash-yellow") {
    return red > 180 && green > 135 && blue < 150 && red > blue + 70 && green > blue + 45;
  }
  return green > 170 && blue > 150 && green > red + 65 && blue > red + 45
    && Math.abs(green - blue) < 45;
}

function findPixelBounds(
  image: PngImage,
  target: EditorPixelTarget,
  searchBounds?: Omit<PixelBounds, "pixels">,
): PixelBounds | null {
  const searchMinX = Math.max(0, searchBounds?.minX ?? 0);
  const searchMinY = Math.max(0, searchBounds?.minY ?? 0);
  const searchMaxX = Math.min(image.width - 1, searchBounds?.maxX ?? image.width - 1);
  const searchMaxY = Math.min(image.height - 1, searchBounds?.maxY ?? image.height - 1);
  let minX = image.width;
  let minY = image.height;
  let maxX = -1;
  let maxY = -1;
  let pixels = 0;
  for (let y = searchMinY; y <= searchMaxY; ++y) {
    for (let x = searchMinX; x <= searchMaxX; ++x) {
      const offset = (y * image.width + x) * image.channels;
      const red = image.data[offset];
      const green = image.data[offset + 1];
      const blue = image.data[offset + 2];
      const alpha = image.channels === 4 ? image.data[offset + 3] : 255;
      if (!matchesEditorPixelTarget(target, red, green, blue, alpha)) {
        continue;
      }
      minX = Math.min(minX, x);
      minY = Math.min(minY, y);
      maxX = Math.max(maxX, x);
      maxY = Math.max(maxY, y);
      pixels += 1;
    }
  }
  return pixels > 0 ? { minX, minY, maxX, maxY, pixels } : null;
}

function measureTextStyleGlyphs(image: PngImage): TextStyleGlyphStats {
  const isBackground = (red: number, green: number, blue: number, alpha: number) =>
    alpha > 200 && red >= 15 && red <= 32 && green >= 24 && green <= 45 && blue >= 34
    && blue <= 55;

  let minBackgroundX = image.width;
  let minBackgroundY = image.height;
  let maxBackgroundX = -1;
  let maxBackgroundY = -1;
  let backgroundPixels = 0;
  for (let y = 0; y < image.height; ++y) {
    for (let x = 0; x < image.width; ++x) {
      const offset = (y * image.width + x) * image.channels;
      const alpha = image.channels === 4 ? image.data[offset + 3] : 255;
      if (
        !isBackground(image.data[offset], image.data[offset + 1], image.data[offset + 2], alpha)
      ) {
        continue;
      }
      minBackgroundX = Math.min(minBackgroundX, x);
      minBackgroundY = Math.min(minBackgroundY, y);
      maxBackgroundX = Math.max(maxBackgroundX, x);
      maxBackgroundY = Math.max(maxBackgroundY, y);
      backgroundPixels += 1;
    }
  }

  if (backgroundPixels === 0) {
    return { backgroundPixels: 0, glyphPixels: 0 };
  }

  let glyphPixels = 0;
  for (let y = minBackgroundY; y <= maxBackgroundY; ++y) {
    for (let x = minBackgroundX; x <= maxBackgroundX; ++x) {
      const offset = (y * image.width + x) * image.channels;
      const red = image.data[offset];
      const green = image.data[offset + 1];
      const blue = image.data[offset + 2];
      const alpha = image.channels === 4 ? image.data[offset + 3] : 255;
      const neutralLight = alpha > 200 && Math.min(red, green, blue) > 130
        && Math.max(red, green, blue) - Math.min(red, green, blue) < 35;
      const mintText = alpha > 200 && red > 100 && green > 170 && blue > 140
        && green - red > 30 && green - blue > 10;
      if (neutralLight || mintText) {
        glyphPixels += 1;
      }
    }
  }

  return { backgroundPixels, glyphPixels };
}

export async function readElementColorStats(locator: Locator): Promise<CanvasColorStats> {
  const box = await locator.boundingBox();
  if (box === null) {
    throw new Error("element not found");
  }
  const image = decodePng(await locator.screenshot());
  return measureImage(image, { x: 0, y: 0, width: image.width, height: image.height });
}

export async function readSplashSurfaceCoverageStats(
  locator: Locator,
): Promise<SplashSurfaceCoverageStats> {
  const box = await locator.boundingBox();
  if (box === null) {
    throw new Error("element not found");
  }
  const image = decodePng(await locator.screenshot());
  let checkerboardPixels = 0;
  let darkBackgroundPixels = 0;
  for (let offset = 0; offset < image.data.length; offset += image.channels) {
    const red = image.data[offset];
    const green = image.data[offset + 1];
    const blue = image.data[offset + 2];
    const alpha = image.channels === 4 ? image.data[offset + 3] : 255;
    if (alpha < 200) {
      continue;
    }
    const maxRgb = Math.max(red, green, blue);
    const minRgb = Math.min(red, green, blue);
    if (minRgb >= 215 && maxRgb - minRgb <= 18) {
      ++checkerboardPixels;
    }
    if (red >= 20 && red <= 72 && green >= 20 && green <= 72 && blue >= red + 5 && blue <= 105) {
      ++darkBackgroundPixels;
    }
  }
  return {
    checkerboardPixels,
    darkBackgroundPixels,
    samples: image.width * image.height,
  };
}

export async function readSplashPageCoverageStats(
  page: Page,
  region: CssRegion,
): Promise<SplashSurfaceCoverageStats> {
  const image = decodePng(await page.screenshot({ clip: region }));
  let checkerboardPixels = 0;
  let darkBackgroundPixels = 0;
  for (let offset = 0; offset < image.data.length; offset += image.channels) {
    const red = image.data[offset];
    const green = image.data[offset + 1];
    const blue = image.data[offset + 2];
    const alpha = image.channels === 4 ? image.data[offset + 3] : 255;
    if (alpha < 200) {
      continue;
    }
    const maxRgb = Math.max(red, green, blue);
    const minRgb = Math.min(red, green, blue);
    if (minRgb >= 215 && maxRgb - minRgb <= 18) {
      ++checkerboardPixels;
    }
    if (red >= 20 && red <= 72 && green >= 20 && green <= 72 && blue >= red + 5 && blue <= 105) {
      ++darkBackgroundPixels;
    }
  }
  return {
    checkerboardPixels,
    darkBackgroundPixels,
    samples: image.width * image.height,
  };
}

export async function captureSplashCompositeFrame(
  page: Page,
  region: CssRegion,
  letterSearchBounds: Omit<PixelBounds, "pixels">,
): Promise<SplashCompositeFrameCapture> {
  const png = await page.screenshot({ clip: region });
  const image = decodePng(png);
  const screenshotScaleX = image.width / region.width;
  const screenshotScaleY = image.height / region.height;
  const screenshotLetterSearchBounds = {
    minX: Math.floor(letterSearchBounds.minX * screenshotScaleX),
    minY: Math.floor(letterSearchBounds.minY * screenshotScaleY),
    maxX: Math.ceil(letterSearchBounds.maxX * screenshotScaleX),
    maxY: Math.ceil(letterSearchBounds.maxY * screenshotScaleY),
  };
  const toCssBounds = (
    bounds: PixelBounds | null,
  ): PixelBounds | null => bounds === null
    ? null
    : {
      minX: bounds.minX / screenshotScaleX,
      minY: bounds.minY / screenshotScaleY,
      maxX: bounds.maxX / screenshotScaleX,
      maxY: bounds.maxY / screenshotScaleY,
      pixels: bounds.pixels,
  };
  const stats = readSplashCompositeFrameStatsFromPng(
    png,
    screenshotLetterSearchBounds,
  );
  return {
    png,
    stats: {
      ...stats,
      teal: toCssBounds(stats.teal),
      yellow: toCssBounds(stats.yellow),
    },
  };
}

export async function readSplashCompositeFrameStats(
  page: Page,
  region: CssRegion,
  letterSearchBounds: Omit<PixelBounds, "pixels">,
): Promise<SplashCompositeFrameStats> {
  return (await captureSplashCompositeFrame(page, region, letterSearchBounds)).stats;
}

export async function findElementColoredPixel(locator: Locator): Promise<{ x: number; y: number }> {
  const box = await locator.boundingBox();
  if (box === null) {
    throw new Error("element not found");
  }
  const image = decodePng(await locator.screenshot());
  for (let y = 0; y < image.height; ++y) {
    for (let x = 0; x < image.width; ++x) {
      const offset = (y * image.width + x) * image.channels;
      const red = image.data[offset];
      const green = image.data[offset + 1];
      const blue = image.data[offset + 2];
      const alpha = image.channels === 4 ? image.data[offset + 3] : 255;
      const maxRgb = Math.max(red, green, blue);
      const minRgb = Math.min(red, green, blue);
      if (alpha > 0 && maxRgb > 80 && maxRgb - minRgb > 30) {
        return {
          x: ((x + 0.5) / image.width) * box.width,
          y: ((y + 0.5) / image.height) * box.height,
        };
      }
    }
  }
  throw new Error("element contains no colored pixel");
}

export async function readCanvasColorStats(
  page: Page,
  region: CssRegion,
): Promise<CanvasColorStats> {
  const canvasBox = await page.locator("canvas#canvas").boundingBox();
  if (canvasBox === null) {
    throw new Error("canvas not found");
  }

  const x = Math.max(0, region.x);
  const y = Math.max(0, region.y);
  const width = Math.max(1, Math.min(region.width, canvasBox.width - x));
  const height = Math.max(1, Math.min(region.height, canvasBox.height - y));
  const clip = {
    x: canvasBox.x + x,
    y: canvasBox.y + y,
    width,
    height,
  };
  const image = decodePng(await page.screenshot({ clip }));

  return measureImage(image, { x, y, width: image.width, height: image.height });
}

export async function readTextStyleGlyphStats(
  page: Page,
  region: CssRegion,
): Promise<TextStyleGlyphStats> {
  const canvasBox = await page.locator("canvas#canvas").boundingBox();
  if (canvasBox === null) {
    throw new Error("canvas not found");
  }

  const x = Math.max(0, region.x);
  const y = Math.max(0, region.y);
  const width = Math.max(1, Math.min(region.width, canvasBox.width - x));
  const height = Math.max(1, Math.min(region.height, canvasBox.height - y));
  const image = decodePng(
    await page.screenshot({
      clip: { x: canvasBox.x + x, y: canvasBox.y + y, width, height },
    }),
  );
  return measureTextStyleGlyphs(image);
}

export async function readEditorPixelBounds(
  page: Page,
  region: CssRegion,
  target: EditorPixelTarget,
): Promise<PixelBounds | null> {
  const image = decodePng(await page.screenshot({ clip: region }));
  return findPixelBounds(image, target);
}

export async function readEditorResizePixelBounds(
  page: Page,
  region: CssRegion,
): Promise<{ blue: PixelBounds | null; teal: PixelBounds | null }> {
  const image = decodePng(await page.screenshot({ clip: region }));
  const blue = findPixelBounds(image, "basic-blue");
  const selectionSearchMargin = 32;
  return {
    blue,
    teal: blue === null
      ? findPixelBounds(image, "selection-teal")
      : findPixelBounds(image, "selection-teal", {
        minX: blue.minX - selectionSearchMargin,
        minY: blue.minY - selectionSearchMargin,
        maxX: blue.maxX + selectionSearchMargin,
        maxY: blue.maxY + selectionSearchMargin,
      }),
  };
}
