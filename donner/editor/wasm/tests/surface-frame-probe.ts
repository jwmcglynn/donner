import type { Page, Worker } from "@playwright/test";

/**
 * A command buffer that wrote into a canvas frame after the task that acquired
 * the frame had already ended.
 *
 * A WebGPU canvas presents its current texture when the task that called
 * `getCurrentTexture()` returns to the event loop, and the texture is expired at
 * that point. Work submitted into it afterwards never reaches the screen, and
 * whatever had been submitted before the task ended is what the page shows - a
 * partially drawn frame, or an empty one. On the Wasm build a task ends early
 * whenever the frame suspends through Asyncify (a GPU wait, a map, a sleep), so
 * this is the observable for "a frame yielded while it held the canvas".
 */
export interface LateCanvasSubmit {
  /** Ordinal of the canvas frame, counting acquisitions in this worker. */
  frame: number;
  /** Worker clock when the frame's texture was acquired. */
  acquiredAtMs: number;
  /** Worker clock when the acquiring task gave the thread back to the event loop. */
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
   * own work, and a clean `lateSubmits` would then prove nothing.
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

type ProbeGlobal = typeof globalThis & { __donnerSurfaceFrameProbe?: WorkerProbeState };

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
      // A microtask runs once the JavaScript stack is empty: at the end of the
      // task, or the moment an Asyncify suspend unwinds the frame to the event
      // loop. Either is when the canvas presents this texture.
      queueMicrotask(() => {
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

  const noteFrame = (encoder: GPUCommandEncoder, texture: GPUTexture | undefined) => {
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
        noteFrame(this, textureOfView.get(attachment.view as GPUTextureView));
        if (attachment.resolveTarget) {
          noteFrame(this, textureOfView.get(attachment.resolveTarget as GPUTextureView));
        }
      }
    }
    return beginRenderPass.call(this, descriptor);
  };

  const copyTextureToBuffer = GPUCommandEncoder.prototype.copyTextureToBuffer;
  GPUCommandEncoder.prototype.copyTextureToBuffer = function(
    this: GPUCommandEncoder,
    source: GPUTexelCopyTextureInfo,
    ...rest: unknown[]
  ) {
    noteFrame(this, source.texture);
    return (copyTextureToBuffer as (...args: unknown[]) => void).call(this, source, ...rest);
  };

  const copyTextureToTexture = GPUCommandEncoder.prototype.copyTextureToTexture;
  GPUCommandEncoder.prototype.copyTextureToTexture = function(
    this: GPUCommandEncoder,
    source: GPUTexelCopyTextureInfo,
    destination: GPUTexelCopyTextureInfo,
    ...rest: unknown[]
  ) {
    noteFrame(this, source.texture);
    noteFrame(this, destination.texture);
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

// Runs inside a worker that already has the probe installed. Clears a scratch
// canvas frame once inside the task that acquired it and once more after that
// task has ended - the second is exactly the ordering the probe exists to report.
async function selfCheckInWorker(): Promise<{ late: number; inTask: number } | string> {
  const state = (globalThis as ProbeGlobal).__donnerSurfaceFrameProbe;
  if (!state?.installed) {
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
  context.configure({ device, format: navigator.gpu.getPreferredCanvasFormat() });
  const lateBefore = state.lateSubmits.length;
  const inTaskBefore = state.inTaskSubmits;
  const clear = (texture: GPUTexture) => {
    const encoder = device.createCommandEncoder();
    encoder.beginRenderPass({
      colorAttachments: [{ view: texture.createView(), loadOp: "clear", storeOp: "store" }],
    }).end();
    device.queue.submit([encoder.finish()]);
  };
  const frame = context.getCurrentTexture();
  clear(frame);
  await new Promise((resolve) => setTimeout(resolve, 0));
  clear(frame);
  device.destroy();
  return {
    late: state.lateSubmits.length - lateBefore,
    inTask: state.inTaskSubmits - inTaskBefore,
  };
}

function readInWorker(): WorkerProbeState | null {
  return (globalThis as ProbeGlobal).__donnerSurfaceFrameProbe ?? null;
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
 * test: a frame cleared inside its task counts once as in-task, and a frame
 * cleared after its task ended counts once as late. The scratch canvas and
 * device are the probe's own, so the editor's frames are untouched.
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
