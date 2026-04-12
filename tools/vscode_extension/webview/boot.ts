import { renderSvgAsync, getLastError } from "./mock_donner";

// Acquire the VS Code webview API.
const vscode = acquireVsCodeApi();

// DOM elements.
const canvasContainer = document.getElementById("canvas-container") as HTMLDivElement;
const canvas = document.getElementById("preview-canvas") as HTMLCanvasElement;
const ctx = canvas.getContext("2d")!;
const errorOverlay = document.getElementById("error-overlay") as HTMLDivElement;
const errorMessage = document.getElementById("error-message") as HTMLParagraphElement;
const dirtyBadge = document.getElementById("dirty-badge") as HTMLSpanElement;
const zoomLabel = document.getElementById("zoom-label") as HTMLSpanElement;
const zoomFitBtn = document.getElementById("zoom-fit") as HTMLButtonElement;
const zoomResetBtn = document.getElementById("zoom-reset") as HTMLButtonElement;

// State.
let currentSvgText = "";
let zoomLevel = 1.0;
let panX = 0;
let panY = 0;
let isPanning = false;
let panStartX = 0;
let panStartY = 0;
let themeKind = 2; // Default to dark.

declare function acquireVsCodeApi(): {
  postMessage(msg: unknown): void;
  getState(): unknown;
  setState(state: unknown): void;
};

// --- Rendering ---

async function render(): Promise<void> {
  if (!currentSvgText) return;

  const containerWidth = canvasContainer.clientWidth || 800;
  const containerHeight = canvasContainer.clientHeight || 600;
  const renderWidth = Math.round(containerWidth * window.devicePixelRatio);
  const renderHeight = Math.round(containerHeight * window.devicePixelRatio);

  canvas.width = renderWidth;
  canvas.height = renderHeight;
  canvas.style.width = `${containerWidth}px`;
  canvas.style.height = `${containerHeight}px`;

  // Clear with theme-appropriate background.
  ctx.fillStyle = themeKind === 1 ? "#ffffff" : themeKind === 3 ? "#000000" : "#1e1e1e";
  ctx.fillRect(0, 0, renderWidth, renderHeight);

  try {
    const imageData = await renderSvgAsync(currentSvgText, renderWidth, renderHeight);
    if (!imageData) {
      const err = getLastError();
      showError(err || "Unknown rendering error");
      return;
    }

    ctx.save();
    ctx.setTransform(zoomLevel, 0, 0, zoomLevel, panX, panY);
    ctx.clearRect(0, 0, renderWidth / zoomLevel, renderHeight / zoomLevel);
    ctx.fillStyle = themeKind === 1 ? "#ffffff" : themeKind === 3 ? "#000000" : "#1e1e1e";
    ctx.fillRect(0, 0, renderWidth / zoomLevel, renderHeight / zoomLevel);
    ctx.putImageData(imageData, 0, 0);
    ctx.restore();

    hideError();
  } catch (e: unknown) {
    showError(e instanceof Error ? e.message : String(e));
  }
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
  const msg = event.data;
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
  zoomLevel = 1.0;
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

// --- Resize ---

const resizeObserver = new ResizeObserver(() => {
  render();
});
resizeObserver.observe(canvasContainer);
