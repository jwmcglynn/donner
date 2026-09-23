import type { Page, Worker } from "@playwright/test";

/**
 * A command buffer that wrote into a canvas frame in a later task than the one
 * that acquired the frame.
 *
 * A WebGPU canvas presents its current texture at the rendering update that
 * follows the task which called `getCurrentTexture()`, and expires the texture
 * there. That update can only run between tasks, so once the acquiring task has
 * ended the canvas may already show whatever had been written by then - a
 * partially drawn frame, or an empty one - and later writes never reach the
 * screen. On the Wasm build a frame's task ends early whenever the frame
 * suspends through Asyncify (a GPU wait, a map, a sleep), so this is the
 * observable for "a frame gave the thread back while it held the canvas".
 * Microtasks the acquiring task runs before it ends still belong to it.
 */
export interface LateCanvasSubmit {
  /** Ordinal of the canvas frame, counting acquisitions in this worker. */
  frame: number;
  /** Worker clock when the frame's texture was acquired. */
  acquiredAtMs: number;
  /** Worker clock when the first task after the acquiring one ran. */
  taskEndedAtMs: number;
  /** Worker clock of the late submission. */
  submittedAtMs: number;
  /** Command buffers that reached the frame inside the acquiring task. */
  submitsBeforeTaskEnd: number;
  /** Timer delays the frame scheduled while it held the texture, in call order. */
  timersWhileHeld: number[];
}

export interface SurfaceFrameProbeReport {
  /** Workers that acquired at least one canvas frame while the probe watched. */
  canvasWorkers: number;
  /** Canvas frames acquired while the probe was installed, over every worker. */
  frames: number;
  /**
   * Submissions that wrote into a canvas frame inside the task that acquired
   * it. Zero with frames above zero would mean the probe never saw the frames'
   * own work, and a clean `lateSubmits` would then prove nothing. Reads of a
   * frame, such as a copy out of it, are not writes and are counted nowhere.
   */
  inTaskSubmits: number;
  lateSubmits: LateCanvasSubmit[];
}

interface WorkerProbeState {
  installed: boolean;
  frames: number;
  inTaskSubmits: number;
  lateSubmits: LateCanvasSubmit[];
}

type ProbeGlobal = typeof globalThis & {
  __donnerSurfaceFrameProbe?: WorkerProbeState;
  /** Resolves in a task that runs after every task boundary the probe has marked so far. */
  __donnerSurfaceFrameProbeNextTask?: () => Promise<void>;
};

