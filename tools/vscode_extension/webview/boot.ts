import { renderSvgAsync as mockRenderSvgAsync, getLastError as mockGetLastError } from "./mock_donner";

// Acquire the VS Code webview API.
const vscode = acquireVsCodeApi();

// DOM elements.
const canvasContainer = document.getElementById("canvas-container") as HTMLDivElement;
const canvas = document.getElementById("preview-canvas") as HTMLCanvasElement;
const ctx = canvas.getContext("2d");
if (!ctx) {
  const msg = document.createElement("p");
  msg.textContent = "Error: Unable to create canvas 2D context. The preview cannot render.";
  msg.style.color = "red";
  msg.style.padding = "1rem";
  canvasContainer.replaceChildren(msg);
  throw new Error("Canvas 2D context unavailable");
}
const errorOverlay = document.getElementById("error-overlay") as HTMLDivElement;
const errorMessage = document.getElementById("error-message") as HTMLParagraphElement;
const dirtyBadge = document.getElementById("dirty-badge") as HTMLSpanElement;
const zoomLabel = document.getElementById("zoom-label") as HTMLSpanElement;
const zoomFitBtn = document.getElementById("zoom-fit") as HTMLButtonElement;
const zoomResetBtn = document.getElementById("zoom-reset") as HTMLButtonElement;

// State.
let currentSvgText = "";
let renderGeneration = 0;
let zoomLevel = 1.0;
let panX = 0;
let panY = 0;
let isPanning = false;
let panStartX = 0;
let panStartY = 0;
let themeKind = 2; // Default to dark.

// Offscreen canvas used to hold the rendered ImageData so that drawImage
// respects the visible canvas's current transform (zoom/pan).
const offscreenCanvas = document.createElement("canvas");
const offscreenCtx = offscreenCanvas.getContext("2d")!;

// The last rendered image dimensions, used for "Fit" calculations.
let lastRenderWidth = 0;
let lastRenderHeight = 0;

declare function acquireVsCodeApi(): {
  postMessage(msg: unknown): void;
  getState(): unknown;
  setState(state: unknown): void;
};

// --- WASM module loading ---

interface DonnerWasmModule {
  cwrap: (name: string, returnType: string | null, argTypes: string[]) => Function;
  HEAPU8: Uint8Array;
}

let wasmModule: DonnerWasmModule | null = null;
let wasmRenderSvg: ((svgText: string, w: number, h: number) => number) | null = null;
let wasmFreePixels: ((ptr: number) => void) | null = null;
let wasmGetLastError: (() => string) | null = null;

async function tryLoadWasm(): Promise<boolean> {
  try {
    // The WASM loader is staged into media/generated/ by stage_wasm_assets.sh
    // and served as a webview resource. The esbuild config marks it external.
    const scriptTags = document.querySelectorAll("script[src]");
    let baseUrl = "";
    for (const tag of scriptTags) {
      const src = (tag as HTMLScriptElement).src;
      if (src.includes("boot.js")) {
        baseUrl = src.substring(0, src.lastIndexOf("/"));
        break;
      }
    }

    // Resolve the WASM loader relative to the webview dist directory.
    // The extension host serves media/generated/ alongside dist/webview/.
    const loaderUrl = baseUrl
      ? baseUrl.replace(/\/dist\/webview\/?$/, "/media/generated/donner_wasm_bin.js")
      : "";
    if (!loaderUrl) return false;

    // Attempt to fetch the loader to verify it exists before importing.
    const probe = await fetch(loaderUrl, { method: "HEAD" });
    if (!probe.ok) return false;

    const createModule = (await import(/* webpackIgnore: true */ loaderUrl)).default as (
      opts?: object,
    ) => Promise<DonnerWasmModule>;
    wasmModule = await createModule();

    const init = wasmModule.cwrap("donner_init", null, []) as () => void;
    wasmRenderSvg = wasmModule.cwrap("donner_render_svg", "number", [
      "string",
      "number",
      "number",
    ]) as (svgText: string, w: number, h: number) => number;
    wasmFreePixels = wasmModule.cwrap("donner_free_pixels", null, ["number"]) as (
      ptr: number,
    ) => void;
    wasmGetLastError = wasmModule.cwrap("donner_get_last_error", "string", []) as () => string;

    init();
    return true;
  } catch {
    // WASM not available — fall back to mock renderer.
    wasmModule = null;
    return false;
  }
}

/**
 * Renders SVG text via the real WASM module if loaded, otherwise delegates to
 * the browser-based mock renderer.
 */
async function renderSvg(
  svgText: string,
  width: number,
  height: number,
): Promise<{ imageData: ImageData | null; error: string }> {
  const MAX_DIM = 8192;
  const cappedWidth = Math.min(Math.max(1, Math.round(width)), MAX_DIM);
  const cappedHeight = Math.min(Math.max(1, Math.round(height)), MAX_DIM);

  if (wasmModule && wasmRenderSvg && wasmFreePixels && wasmGetLastError) {
    const ptr = wasmRenderSvg(svgText, cappedWidth, cappedHeight);
    if (ptr === 0) {
      return { imageData: null, error: wasmGetLastError() || "Unknown rendering error" };
    }
    const numBytes = cappedWidth * cappedHeight * 4;
    const pixels = new Uint8ClampedArray(wasmModule.HEAPU8.buffer, ptr, numBytes);
    // Copy before freeing the WASM-side buffer.
    const imageData = new ImageData(new Uint8ClampedArray(pixels), cappedWidth, cappedHeight);
    wasmFreePixels(ptr);
    return { imageData, error: "" };
  }

  // Fallback: mock renderer (browser-native SVG rasterisation).
  const imageData = await mockRenderSvgAsync(svgText, cappedWidth, cappedHeight);
  return { imageData, error: imageData ? "" : mockGetLastError() || "Unknown rendering error" };
}

