import type { Page } from "@playwright/test";
import { boundBrowserDiagnostic } from "./browser-runtime-diagnostics";
import { scaledMs } from "./test-timing";

// Evidence and diagnosis helpers of browser-presentation-regression.spec.ts that take their page
// as a parameter. They live here so that presentation-evidence_tests.mjs can drive them with fake
// pages and fixed records under Node, with no browser.

export interface SurfaceFrameSnapshot {
  renderedFrames: number;
  hostFrames: number | null;
  acquiredFrames: number | null;
  presentedFrames: number | null;
  lastAcquired: boolean | null;
  lastPresented: boolean | null;
}

export interface ScoredCanvasProbe {
  beforeRead: SurfaceFrameSnapshot;
  afterRead: SurfaceFrameSnapshot;
  opaquePixels: number | null;
  pngBase64: string | null;
}

function snapshotsAgree(left: SurfaceFrameSnapshot, right: SurfaceFrameSnapshot): boolean {
  return left.renderedFrames === right.renderedFrames && left.hostFrames === right.hostFrames
    && left.acquiredFrames === right.acquiredFrames
    && left.presentedFrames === right.presentedFrames
    && left.lastAcquired === right.lastAcquired
    && left.lastPresented === right.lastPresented;
}

export function scoredFrameIsStable(
  beforeScreenshot: SurfaceFrameSnapshot | null,
  canvasProbe: ScoredCanvasProbe | null,
): boolean {
  if (
    beforeScreenshot === null || canvasProbe === null || beforeScreenshot.hostFrames === null
    || beforeScreenshot.acquiredFrames === null || beforeScreenshot.presentedFrames === null
    || beforeScreenshot.lastAcquired === null || beforeScreenshot.lastPresented === null
  ) {
    return false;
  }
  return beforeScreenshot.hostFrames === beforeScreenshot.renderedFrames
    && snapshotsAgree(beforeScreenshot, canvasProbe.beforeRead)
    && snapshotsAgree(canvasProbe.beforeRead, canvasProbe.afterRead);
}

// A hung Gecko evaluation cannot replace the already-attached scored PNG or its assertion.
// The abandoned evaluation is never followed by another page command in this failure path.
// The geometry frame account also bounds its reads with this, and there the capture follows:
// an abandoned evaluation keeps running in the browser, so the bound covers a read that throws
// or is slow, not a browser that has stopped answering page commands.
export async function boundFailureDiagnostic<T>(
  work: Promise<T>,
  timeoutMs = scaledMs(2_000),
): Promise<T | null> {
  return boundBrowserDiagnostic(work, timeoutMs);
}

// Sample the canvas element after the scored screenshot failed, then once more
// after requesting another frame. These observers run at different times:
// a colored direct read beside a black screenshot records a disagreement in
// this window, and a zero after wake may precede the browser's next composite.
// Neither sample alone identifies which GPU or capture stage lost content.
//
// This diagnostic runs only after the original pixel assertion has failed;
// the wake cannot turn that failure into a pass.
export interface PresentedCanvasDiagnosis {
  beforeWake: number;
  afterWake: number;
  framesBeforeWake: number;
  framesAfterWake: number;
  hostBeforeWake: HostPresentationCounters | null;
  hostAfterWake: HostPresentationCounters | null;
  canvasWidth: number;
  canvasHeight: number;
}

export interface HostPresentationCounters {
  frames: number;
  acquiredFrames: number;
  presentedFrames: number;
  lastAcquired: boolean;
  lastPresented: boolean;
  underlaySequence: number;
  underlayHostFrame: number;
  checkerboardDraws: number;
  overviewTileDraws: number;
  activeTileDraws: number;
  underlayDirectMs: number;
  imguiVertices: number;
  imguiIndices: number;
  imguiCommandLists: number;
}