// Runs inside each worker. Everything it needs is defined in the body because
// Playwright serializes the function source into the worker's scope.
function installInWorker(): boolean {
  const scope = globalThis as ProbeGlobal;
  if (scope.__donnerSurfaceFrameProbe) {
    return scope.__donnerSurfaceFrameProbe.installed;
  }
  const state: WorkerProbeState = {
    installed: false,
    frames: 0,
    inTaskSubmits: 0,
    lateSubmits: [],
  };
  scope.__donnerSurfaceFrameProbe = state;
  if (
    typeof GPUCanvasContext === "undefined" || typeof GPUCommandEncoder === "undefined"
    || typeof GPUQueue === "undefined" || typeof GPUTexture === "undefined"
  ) {
    return false;
  }

  interface FrameRecord {
    frame: number;
    acquiredAtMs: number;
    taskEndedAtMs: number;
    taskEnded: boolean;
    submitsBeforeTaskEnd: number;
    timersWhileHeld: number[];
  }
  const frames = new WeakMap<GPUTexture, FrameRecord>();
  const textureOfView = new WeakMap<GPUTextureView, GPUTexture>();
  const encoderFrames = new WeakMap<GPUCommandEncoder, Set<GPUTexture>>();
  const bufferFrames = new WeakMap<GPUCommandBuffer, Set<GPUTexture>>();
  let heldFrame: FrameRecord | null = null;

  // Task boundaries are marked with messages to the probe's own port: a message
  // is a task of its own, so it runs only once the task that posted it and that
  // task's microtasks are done, and one port delivers its messages in order. A
  // resumption the engine delivered ahead of that message would go unreported,
  // never misreported.
  const boundaryCallbacks: Array<() => void> = [];
  const boundaries = new MessageChannel();
  boundaries.port1.onmessage = () => boundaryCallbacks.shift()?.();
  const afterThisTask = (callback: () => void) => {
    boundaryCallbacks.push(callback);
    boundaries.port2.postMessage(null);
  };
  scope.__donnerSurfaceFrameProbeNextTask = () =>
    new Promise<void>((resolve) => afterThisTask(resolve));

  const getCurrentTexture = GPUCanvasContext.prototype.getCurrentTexture;
  GPUCanvasContext.prototype.getCurrentTexture = function(this: GPUCanvasContext) {
    const texture = getCurrentTexture.call(this);
    // A texture handed out again by a later task is a new acquisition; the
    // standard hands out a new object then, and an engine that reused one must
    // not have the earlier frame's ended task charged to it.
    if (!frames.has(texture) || frames.get(texture)!.taskEnded) {
      const record: FrameRecord = {
        frame: ++state.frames,
        acquiredAtMs: performance.now(),
        taskEndedAtMs: -1,
        taskEnded: false,
        submitsBeforeTaskEnd: 0,
        timersWhileHeld: [],
      };
      frames.set(texture, record);
      heldFrame = record;
      afterThisTask(() => {
        record.taskEnded = true;
        record.taskEndedAtMs = performance.now();
        if (heldFrame === record) {
          heldFrame = null;
        }
      });
    }
    return texture;
  };

  const setTimeoutImpl = scope.setTimeout;
  scope.setTimeout = function(handler: TimerHandler, timeout?: number, ...rest: unknown[]) {
    if (heldFrame !== null) {
      heldFrame.timersWhileHeld.push(Number(timeout ?? 0));
    }
    return setTimeoutImpl(handler, timeout, ...rest);
  } as typeof setTimeout;

  const createView = GPUTexture.prototype.createView;
  GPUTexture.prototype.createView = function(
    this: GPUTexture,
    descriptor?: GPUTextureViewDescriptor,
  ) {
    const view = createView.call(this, descriptor);
    if (frames.has(this)) {
      textureOfView.set(view, this);
    }
    return view;
  };

  // Records that `encoder` writes into a canvas frame. Only writes are recorded:
  // a late read, such as a copy out of an expired frame, changes nothing on screen.
  const noteFrameWrite = (encoder: GPUCommandEncoder, texture: GPUTexture | undefined) => {
    if (texture === undefined || !frames.has(texture)) {
      return;
    }
    let touched = encoderFrames.get(encoder);
    if (touched === undefined) {
      touched = new Set();
      encoderFrames.set(encoder, touched);
    }
    touched.add(texture);
  };

  const beginRenderPass = GPUCommandEncoder.prototype.beginRenderPass;
  GPUCommandEncoder.prototype.beginRenderPass = function(
    this: GPUCommandEncoder,
    descriptor: GPURenderPassDescriptor,
  ) {
    for (const attachment of descriptor.colorAttachments ?? []) {
      if (attachment) {
        noteFrameWrite(this, textureOfView.get(attachment.view as GPUTextureView));
        if (attachment.resolveTarget) {
          noteFrameWrite(this, textureOfView.get(attachment.resolveTarget as GPUTextureView));
        }
      }
    }
    return beginRenderPass.call(this, descriptor);
  };

  const copyBufferToTexture = GPUCommandEncoder.prototype.copyBufferToTexture;
  GPUCommandEncoder.prototype.copyBufferToTexture = function(
    this: GPUCommandEncoder,
    source: GPUTexelCopyBufferInfo,
    destination: GPUTexelCopyTextureInfo,
    ...rest: unknown[]
  ) {
    noteFrameWrite(this, destination.texture);
    return (copyBufferToTexture as (...args: unknown[]) => void).call(
      this,
      source,
      destination,
      ...rest,
    );
  };

  const copyTextureToTexture = GPUCommandEncoder.prototype.copyTextureToTexture;
  GPUCommandEncoder.prototype.copyTextureToTexture = function(
    this: GPUCommandEncoder,
    source: GPUTexelCopyTextureInfo,
    destination: GPUTexelCopyTextureInfo,
    ...rest: unknown[]
  ) {
    noteFrameWrite(this, destination.texture);
    return (copyTextureToTexture as (...args: unknown[]) => void).call(
      this,
      source,
      destination,
      ...rest,
    );
  };

  const finish = GPUCommandEncoder.prototype.finish;
  GPUCommandEncoder.prototype.finish = function(
    this: GPUCommandEncoder,
    descriptor?: GPUCommandBufferDescriptor,
  ) {
    const buffer = finish.call(this, descriptor);
    const touched = encoderFrames.get(this);
    if (touched !== undefined) {
      bufferFrames.set(buffer, touched);
    }
    return buffer;
  };

  const submit = GPUQueue.prototype.submit;
  GPUQueue.prototype.submit = function(this: GPUQueue, buffers: Iterable<GPUCommandBuffer>) {
    const list = [...buffers];
    const submittedAtMs = performance.now();
    for (const buffer of list) {
      for (const texture of bufferFrames.get(buffer) ?? []) {
        const record = frames.get(texture)!;
        if (!record.taskEnded) {
          ++record.submitsBeforeTaskEnd;
          ++state.inTaskSubmits;
          continue;
        }
        state.lateSubmits.push({
          frame: record.frame,
          acquiredAtMs: record.acquiredAtMs,
          taskEndedAtMs: record.taskEndedAtMs,
          submittedAtMs,
          submitsBeforeTaskEnd: record.submitsBeforeTaskEnd,
          timersWhileHeld: [...record.timersWhileHeld],
        });
      }
    }
    return submit.call(this, list);
  };

  state.installed = true;
  return true;
}

