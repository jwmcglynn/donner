/**
 * Mock Donner WASM module — uses the browser's native SVG rendering so the
 * extension can be developed and tested before the real WASM build (WP1) lands.
 */

export interface DonnerWasm {
  donner_init(): void;
  donner_render_svg(svgText: string, width: number, height: number): ImageData | null;
  donner_free_pixels(ptr: number): void;
  donner_get_last_error(): string;
}

let lastError = "";

function createMockWasm(): DonnerWasm {
  const offscreen = document.createElement("canvas");
  const offCtx = offscreen.getContext("2d")!;

  return {
    donner_init() {},

    donner_render_svg(svgText: string, width: number, height: number): ImageData | null {
      lastError = "";
      try {
        const parser = new DOMParser();
        const doc = parser.parseFromString(svgText, "image/svg+xml");
        const parseError = doc.querySelector("parsererror");
        if (parseError) {
          lastError = parseError.textContent ?? "SVG parse error";
          return null;
        }

        offscreen.width = width;
        offscreen.height = height;
        offCtx.clearRect(0, 0, width, height);

        const blob = new Blob([svgText], { type: "image/svg+xml;charset=utf-8" });
        const url = URL.createObjectURL(blob);

        return new Promise<ImageData | null>((resolve) => {
          const img = new Image();
          img.onload = () => {
            offCtx.drawImage(img, 0, 0, width, height);
            URL.revokeObjectURL(url);
            resolve(offCtx.getImageData(0, 0, width, height));
          };
          img.onerror = () => {
            URL.revokeObjectURL(url);
            lastError = "Failed to rasterize SVG";
            resolve(null);
          };
          img.src = url;
        }) as unknown as ImageData | null;
      } catch (e: unknown) {
        lastError = e instanceof Error ? e.message : String(e);
        return null;
      }
    },

    donner_free_pixels(_ptr: number) {},

    donner_get_last_error(): string {
      return lastError;
    },
  };
}

/**
 * Async rendering helper that wraps the mock's Image-loading pipeline.
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
  const ctx = offscreen.getContext("2d")!;
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

export function createDonnerWasm(): DonnerWasm {
  return createMockWasm();
}