export async function diagnosePostFailureGpuReadback(
  page: Page,
  baselineFailed: boolean,
): Promise<object | null> {
  if (!baselineFailed) return null;
  const diagnosis = await boundFailureDiagnostic(
    (async () => {
      const before = await page.evaluate(() => ({
        hostFrameAtRequest: window.__donnerHostFrameTiming?.frames ?? null,
        renderedFramesAtRequest: window.__donnerMainLoopRenderedFrames || 0,
        request: window.__donnerRequestWgpuReadback?.() ?? 0,
      }));
      if (before.request <= 0) {
        return { unavailable: "WGPU readback request hook unavailable", ...before };
      }
      let completion = "completed";
      try {
        await page.waitForFunction(
          (requestId) => Number(window.__donnerWgpuReadbackCompleted || 0) >= requestId,
          before.request,
          { timeout: scaledMs(4_000) },
        );
      } catch {
        completion = "request did not complete";
      }
      const after = await page.evaluate(() => ({
        hostFrameAtRead: window.__donnerHostFrameTiming?.frames ?? null,
        renderedFramesAtRead: window.__donnerMainLoopRenderedFrames || 0,
        underlaySequenceAtRead: window.__donnerHostFrameTiming?.lastUnderlaySequence ?? null,
        stats: window.__donnerWgpuReadbackStats ?? null,
        starts: window.__donnerWgpuReadbackCaptureStarts || 0,
        completions: window.__donnerWgpuReadbackCaptureCompletions || 0,
        failures: window.__donnerWgpuReadbackCaptureFailures || 0,
        completedRequest: window.__donnerWgpuReadbackCompleted || 0,
      }));
      return {
        frameRelation: "later than the retained scored screenshot",
        ...before,
        completion,
        ...after,
      };
    })(),
    scaledMs(5_000),
  );
  return diagnosis ?? { unavailable: "post-failure WGPU readback unavailable" };
}

export async function diagnosePresentedCanvas(
  page: Page,
): Promise<PresentedCanvasDiagnosis | { unavailable: string }> {
  const diagnosis = await boundFailureDiagnostic(
    page.evaluate(async () => {
      const surface = document.getElementById("canvas") as HTMLCanvasElement | null;
      if (surface === null) {
        throw new Error("the editor canvas is missing");
      }
      const scratch = document.createElement("canvas");
      scratch.width = 240;
      scratch.height = 150;
      const context = scratch.getContext("2d", { willReadFrequently: true });
      if (context === null) {
        throw new Error("no 2d context for the canvas probe");
      }
      const hostCounters = (): HostPresentationCounters | null => {
        const host = window.__donnerHostFrameTiming;
        return host === undefined ? null : {
          frames: host.frames,
          acquiredFrames: host.surfaceAcquiredFrames,
          presentedFrames: host.surfacePresentedFrames,
          lastAcquired: host.lastSurfaceAcquired,
          lastPresented: host.lastSurfacePresented,
          underlaySequence: host.lastUnderlaySequence ?? 0,
          underlayHostFrame: host.lastUnderlayHostFrame ?? 0,
          checkerboardDraws: host.lastCheckerboardDraws ?? 0,
          overviewTileDraws: host.lastOverviewTileDraws ?? 0,
          activeTileDraws: host.lastActiveTileDraws ?? 0,
          underlayDirectMs: host.lastUnderlayDirectMs ?? 0,
          imguiVertices: host.lastImguiVertices ?? 0,
          imguiIndices: host.lastImguiIndices ?? 0,
          imguiCommandLists: host.lastImguiCommandLists ?? 0,
        };
      };
      // Count pixels that carry both coverage and hue, so the editor's own
      // chrome registers while the flat page background does not.
      const readColoredPixels = (): number => {
        context.clearRect(0, 0, scratch.width, scratch.height);
        context.drawImage(
          surface,
          0,
          0,
          surface.width,
          surface.height,
          0,
          0,
          scratch.width,
          scratch.height,
        );
        const pixels = context.getImageData(0, 0, scratch.width, scratch.height).data;
        let colored = 0;
        for (let index = 0; index < pixels.length; index += 4) {
          const red = pixels[index];
          const green = pixels[index + 1];
          const blue = pixels[index + 2];
          const alpha = pixels[index + 3];
          const spread = Math.max(red, green, blue) - Math.min(red, green, blue);
          if (alpha >= 40 && spread >= 12) {
            colored += 1;
          }
        }
        return colored;
      };

      const framesBeforeWake = window.__donnerMainLoopRenderedFrames || 0;
      const hostBeforeWake = hostCounters();
      const beforeWake = readColoredPixels();

      // Ask the editor for one frame through the same flag the page uses, then
      // wait for the loop to report it before reading again.
      window.__donnerEditorFrameRequested = true;
      const deadline = performance.now() + 2000;
      while (
        (window.__donnerMainLoopRenderedFrames || 0) === framesBeforeWake
        && performance.now() < deadline
      ) {
        await new Promise((resolve) => requestAnimationFrame(() => resolve(null)));
      }
      await new Promise((resolve) =>
        requestAnimationFrame(() => requestAnimationFrame(() => resolve(null)))
      );

      return {
        beforeWake,
        afterWake: readColoredPixels(),
        framesBeforeWake,
        framesAfterWake: window.__donnerMainLoopRenderedFrames || 0,
        hostBeforeWake,
        hostAfterWake: hostCounters(),
        canvasWidth: surface.width,
        canvasHeight: surface.height,
      };
    }),
    scaledMs(4_000),
  );
  return diagnosis ?? { unavailable: "canvas diagnosis unavailable" };
}

