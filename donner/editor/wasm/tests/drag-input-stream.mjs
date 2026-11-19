/**
 * One measured drag of the responsiveness lane: the input stream it dispatches into the editor
 * canvas, the frames sampled while it runs, and which of its inputs those frames completed.
 *
 * `streamDragInputs` runs in the page. Playwright serializes it, so it reads only its argument
 * and browser globals. It presses at `start`, moves 60 times 16 ms apart, releases, and samples
 * the editor's frame loop and presentation statistics on every animation frame from the press
 * until the frame that represents the release completes, or the release wait ends.
 *
 * The rest runs in Node on the returned stream, so the spec and its Node tests share one reading.
 *
 * Plain JavaScript with no module-only features beyond `export`, so Playwright can load it from a
 * TypeScript spec.
 */

/**
 * @typedef {{ inputRepresented: boolean, mouseDown: boolean, completedSerial: number,
 *   completedAtMs: number, pointerX: number, pointerY: number }} DragPresentation
 *   The editor's completed-frame presentation statistics, as far as a drag reads them.
 * @typedef {{ frame: number, atMs: number, pointerX?: number, pointerY?: number,
 *   selectedCount?: number, dragging?: boolean, workerMs?: number, completedResults?: number,
 *   readbackCount?: unknown, readbackPollIterations?: unknown,
 *   presentation?: DragPresentation }} DragFrame One sampled editor frame.
 * @typedef {{ startedAtMs: number, finishedAtMs: number, dispatchTimes: number[],
 *   dispatchPoints: Array<{ x: number, y: number }>, frames: DragFrame[],
 *   end: { x: number, y: number } }} DragStream One drag's dispatched inputs and sampled frames.
 */

/**
 * Press, drag and release in the page, sampling the editor's frame statistics.
 * @param {{ start: { x: number, y: number }, direction: number }} options Where the drag
 *   starts, and the signed fraction of its 120 by 72 pixel travel.
 * @returns {Promise<DragStream>}
 */
export async function streamDragInputs({ start, direction }) {
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
  const observe = () => {
    const loop = state.__donnerFrameLoopStats;
    const presentation = state.__donnerPresentationQueueStats;
    if (
      loop
      && (loop.renderedFrames !== previousFrame
        || (presentation?.completedSerial ?? 0) !== previousCompletedSerial)
    ) {
      previousCompletedSerial = presentation?.completedSerial ?? 0;
      previousFrame = loop.renderedFrames;
      if (frames.length < 256) {
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
    }
    if (observing) sampleId = requestAnimationFrame(observe);
  };
  const startedAtMs = performance.now();
  observe();
  dispatch("mousedown", start.x, start.y, 1);
  let end = start;
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
    if (state.__donnerPresentationQueueStats) {
      await new Promise((resolve) => {
        const deadline = performance.now() + 500;
        const finish = () => {
          const frame = state.__donnerPresentationQueueStats;
          if (
            (frame?.inputRepresented && !frame.mouseDown && frame.pointerX === end.x
              && frame.pointerY === end.y) || performance.now() >= deadline
          ) resolve();
          else setTimeout(finish, 8);
        };
        finish();
      });
    }
    observe();
    observing = false;
    cancelAnimationFrame(sampleId);
  }
  const stream = {
    startedAtMs,
    finishedAtMs: performance.now(),
    dispatchTimes,
    dispatchPoints,
    frames,
    end,
  };
  state.__donnerLastDragStream = stream;
  return stream;
}

/**
 * Whether a sampled frame is a completed frame of this drag that represents one of its inputs.
 * @param {DragFrame} frame Sampled frame.
 * @param {DragStream} stream The drag's stream.
 * @param {number} baselineSerial Completed serial before the drag started.
 * @returns {boolean}
 */
function completesThisDrag(frame, stream, baselineSerial) {
  return Boolean(
    frame.presentation?.inputRepresented
      && frame.presentation.completedSerial > baselineSerial
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
      && stream.dispatchPoints.some((point) =>
        point.x === frame.presentation.pointerX && point.y === frame.presentation.pointerY
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