// Kick off WASM loading immediately; rendering works either way.
tryLoadWasm().catch(() => {
  /* non-fatal */
});

// --- Rendering ---

function render(): void {
  if (!currentSvgText) return;

  const generation = ++renderGeneration;

  const containerWidth = canvasContainer.clientWidth || 800;
  const containerHeight = canvasContainer.clientHeight || 600;
  const renderWidth = Math.round(containerWidth * window.devicePixelRatio);
  const renderHeight = Math.round(containerHeight * window.devicePixelRatio);

  lastRenderWidth = renderWidth;
  lastRenderHeight = renderHeight;

  canvas.width = renderWidth;
  canvas.height = renderHeight;
  canvas.style.width = `${containerWidth}px`;
  canvas.style.height = `${containerHeight}px`;

  const bgColor = themeKind === 1 ? "#ffffff" : themeKind === 3 ? "#000000" : "#1e1e1e";

  // Clear with theme-appropriate background.
  ctx.fillStyle = bgColor;
  ctx.fillRect(0, 0, renderWidth, renderHeight);

  renderSvg(currentSvgText, renderWidth, renderHeight)
    .then(({ imageData, error }) => {
      // A newer render was started — discard this result.
      if (generation !== renderGeneration) return;

      if (!imageData) {
        showError(error);
        return;
      }

      // Place rendered pixels onto the offscreen canvas.
      offscreenCanvas.width = renderWidth;
      offscreenCanvas.height = renderHeight;
      offscreenCtx.putImageData(imageData, 0, 0);

      // Draw onto the visible canvas with zoom/pan transform applied.
      ctx.save();
      ctx.setTransform(zoomLevel, 0, 0, zoomLevel, panX, panY);
      ctx.clearRect(0, 0, renderWidth / zoomLevel, renderHeight / zoomLevel);
      ctx.fillStyle = bgColor;
      ctx.fillRect(0, 0, renderWidth / zoomLevel, renderHeight / zoomLevel);
      ctx.drawImage(offscreenCanvas, 0, 0);
      ctx.restore();

      hideError();
    })
    .catch((e: unknown) => {
      showError(e instanceof Error ? e.message : String(e));
    });
}

function showError(msg: string): void {
  errorMessage.textContent = msg;
  errorOverlay.classList.remove("hidden");
}

function hideError(): void {
  errorOverlay.classList.add("hidden");
}

function updateZoomLabel(): void {
  zoomLabel.textContent = `${Math.round(zoomLevel * 100)}%`;
}

// --- Message handling ---

window.addEventListener("message", (event) => {
  if (event.origin !== "vscode-webview:" && event.origin !== "") return;

  const msg = event.data;
  if (!msg || typeof msg.type !== "string") return;

  switch (msg.type) {
    case "initDocument":
      currentSvgText = msg.body.svgText;
      dirtyBadge.classList.add("hidden");
      render();
      break;

    case "replaceDocumentSnapshot":
      currentSvgText = msg.body.svgText;
      dirtyBadge.classList.add("hidden");
      render();
      break;

    case "setDirtyState":
      if (msg.body.dirty) {
        dirtyBadge.classList.remove("hidden");
      } else {
        dirtyBadge.classList.add("hidden");
      }
      break;

    case "themeChanged":
      themeKind = msg.body.kind;
      render();
      break;

    case "error":
      showError(msg.body);
      break;
  }
});

// --- Zoom (mouse wheel) ---

canvasContainer.addEventListener(
  "wheel",
  (e) => {
    e.preventDefault();
    const delta = e.deltaY > 0 ? 0.9 : 1.1;
    zoomLevel = Math.max(0.1, Math.min(20, zoomLevel * delta));
    updateZoomLabel();
    render();
  },
  { passive: false },
);

// --- Pan (mouse drag) ---

canvasContainer.addEventListener("mousedown", (e) => {
  if (e.button === 0) {
    isPanning = true;
    panStartX = e.clientX - panX;
    panStartY = e.clientY - panY;
    canvasContainer.style.cursor = "grabbing";
  }
});

window.addEventListener("mousemove", (e) => {
  if (!isPanning) return;
  panX = e.clientX - panStartX;
  panY = e.clientY - panStartY;
  render();
});

window.addEventListener("mouseup", () => {
  isPanning = false;
  canvasContainer.style.cursor = "grab";
});

// --- Toolbar buttons ---

zoomFitBtn.addEventListener("click", () => {
  // Compute a zoom level that fits the rendered SVG within the viewport.
  const containerWidth = canvasContainer.clientWidth || 800;
  const containerHeight = canvasContainer.clientHeight || 600;
  const imgWidth = lastRenderWidth / window.devicePixelRatio || containerWidth;
  const imgHeight = lastRenderHeight / window.devicePixelRatio || containerHeight;
  zoomLevel = Math.min(containerWidth / imgWidth, containerHeight / imgHeight, 1.0);
  panX = 0;
  panY = 0;
  updateZoomLabel();
  render();
});

zoomResetBtn.addEventListener("click", () => {
  zoomLevel = 1.0;
  panX = 0;
  panY = 0;
  updateZoomLabel();
  render();
});

// --- Resize (debounced) ---

let resizeTimer: ReturnType<typeof setTimeout> | null = null;

const resizeObserver = new ResizeObserver(() => {
  if (resizeTimer !== null) clearTimeout(resizeTimer);
  resizeTimer = setTimeout(() => {
    resizeTimer = null;
    render();
  }, 100);
});
resizeObserver.observe(canvasContainer);
