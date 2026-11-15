// The browser-test helpers that presentation-evidence_tests.mjs drives under Node, bundled into one
// CommonJS module by :presentation_evidence_node. Each takes its page, test info or comparator as a
// parameter, so fake pages and fixed records exercise it without a browser.
export * from "./presentation-evidence";
export { hasPresentedBasicShapesHostFrame } from "./basic-shapes-capture-gate";
export {
  captureEditorPage,
  readEditorPixelBoundsFromPng,
  readEditorResizePixelBounds,
} from "./canvas-color-stats";
export { captureFailureCanvasEvidence, kFailureEvidenceBudgetMs } from "./failure-canvas-evidence";
export { compareOverlayBitmap } from "./overlay-bitmap-compare";