export async function diagnoseCanvasAfterState(
  page: Page,
  baselineFailed: boolean,
  stateAvailable: boolean,
): Promise<PresentedCanvasDiagnosis | { unavailable: string } | null> {
  if (!baselineFailed) return null;
  if (!stateAvailable) {
    return { unavailable: "canvas diagnosis skipped after state IPC unavailable" };
  }
  return diagnosePresentedCanvas(page);
}

// The application's own account of its canvas frames, read immediately before and after one
// capture. `uiVertices` is the UI vertex count the latest host frame recorded. `probe` is the
// canvas owner's surface frame probe; null when it is not installed or did not answer, which
// leaves the account incomplete. Its `frames` counts the owner's canvas acquisitions and
// `newestLateFrame` is the highest acquisition ordinal written after its acquiring task ended, on
// the same counter, so the two are equal when the newest acquired frame was written late.
export interface CanvasCaptureEvidence {
  surface: SurfaceFrameSnapshot;
  uiVertices: number | null;
  completedResults: number;
  presentedAtMs: number | null;
  probe: {
    canvasWorkers: number;
    frames: number;
    inTaskSubmits: number;
    lateSubmits: number;
    newestLateFrame: number | null;
  } | null;
}

// A probe that saw frames but none of their in-task writes cannot vouch for late writes either.
function frameAccountIsComplete(evidence: CanvasCaptureEvidence): boolean {
  return evidence.surface.acquiredFrames !== null && evidence.surface.presentedFrames !== null
    && evidence.probe?.canvasWorkers === 1 && evidence.probe.inTaskSubmits > 0;
}

// A late write never reaches the screen, so a newest frame written late leaves an empty or
// partial frame on screen even when it finished before the first read.
function newestCanvasFrameWasWrittenLate(evidence: CanvasCaptureEvidence): boolean {
  return evidence.probe !== null && evidence.probe.newestLateFrame === evidence.probe.frames;
}

