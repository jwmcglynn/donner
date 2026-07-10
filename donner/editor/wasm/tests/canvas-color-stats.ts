import type { Page } from "@playwright/test";
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
    region: { x, y, width: image.width, height: image.height },
  };
}
