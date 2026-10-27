import type { Page, Worker } from "@playwright/test";
import { armCanvasCaptureFault } from "./canvas-capture-fault";
import { captureQuiescentCanvasFrame, findCanvasOwnerWorker } from "./surface-frame-probe";

/**
 * Bound \p work by an absolute deadline, failing with the blue-pixel deadline message once it
 * passes. The capture gate below has no such deadline; this remains for callers that bound their
 * own diagnostics.
 */
export async function awaitBeforeBlueDeadline<T>(
  work: Promise<T>,
  deadlineAtMs: number,
): Promise<T> {
  const remainingMs = deadlineAtMs - performance.now();
  if (remainingMs <= 0) {
    throw new Error("Basic Shapes canvas GPU completion exceeded the blue-pixel deadline");
  }
  let timer: ReturnType<typeof setTimeout> | undefined;
  return Promise.race([
    work,
    new Promise<never>((_, reject) => {
      timer = setTimeout(
        () =>
          reject(new Error("Basic Shapes canvas GPU completion exceeded the blue-pixel deadline")),
        remainingMs,
      );
    }),
  ]).finally(() => clearTimeout(timer));
}

/**
 * Take one scored screenshot of \p owner's canvas through the hold/settle/activity gate
 * (\ref captureQuiescentCanvasFrame): it starts only once the GPU has run every canvas write, and
 * is accepted only if no canvas frame was acquired or written until it ended. The stall guard is
 * its only time bound. Applies the env-gated faults of canvas-capture-fault.ts when they are set.
 */
export async function captureSettledCanvas<T>(
  page: Page,
  owner: Worker,
  capture: (timeoutMs: number) => Promise<T>,
  timedOut?: (result: T) => boolean,
): Promise<T> {
  const fault = await armCanvasCaptureFault(page, owner);
  if (fault === null) return (await captureQuiescentCanvasFrame(owner, capture, { timedOut })).result;
  let result: T;
  try {
    result = (await captureQuiescentCanvasFrame(owner, fault.wrap(capture), { timedOut })).result;
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
  options: { onOwner?: (owner: Worker) => void; timedOut?: (result: T) => boolean } = {},
): Promise<T | null> {
  if (!hasPresentedBasicShapesHostFrame(state, beforeSample)) return null;
  const owner = await findCanvasOwnerWorker(page);
  if (owner === null) return null;
  options.onOwner?.(owner);
  return captureSettledCanvas(page, owner, capture, options.timedOut);
}