// Why the editor, rather than the browser's capture, could explain an empty picture between the
// `before` and `after` reads, or null when its account is complete and shows only drawn frames.
// Every editor frame records the menu bar and panels, so a host frame with no UI vertices recorded
// no editor UI. This classifies evidence for a reader of a failed run; it decides nothing.
export function canvasCaptureImplicatesApplication(
  before: CanvasCaptureEvidence,
  after: CanvasCaptureEvidence,
): string | null {
  if (!frameAccountIsComplete(before) || !frameAccountIsComplete(after)) {
    return "the application's frame account is incomplete";
  }
  const acquired = after.surface.acquiredFrames! - before.surface.acquiredFrames!;
  const presented = after.surface.presentedFrames! - before.surface.presentedFrames!;
  if (acquired > presented) {
    return "a frame acquired the canvas without completing its draw";
  }
  if (before.surface.lastPresented !== true || after.surface.lastPresented !== true) {
    return "the latest host frame did not complete its presentation";
  }
  if ((before.uiVertices ?? 0) <= 0 || (after.uiVertices ?? 0) <= 0) {
    return "the latest host frame recorded no editor UI";
  }
  if (newestCanvasFrameWasWrittenLate(before) || newestCanvasFrameWasWrittenLate(after)) {
    return "the newest canvas frame was written after its acquiring task ended";
  }
  if (after.probe!.lateSubmits > before.probe!.lateSubmits) {
    return "a frame wrote to the canvas after its acquiring task ended";
  }
  return null;
}

// What the account records in place of a read that failed or outlasted its bound.
export interface UnavailableCanvasAccount {
  unavailable: string;
}

export const kUnavailableCanvasAccount: UnavailableCanvasAccount = {
  unavailable: "the frame account read failed or exceeded its bound",
};

export interface CanvasFrameAccountRecorder {
  read: () => Promise<CanvasCaptureEvidence>;
  attach: (json: string) => Promise<void>;
  boundMs: number;
}

async function readCanvasAccountOrUnavailable(
  recorder: CanvasFrameAccountRecorder,
): Promise<CanvasCaptureEvidence | UnavailableCanvasAccount> {
  const evidence = await boundFailureDiagnostic(
    Promise.resolve().then(recorder.read),
    recorder.boundMs,
  );
  return evidence ?? kUnavailableCanvasAccount;
}

function classifyCanvasAccount(
  before: CanvasCaptureEvidence | UnavailableCanvasAccount,
  after: CanvasCaptureEvidence | UnavailableCanvasAccount,
): string | null {
  if ("unavailable" in before || "unavailable" in after) {
    return "the application's frame account is unavailable";
  }
  return canvasCaptureImplicatesApplication(before, after);
}

// Take one capture between two reads of the editor's frame account and attach the account. Every
// read and the attach are bounded and caught, so the account can only be recorded as unavailable;
// the capture itself runs outside them and fails or returns exactly as it would without them.
export async function captureWithCanvasAccount(
  recorder: CanvasFrameAccountRecorder,
  capture: () => Promise<Buffer>,
): Promise<Buffer> {
  const before = await readCanvasAccountOrUnavailable(recorder);
  const png = await capture();
  const after = await readCanvasAccountOrUnavailable(recorder);
  const applicationImplication = classifyCanvasAccount(before, after);
  const json = JSON.stringify({ before, after, applicationImplication }, null, 2);
  await boundFailureDiagnostic(
    Promise.resolve().then(() => recorder.attach(json)),
    recorder.boundMs,
  );
  return png;
}

// The presentation queue, viewport and host frame fields a zoom completion is judged by; the
// browser publishes them as __donnerPresentationQueueStats, __donnerViewportStats and
// __donnerHostFrameTiming.
export type CompletedZoomState = {
  queue?: {
    captureId: number;
    inputRepresented: boolean;
    deviceLost?: boolean;
    viewportZoom: number;
  };
  viewport?: { zoom: number };
  host?: { lastSurfacePresented: boolean };
};

export function completedFrameMatchesCamera(state: CompletedZoomState): boolean {
  if (!state.queue || !state.viewport || !state.host?.lastSurfacePresented) return false;
  // Canvas-size commits change scene revision while retained tiles still cover the new camera.
  return state.queue.captureId > 0 && state.queue.deviceLost !== true
    && Math.abs(state.queue.viewportZoom - state.viewport.zoom) < 0.001;
}
