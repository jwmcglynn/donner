/**
 * One measured drag of the responsiveness lane: the input stream it dispatches into the editor
 * canvas, the frames sampled while it runs, and which of its inputs those frames completed.
 *
 * `streamDragInputs` runs in the page. Playwright serializes it, so it reads only its argument
 * and browser globals. It presses at `start`, moves 60 times 16 ms apart, releases, and samples
 * the editor's frame loop and presentation statistics on every animation frame from the press
 * until the release frame completes or the release stall guard, `kReleaseFrameTimeoutMs` after
 * the mouse up, expires. It returns that frame when it completed in time, and null otherwise.
 * The release frame is the only frame certain to represent the final input, and when every held
 * frame lags behind the pointer it is the only one that completes any input. On a slow runner it
 * can complete seconds after the release (#2106); a sampler that stopped observing earlier than
 * the release requirement read a slow but complete drag as one in which no input completed.
 *
 * The rest runs in Node on the returned stream, so the spec and its Node tests share one reading.
 *
 * Plain JavaScript with no module-only features beyond `export`, so Playwright can load it from a
 * TypeScript spec.
 */

/**
 * The release stall guard: how long after the mouse up the frame that represents the release, at
 * the current camera, may take to complete, measured once. It is the only deadline on the release
 * and equals the bound the release had before it was measured once: a 500 ms sampler window, an
 * idle wait of up to 15 s, then a 5 s release poll. It only catches a release that never arrives;
 * how long the release takes is measured by the input latency and stall gates. The sampler
 * observes the drag until then, and the spec fails a drag whose release frame missed it before it
 * reads which inputs completed.
 */
export const kReleaseFrameTimeoutMs = 20500;
export const kPresentationSamplePollMs = 8;

/**
 * @typedef {{ inputRepresented: boolean, mouseDown: boolean, completedSerial: number,
 *   completedAtMs: number, pointerX: number, pointerY: number,
 *   viewportZoom: number }} DragPresentation
 *   The editor's completed-frame presentation statistics, as far as a drag reads them.
 * @typedef {{ frame: number, atMs: number, pointerX?: number, pointerY?: number,
 *   selectedCount?: number, dragging?: boolean, workerMs?: number, completedResults?: number,
 *   readbackCount?: unknown, readbackPollIterations?: unknown,
 *   presentation?: DragPresentation }} DragFrame One sampled editor frame.
 * @typedef {{ startedAtMs: number, finishedAtMs: number, dispatchTimes: number[],
 *   dispatchPoints: Array<{ x: number, y: number }>, frames: DragFrame[],
 *   end: { x: number, y: number }, releasedAtMs: number,
 *   releaseFrame: DragPresentation | null }} DragStream One drag's dispatched inputs, sampled
 *   frames, mouse-up time, and its release frame if that completed by the release deadline.
 */

/**
 * Press, drag and release in the page, sampling the editor's frame statistics.
 * @param {{ start: { x: number, y: number }, direction: number,
 *   releaseFrameTimeoutMs: number, presentationSamplePollMs: number }} options Where the drag
 *   starts, the signed fraction of its
 *   120 by 72 pixel travel, and `kReleaseFrameTimeoutMs`.
 * @returns {Promise<DragStream>}
 */
