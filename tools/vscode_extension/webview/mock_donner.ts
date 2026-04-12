/**
 * Mock Donner WASM module — uses the browser's native SVG rendering so the
 * extension can be developed and tested before the real WASM build lands.
 */

let lastError = "";

/**
 * Renders SVG text to an ImageData using the browser's native SVG pipeline.
 * Used as a fallback when the real Donner WASM module is not available.
 */
export async function renderSvgAsync(
  svgText: string,
  width: number,
  height: number,
): Promise<ImageData | null> {
  lastError = "";
  const offscreen = document.createElement("canvas");
  offscreen.width = width;
  offscreen.height = height;
  const ctx = offscreen.getContext("2d");
  if (!ctx) {
    lastError = "Failed to create offscreen 2D context";
    return null;
  }
  ctx.clearRect(0, 0, width, height);

  const parser = new DOMParser();
  const doc = parser.parseFromString(svgText, "image/svg+xml");
  const parseError = doc.querySelector("parsererror");
  if (parseError) {
    lastError = parseError.textContent ?? "SVG parse error";
    return null;
  }

  const blob = new Blob([svgText], { type: "image/svg+xml;charset=utf-8" });
  const url = URL.createObjectURL(blob);

  return new Promise<ImageData | null>((resolve) => {
    const img = new Image();
    img.onload = () => {
      ctx.drawImage(img, 0, 0, width, height);
      URL.revokeObjectURL(url);
      resolve(ctx.getImageData(0, 0, width, height));
    };
    img.onerror = () => {
      URL.revokeObjectURL(url);
      lastError = "Failed to rasterize SVG";
      resolve(null);
    };
    img.src = url;
  });
}

export function getLastError(): string {
  return lastError;
}
