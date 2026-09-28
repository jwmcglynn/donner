import type { Page, Worker } from "@playwright/test";
import { findCanvasOwnerWorker, waitForSubmittedCanvasGpuWork } from "./surface-frame-probe";

// The app already requests queue completion after each canvas-writing submit.
// This test-only gate awaits that same promise; it neither submits GPU work nor
// requests another frame. A missing observer is retried by the enclosing poll,
// while a rejected or overdue completion fails rather than scoring pixels.
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

export async function captureAfterCanvasGpuCompletion<T>(
  owner: Worker,
  deadlineAtMs: number,
  capture: (remainingMs: number) => Promise<T>,
): Promise<T> {
  await awaitBeforeBlueDeadline(waitForSubmittedCanvasGpuWork(owner), deadlineAtMs);
  const remainingMs = deadlineAtMs - performance.now();
  if (remainingMs < 1) {
    throw new Error("Basic Shapes canvas GPU completion exceeded the blue-pixel deadline");
  }
  const result = await capture(remainingMs);
  if (performance.now() >= deadlineAtMs) {
    throw new Error("Basic Shapes canvas GPU completion exceeded the blue-pixel deadline");
  }
  return result;
}

export interface InitialBlueFrameState {
  sampleId: string | null;
  completedResults: number;
  presentedAtMs: number | null;
  renderedFrames: number;
  hostFrames: number | null;
  hostPresented: boolean;
}

export function hasPresentedBasicShapesHostFrame(
  state: InitialBlueFrameState,
  beforeSample: number,
): boolean {
  // RunEditorFrame calls endFrame once, and its timing callback is queued
  // before RecordFrameSample. Both counters advance once per active frame,
  // including early surface returns; only the completed draw/present path
  // sets the same frame's lastSurfacePresented flag.
  return state.sampleId === "basic-shapes"
    && state.completedResults > beforeSample
    && state.presentedAtMs !== null
    && state.hostFrames !== null
    && state.hostFrames === state.renderedFrames
    && state.hostPresented;
}

export async function captureReadyBasicShapesFrame<T>(
  page: Page,
  state: InitialBlueFrameState,
  beforeSample: number,
  deadlineAtMs: number,
  capture: (remainingMs: number) => Promise<T>,
  onOwner?: (owner: Worker) => void,
): Promise<T | null> {
  if (!hasPresentedBasicShapesHostFrame(state, beforeSample)) return null;
  const owner = await awaitBeforeBlueDeadline(findCanvasOwnerWorker(page), deadlineAtMs);
  if (owner === null) return null;
  onOwner?.(owner);
  return captureAfterCanvasGpuCompletion(owner, deadlineAtMs, capture);
}
