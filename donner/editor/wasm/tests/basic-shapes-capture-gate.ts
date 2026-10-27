import type { Page, Worker } from "@playwright/test";
import { armCanvasCaptureFault } from "./canvas-capture-fault";
import { captureQuiescentCanvasFrame, findCanvasOwnerWorker } from "./surface-frame-probe";

/**
 * Bound \p work by an absolute end on the `performance.now()` clock, failing with \p message once
 * it passes. The capture gate below does not use it.
 */
export async function awaitBefore<T>(
  work: Promise<T>,
  deadlineAtMs: number,
  message = "Basic Shapes canvas GPU completion exceeded the blue-pixel deadline",
): Promise<T> {
  const remainingMs = deadlineAtMs - performance.now();
  if (remainingMs <= 0) {
    throw new Error(message);
  }
  let timer: ReturnType<typeof setTimeout> | undefined;
  return Promise.race([
    work,
    new Promise<never>((_, reject) => {
      timer = setTimeout(() => reject(new Error(message)), remainingMs);
    }),
  ]).finally(() => clearTimeout(timer));
}

/**
 * Take one scored screenshot of \p owner's canvas through the hold/settle/activity gate
 * (\ref captureQuiescentCanvasFrame): it starts only once the GPU has run every canvas write, and
 * is accepted only if no canvas frame was acquired or written until it ended. Its only time bound
 * is the stall guard, or the caller's own stall guard end `untilMs` on the `performance.now()`
 * clock. Applies the env-gated faults of canvas-capture-fault.ts when they are set.
 */
export async function captureSettledCanvas<T>(
  page: Page,
  owner: Worker,
  capture: (timeoutMs: number) => Promise<T>,
  options: { timedOut?: (result: T) => boolean; untilMs?: number; buttonDown?: boolean } = {},
): Promise<T> {
  // A caller dragging with the button down must not have the fault move its pointer.
  const fault = await armCanvasCaptureFault(page, owner, { pointerChurn: !options.buttonDown });
  const gate = { timedOut: options.timedOut, untilMs: options.untilMs };
  if (fault === null) return (await captureQuiescentCanvasFrame(owner, capture, gate)).result;
  let result: T;
  try {
    result = (await captureQuiescentCanvasFrame(owner, fault.wrap(capture), gate)).result;
  } catch (error) {
    // The capture's own failure is the result; a failed cleanup must not replace it.
    await fault.finish(false).catch(() => {});
    throw error;
  }
  await fault.finish(true);
  return result;
}

export interface InitialBlueFrameState {
  sampleId: string | null;
  completedResults: number;
  presentedAtMs: number | null;
  renderedFrames: number;
  hostFrames: number | null;
  hostPresented: boolean;
  frameId: number;
  captureId: number;
  completedFrameId: number;
  completedCaptureId: number;
  completedInputRepresented: boolean;
}

export function hasPresentedBasicShapesHostFrame(
  state: InitialBlueFrameState,
  beforeSample: number,
): boolean {
  // Later UI frames may coalesce after this sealed document frame reached the GPU.
  // Match its completion identity rather than the latest host frame's acquisition flag.
  return state.sampleId === "basic-shapes"
    && state.completedResults > beforeSample
    && state.presentedAtMs !== null
    && state.hostFrames !== null
    && state.hostFrames === state.renderedFrames
    && state.frameId > 0
    && state.captureId > 0
    && state.completedFrameId >= state.frameId
    && state.completedCaptureId === state.captureId
    && state.completedInputRepresented;
}

export async function captureReadyBasicShapesFrame<T>(
  page: Page,
  state: InitialBlueFrameState,
  beforeSample: number,
  capture: (timeoutMs: number) => Promise<T>,
  options: {
    onOwner?: (owner: Worker) => void;
    timedOut?: (result: T) => boolean;
    untilMs?: number;
  } = {},
): Promise<T | null> {
  if (!hasPresentedBasicShapesHostFrame(state, beforeSample)) return null;
  const owner = await findCanvasOwnerWorker(page);
  if (owner === null) return null;
  options.onOwner?.(owner);
  return captureSettledCanvas(page, owner, capture, {
    timedOut: options.timedOut,
    untilMs: options.untilMs,
  });
}