// Runs inside a worker that already has the probe installed. Writes a scratch
// canvas frame in the task that acquired it, again from a microtask of that
// same task, and once more after that task has ended; reads it once late as
// well. Only the third write is the ordering the probe exists to report.
async function selfCheckInWorker(): Promise<{ late: number; inTask: number } | string> {
  const scope = globalThis as ProbeGlobal;
  const state = scope.__donnerSurfaceFrameProbe;
  const nextTask = scope.__donnerSurfaceFrameProbeNextTask;
  if (!state?.installed || nextTask === undefined) {
    return "probe not installed";
  }
  const adapter = await navigator.gpu?.requestAdapter();
  if (!adapter) {
    return "no adapter";
  }
  const device = await adapter.requestDevice();
  const canvas = new OffscreenCanvas(16, 16);
  const context = canvas.getContext("webgpu") as GPUCanvasContext | null;
  if (context === null) {
    return "no webgpu canvas context";
  }
  context.configure({
    device,
    format: navigator.gpu.getPreferredCanvasFormat(),
    usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC,
  });
  const lateBefore = state.lateSubmits.length;
  const inTaskBefore = state.inTaskSubmits;
  const clear = (texture: GPUTexture) => {
    const encoder = device.createCommandEncoder();
    encoder.beginRenderPass({
      colorAttachments: [{ view: texture.createView(), loadOp: "clear", storeOp: "store" }],
    }).end();
    device.queue.submit([encoder.finish()]);
  };
  const readBack = (texture: GPUTexture) => {
    const buffer = device.createBuffer({ size: 256 * 16, usage: GPUBufferUsage.COPY_DST });
    const encoder = device.createCommandEncoder();
    encoder.copyTextureToBuffer({ texture }, { buffer, bytesPerRow: 256 }, [16, 16]);
    device.queue.submit([encoder.finish()]);
  };
  const frame = context.getCurrentTexture();
  clear(frame);
  await Promise.resolve();
  clear(frame);
  await nextTask();
  clear(frame);
  readBack(frame);
  device.destroy();
  return {
    late: state.lateSubmits.length - lateBefore,
    inTask: state.inTaskSubmits - inTaskBefore,
  };
}

function readInWorker(): WorkerProbeState | null {
  const state = (globalThis as ProbeGlobal).__donnerSurfaceFrameProbe;
  return state === undefined ? null : { ...state, lateSubmits: [...state.lateSubmits] };
}

// A pthread parked in a blocking wait never returns to its event loop, so an
// evaluation there would not settle. The canvas owner runs a frame loop and
// answers between frames, and the slowest frames measured on a shared runner
// are well under this; a worker that does not answer within it owns no frame
// loop to observe. Every worker is asked at once, so the bound is paid once.
const kWorkerAnswerMs = 1_000;

// The workers that took the probe, so later reads ask only them rather than
// spending the answer bound again on a worker that never answers.
const probedWorkers = new WeakMap<Page, Worker[]>();

async function evaluateInWorkers<T>(
  workers: Worker[],
  fn: () => T,
  answerMs = kWorkerAnswerMs,
): Promise<Array<Awaited<T> | null>> {
  return Promise.all(workers.map((worker) => {
    let timer: ReturnType<typeof setTimeout> | undefined;
    const timedOut = new Promise<null>((resolve) => {
      timer = setTimeout(() => resolve(null), answerMs);
    });
    return Promise.race([worker.evaluate(fn).catch(() => null), timedOut]).finally(() =>
      clearTimeout(timer)
    );
  }));
}

/**
 * Watch every canvas frame the page's workers acquire from now on.
 *
 * Installed per worker, so it must run after the editor has started its
 * workers. Returns how many workers took the probe (every worker that answered
 * and exposes WebGPU); zero means it will observe nothing.
 */
export async function installSurfaceFrameProbe(page: Page): Promise<number> {
  const workers = page.workers() as Worker[];
  const installed = await evaluateInWorkers(workers, installInWorker);
  const probed = workers.filter((_, index) => installed[index] === true);
  probedWorkers.set(page, probed);
  return probed.length;
}

/**
 * Prove the probe reports the ordering it watches for, in the engine under
 * test: two writes inside the acquiring task (one from its microtask) count as
 * in-task, one write after the task ended counts as late, and a late read
 * counts as neither, so a working probe answers `{ late: 1, inTask: 2 }`. The
 * scratch canvas and device are the probe's own, so the editor's frames are
 * untouched.
 */
export async function selfCheckSurfaceFrameProbe(
  page: Page,
): Promise<Array<{ late: number; inTask: number } | string | null>> {
  // Requesting a scratch device takes longer than reading the probe's state.
  return evaluateInWorkers(probedWorkers.get(page) ?? [], selfCheckInWorker, 4 * kWorkerAnswerMs);
}

export async function readSurfaceFrameProbe(page: Page): Promise<SurfaceFrameProbeReport> {
  const states = (await evaluateInWorkers(probedWorkers.get(page) ?? [], readInWorker)).filter(
    (state): state is WorkerProbeState => state !== null && state.installed,
  );
  return {
    canvasWorkers: states.filter((state) => state.frames > 0).length,
    frames: states.reduce((sum, state) => sum + state.frames, 0),
    inTaskSubmits: states.reduce((sum, state) => sum + state.inTaskSubmits, 0),
    lateSubmits: states.flatMap((state) => state.lateSubmits),
  };
}
