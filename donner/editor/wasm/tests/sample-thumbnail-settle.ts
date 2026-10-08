import { expect, type Page } from "@playwright/test";

/** The thumbnail-lane state the sample picker publishes, as far as the settle gate reads it. */
interface SampleThumbnailLaneStats {
  completed?: number;
  ready?: number;
  active?: boolean;
  pending?: boolean;
  resultReady?: boolean;
  drained?: boolean;
}

/** The worker health fields printed beside the lane, in the presentation suite's vocabulary. */
interface WorkerHealthStats {
  completedResults?: number;
  deviceLost?: boolean;
  gpuWaitTimeoutSite?: string;
  publishReason?: string;
}

/**
 * Wait until the sample picker's thumbnail lane has drained: every sample the picker shows has a
 * finished thumbnail attempt, at least one thumbnail is on screen, and no attempt is queued,
 * running or waiting to be polled.
 *
 * The picker renders the visible samples' thumbnails one after another on the raster worker, and
 * the first one builds and first uses the worker's offscreen WebGPU renderer. A sample click while
 * that lane is busy queues the document render behind the attempt in flight: the click cancels the
 * attempt, but the worker takes the document render only once the attempt returns, and on a cold
 * browser that first-use work has outlasted a document render's whole budget. Once the lane has
 * drained, the document render that follows starts on a worker with no thumbnail work ahead of
 * it, and every visible sample has already been drawn once on that worker's device.
 *
 * The idle counters alone do not establish that. Between two attempts the lane reads idle for part
 * of a frame, and a thumbnail set aside for a font load reads idle until its retry, so the gate
 * requires the editor's own `drained` flag (EditorShell::publishSampleThumbnailStats). A newly
 * visible sample or a later font identity change can still reopen the lane after this returns.
 *
 * Callers that are not testing the thumbnail-to-document handoff wait here before they click a
 * sample, so their deadline measures only the work they are testing. That leaves a gap: the
 * handoff regression in smoke.spec.ts forces the collision only in Firefox, with renderer
 * construction held, and the WebKit carousel case there runs in an already-warm browser, so no CI
 * gate measures how long a cold WebKit page takes to present its first sample.
 *
 * Most callers pass `scaledMs(20_000)`, 80 s on CI. For the presentation, smoke and
 * surface-recovery callers that is longer than the test deadline (30 s, or 90 s for a slow test or
 * the Firefox project), so there the deadline is the effective cap. That is deliberate: how long
 * the lane takes to drain on a cold hosted runner has not been measured, and when the deadline
 * fires first Playwright still reports this poll's message and last snapshot. The composited
 * suites run under 120-480 s deadlines on CI, so for them this bound applies first. The
 * backend-selection test passes an unscaled 20 s inside its 60 s deadline.
 *
 * The polled value carries the lane stats and the worker's health, so a lane that never drains
 * prints which counter stalled and whether the worker reported a lost device or a GPU wait that
 * gave up.
 *
 * @param page The page with the editor open on the sample picker.
 * @param options The failure message and the wait's bound.
 */
export async function expectSampleThumbnailsToSettle(
  page: Page,
  options: { message: string; timeout: number },
): Promise<void> {
  await expect
    .poll(
      () =>
        page.evaluate(() => {
          const state = window as Window & {
            __donnerSampleThumbnailStats?: SampleThumbnailLaneStats;
            __donnerWorkerStats?: WorkerHealthStats;
            __donnerMainLoopRenderedFrames?: number;
          };
          const stats = state.__donnerSampleThumbnailStats;
          const worker = state.__donnerWorkerStats;
          return {
            settled: !!stats && stats.drained === true && (stats.completed ?? 0) > 0
              && (stats.ready ?? 0) > 0 && !stats.active && !stats.pending && !stats.resultReady,
            sampleThumbnail: stats ?? null,
            worker: {
              completedResults: worker?.completedResults ?? 0,
              deviceLost: worker?.deviceLost ?? false,
              gpuWaitTimeoutSite: worker?.gpuWaitTimeoutSite ?? "unpublished",
              publishReason: worker?.publishReason ?? "unpublished",
            },
            renderedFrames: state.__donnerMainLoopRenderedFrames ?? 0,
          };
        }),
      { message: options.message, timeout: options.timeout, intervals: [16, 25, 50, 100] },
    )
    .toEqual(expect.objectContaining({ settled: true }));
}