export async function streamDragInputs({ start, direction, releaseFrameTimeoutMs, presentationSamplePollMs }) {
  const state = window;
  const canvas = document.querySelector("canvas#canvas");
  const dispatch = (type, x, y, buttons) => {
    canvas.dispatchEvent(
      new MouseEvent(type, {
        bubbles: true,
        cancelable: true,
        view: window,
        clientX: x,
        clientY: y,
        button: 0,
        buttons,
      }),
    );
  };
  const frames = [];
  const dispatchTimes = [];
  const dispatchPoints = [];
  let previousCompletedSerial = 0;
  let previousFrame = state.__donnerFrameLoopStats?.renderedFrames ?? 0;
  let observing = true;
  let sampleId = 0;
  let sampleTimeoutId = 0;
  // Samples the current frame when it differs from the last sample. The stream keeps at most 256
  // samples before the final one, which follows the release wait, so it holds the release frame
  // whenever that completed in time.
  const sample = (final) => {
    const loop = state.__donnerFrameLoopStats;
    const presentation = state.__donnerPresentationQueueStats;
    if (
      loop
      && (loop.renderedFrames !== previousFrame
        || (presentation?.completedSerial ?? 0) !== previousCompletedSerial)
      && (frames.length < 256 || final)
    ) {
      previousCompletedSerial = presentation?.completedSerial ?? 0;
      previousFrame = loop.renderedFrames;
      frames.push({
        frame: loop.renderedFrames,
        atMs: loop.lastFrameAtMs,
        pointerX: state.__donnerInteractionStats?.pointerX,
        pointerY: state.__donnerInteractionStats?.pointerY,
        selectedCount: state.__donnerInteractionStats?.selectedCount,
        dragging: state.__donnerInteractionStats?.dragging,
        workerMs: state.__donnerWorkerStats?.workerMs,
        completedResults: state.__donnerWorkerStats?.completedResults,
        presentation,
        readbackCount: state.__donnerWorkerStats?.readbackCount,
        readbackPollIterations: state.__donnerWorkerStats?.readbackPollIterations,
      });
    }
  };
  const observe = () => {
    sample(false);
    if (observing) sampleId = requestAnimationFrame(observe);
  };
  // GPU submission completion is not synchronized to animation frames. In particular, Firefox can
  // publish two completed held-drag presentations between successive rAF callbacks. Poll the
  // bounded completion snapshot as well so a later completion cannot erase evidence of the first.
  const observeCompletion = () => {
    sample(false);
    if (observing) sampleTimeoutId = setTimeout(observeCompletion, presentationSamplePollMs);
  };
  const startedAtMs = performance.now();
  observe();
  observeCompletion();
  dispatch("mousedown", start.x, start.y, 1);
  let end = start;
  let releasedAtMs = 0;
  let releaseFrame = null;
  try {
    await new Promise((resolve) => {
      let step = 0;
      const move = () => {
        ++step;
        const fraction = step / 60;
        end = {
          x: Math.round(start.x + direction * 120 * fraction),
          y: Math.round(start.y + direction * 72 * fraction),
        };
        dispatchTimes.push(performance.now());
        dispatchPoints.push(end);
        dispatch("mousemove", end.x, end.y, 1);
        if (step < 60) setTimeout(move, 16);
        else resolve();
      };
      setTimeout(move, 16);
    });
  } finally {
    dispatch("mouseup", end.x, end.y, 0);
    releasedAtMs = performance.now();
    if (state.__donnerPresentationQueueStats) {
      const deadline = releasedAtMs + releaseFrameTimeoutMs;
      await new Promise((resolve) => {
        const finish = () => {
          const frame = state.__donnerPresentationQueueStats;
          const viewport = state.__donnerViewportStats;
          if (
            frame?.inputRepresented && !frame.mouseDown && frame.pointerX === end.x
            && frame.pointerY === end.y && frame.completedAtMs <= deadline && viewport
            && Math.abs(frame.viewportZoom - viewport.zoom) < 0.001
          ) {
            releaseFrame = { ...frame };
            resolve();
          } else if (performance.now() >= deadline) resolve();
          else setTimeout(finish, 8);
        };
        finish();
      });
    }
    observing = false;
    cancelAnimationFrame(sampleId);
    clearTimeout(sampleTimeoutId);
    sample(true);
  }
  const stream = {
    startedAtMs,
    finishedAtMs: performance.now(),
    dispatchTimes,
    dispatchPoints,
    frames,
    end,
    releasedAtMs,
    releaseFrame,
  };
  state.__donnerLastDragStream = stream;
  return stream;
}

/**
 * Whether a sampled frame completed after this drag began. Callers separately match its pointer
 * to an input dispatched before the completion. The inputRepresented field says whether the
 * presentation caught up to the latest input, so it stays false when a fast held drag completes
 * an earlier dispatched position while newer inputs are queued.
 * @param {DragFrame} frame Sampled frame.
 * @param {DragStream} stream The drag's stream.
 * @param {number} baselineSerial Completed serial before the drag started.
 * @returns {boolean}
 */
function completesThisDrag(frame, stream, baselineSerial) {
  return Boolean(
    frame.presentation?.completedSerial > baselineSerial
      && frame.presentation.completedAtMs >= stream.startedAtMs,
  );
}

/**
 * For each dispatched input, the time from its dispatch to the first sampled frame that completed
 * it or a later input of the drag. An input no sampled frame completed has no latency.
 * @param {DragStream} stream The drag's stream.
 * @param {number} baselineSerial Completed serial before the drag started.
 * @returns {number[]} One latency in milliseconds per completed input, in dispatch order.
 */
export function dragInputLatencies(stream, baselineSerial) {
  return stream.dispatchPoints.flatMap((point, index) => {
    const frame = stream.frames.find((sample) =>
      completesThisDrag(sample, stream, baselineSerial)
      && stream.dispatchPoints.findLastIndex((applied, appliedIndex) =>
          applied.x === sample.presentation.pointerX && applied.y === sample.presentation.pointerY
          && stream.dispatchTimes[appliedIndex] <= sample.presentation.completedAtMs
        ) >= index
      && sample.presentation.completedAtMs >= stream.dispatchTimes[index]
    );
    return frame ? [frame.presentation.completedAtMs - stream.dispatchTimes[index]] : [];
  });
}

/**
 * The sampled frames that completed one of the drag's inputs, one per completed serial, in
 * completion order.
 * @param {DragStream} stream The drag's stream.
 * @param {number} baselineSerial Completed serial before the drag started.
 * @returns {DragFrame[]}
 */
export function completedDragFrames(stream, baselineSerial) {
  const completions = new Map();
  for (const frame of stream.frames) {
    if (
      completesThisDrag(frame, stream, baselineSerial)
      && stream.dispatchPoints.some((point, index) =>
        point.x === frame.presentation.pointerX && point.y === frame.presentation.pointerY
          && stream.dispatchTimes[index] <= frame.presentation.completedAtMs
      )
    ) {
      completions.set(frame.presentation.completedSerial, frame);
    }
  }
  return [...completions.values()];
}

/**
 * How many distinct pointer positions the completed frames represent while the button is held.
 * @param {DragFrame[]} completedFrames Frames from `completedDragFrames`.
 * @returns {number}
 */
export function heldDragPositionCount(completedFrames) {
  return new Set(
    completedFrames.filter((frame) => frame.presentation.mouseDown)
      .map((frame) => `${frame.presentation.pointerX},${frame.presentation.pointerY}`),
  ).size;
}
