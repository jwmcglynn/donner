import { errors, expect, type Page, type Worker } from "@playwright/test";
import { captureQuiescentCanvas } from "./canvas-quiescent-capture.mjs";
import { createSurfaceProbeRegistry } from "./surface-probe-requests.mjs";
import { kStallGuardMs } from "./test-timing";

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
  canvasSubmits: number;
  /** Highest canvas-writing submission whose GPU work the browser queue reports finished. */
  canvasRetired: number;
  lateSubmits: LateCanvasSubmit[];
}

type ProbeGlobal = typeof globalThis & {
  __donnerSurfaceFrameProbe?: WorkerProbeState;
  /** Resolves in a task that runs after every task boundary the probe has marked so far. */
  __donnerSurfaceFrameProbeNextTask?: () => Promise<void>;
  __donnerSurfaceFrameProbeQueue?: GPUQueue;
  __donnerSurfaceFrameProbeCompletionHold?: {
    promise: Promise<void>;
    release: () => void;
    calls: number;
  };
  __donnerSurfaceFrameProbeLateCompletion?: {
    promise: Promise<void>;
    release: () => void;
    claimed: boolean;
  };
  __donnerSurfaceFrameProbeGpuDelay?: CanvasGpuDelayState;
  /** Waits until the GPU has run every canvas write, with none submitted meanwhile. */
  __donnerSurfaceFrameProbeSettle?: () => Promise<CanvasSettleResult>;
};

/** One wait for the GPU to finish the canvas writes submitted so far. */
export interface CanvasSettleResult {
  /** Canvas-writing submissions counted when the GPU had run all of them. */
  canvasSubmits: number;
  /** Canvas frames acquired by then. */
  frames: number;
  /** Queue completions awaited: more than one when a frame was submitted during a wait. */
  rounds: number;
}

/**
 * Test-only fault injection: after the first command buffer that writes every Nth canvas frame,
 * queue GPU work of a fixed size before the frame's remaining submissions. The frame's CPU task
 * and submission order are unchanged; only GPU execution of the rest of that frame is postponed.
 */
interface CanvasGpuDelayState {
  everyFrames: number;
  fragmentIterations: number;
  injected: number;
  /** Most jobs to inject, or -1 for no limit. */
  maxInjections: number;
  /** Injected jobs the GPU has not finished yet. */
  pending: number;
  /** Whether a settle wait began while an injected job was still running on the GPU. */
  settleSawPending: boolean;
  /** Submit-to-completion wall time of each injected job, in worker milliseconds. */
  jobMs: number[];
}

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
    canvasSubmits: 0,
    canvasRetired: 0,
    lateSubmits: [],
  };
  scope.__donnerSurfaceFrameProbe = state;
  if (
    typeof GPUCanvasContext === "undefined" || typeof GPUCommandEncoder === "undefined"
    || typeof GPUDevice === "undefined"
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

  // The queue a submission goes to names its device only through the encoders made from it.
  const queueDevices = new WeakMap<GPUQueue, GPUDevice>();
  const createCommandEncoder = GPUDevice.prototype.createCommandEncoder;
  GPUDevice.prototype.createCommandEncoder = function(
    this: GPUDevice,
    descriptor?: GPUCommandEncoderDescriptor,
  ) {
    queueDevices.set(this.queue, this);
    return createCommandEncoder.call(this, descriptor);
  };
  const delayJobs = new WeakMap<GPUDevice, { pipeline: GPURenderPipeline; view: GPUTextureView }>();
  const submitDelayJob = (queue: GPUQueue, delay: CanvasGpuDelayState) => {
    const device = queueDevices.get(queue);
    if (device === undefined) return;
    let job = delayJobs.get(device);
    if (job === undefined) {
      const module = device.createShaderModule({
        code: `
          @vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
            var p = array(vec2f(-1.0, -1.0), vec2f(3.0, -1.0), vec2f(-1.0, 3.0));
            return vec4f(p[i], 0.0, 1.0);
          }
          @fragment fn fs(@builtin(position) position: vec4f) -> @location(0) vec4f {
            var v = position.x * 0.001 + position.y;
            for (var k = 0u; k < ${Math.max(1, Math.floor(delay.fragmentIterations))}u; k++) {
              v = fract(sin(v) * 43758.5453 + position.y * 0.0001);
            }
            return vec4f(v, 0.0, 0.0, 1.0);
          }`,
      });
      const pipeline = device.createRenderPipeline({
        layout: "auto",
        vertex: { module, entryPoint: "vs" },
        fragment: { module, entryPoint: "fs", targets: [{ format: "rgba8unorm" }] },
      });
      const texture = device.createTexture({
        size: [1024, 1024],
        format: "rgba8unorm",
        usage: GPUTextureUsage.RENDER_ATTACHMENT,
      });
      job = { pipeline, view: texture.createView() };
      delayJobs.set(device, job);
    }
    const encoder = createCommandEncoder.call(device, undefined);
    const pass = beginRenderPass.call(encoder, {
      colorAttachments: [{ view: job.view, loadOp: "clear", storeOp: "store" }],
    });
    pass.setPipeline(job.pipeline);
    pass.draw(3);
    pass.end();
    const startedAtMs = performance.now();
    submit.call(queue, [finish.call(encoder, undefined)]);
    ++delay.injected;
    ++delay.pending;
    onSubmittedWorkDone.call(queue).then(() => {
      if (delay.jobMs.length < 256) delay.jobMs.push(performance.now() - startedAtMs);
    }).finally(() => {
      --delay.pending;
    }).catch(() => {});
  };

  scope.__donnerSurfaceFrameProbeSettle = async () => {
    // Recorded in the worker as the wait begins, with no round trip that a fast GPU could outrun.
    const armedDelay = scope.__donnerSurfaceFrameProbeGpuDelay;
    if (armedDelay !== undefined && armedDelay.pending > 0) armedDelay.settleSawPending = true;
    // A round that saw a new canvas frame or write cannot vouch for that write's GPU work, so wait again.
    for (let rounds = 1; rounds <= 16; ++rounds) {
      const queue = scope.__donnerSurfaceFrameProbeQueue;
      if (queue === undefined) throw new Error("the probe has not observed a canvas write");
      const submits = state.canvasSubmits;
      const frames = state.frames;
      await onSubmittedWorkDone.call(queue);
      if (state.canvasSubmits === submits && state.frames === frames) {
        return { canvasSubmits: submits, frames, rounds };
      }
    }
    throw new Error("canvas writes kept arriving while waiting for the GPU to finish them");
  };

  let pendingCanvasSubmit = 0;
  const onSubmittedWorkDone = GPUQueue.prototype.onSubmittedWorkDone;
  GPUQueue.prototype.onSubmittedWorkDone = function(this: GPUQueue) {
    const gpuPromise = onSubmittedWorkDone.call(this);
    const hold = scope.__donnerSurfaceFrameProbeCompletionHold;
    const late = scope.__donnerSurfaceFrameProbeLateCompletion;
    const isCanvasCompletion = this === scope.__donnerSurfaceFrameProbeQueue
      && pendingCanvasSubmit > 0;
    // A controlled test can delay the app's completion signal after real GPU
    // completion. Ordinary observation returns the original promise unchanged.
    let promise = gpuPromise;
    if (isCanvasCompletion && late !== undefined && !late.claimed) {
      late.claimed = true;
      promise = promise.then(() => late.promise);
    }
    if (isCanvasCompletion && hold !== undefined) {
      promise = promise.then(() => hold.promise);
    }
    if (isCanvasCompletion) {
      if (hold !== undefined) ++hold.calls;
      pendingCanvasSubmit = 0;
    }
    return promise;
  };

  const submit = GPUQueue.prototype.submit;
  GPUQueue.prototype.submit = function(this: GPUQueue, buffers: Iterable<GPUCommandBuffer>) {
    const list = [...buffers];
    const submittedAtMs = performance.now();
    let writesCanvas = false;
    let delayFrame = false;
    for (const buffer of list) {
      for (const texture of bufferFrames.get(buffer) ?? []) {
        writesCanvas = true;
        const record = frames.get(texture)!;
        if (!record.taskEnded) {
          ++record.submitsBeforeTaskEnd;
          ++state.inTaskSubmits;
          const delay = scope.__donnerSurfaceFrameProbeGpuDelay;
          delayFrame ||= delay !== undefined && record.submitsBeforeTaskEnd === 1
            && record.frame % delay.everyFrames === 0
            && (delay.maxInjections < 0 || delay.injected < delay.maxInjections);
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
    const result = submit.call(this, list);
    const delay = scope.__donnerSurfaceFrameProbeGpuDelay;
    if (delayFrame && delay !== undefined) {
      submitDelayJob(this, delay);
    }
    if (writesCanvas) {
      scope.__donnerSurfaceFrameProbeQueue = this;
      const serial = ++state.canvasSubmits;
      pendingCanvasSubmit = serial;
      // The probe's own completion request, apart from the app's: it records when the GPU has run
      // this canvas write without changing any promise the app awaits.
      onSubmittedWorkDone.call(this).then(() => {
        state.canvasRetired = Math.max(state.canvasRetired, serial);
      }).catch(() => {
        // A lost or destroyed device never retires the write; the counter then stays behind.
      });
    }
    return result;
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

// Bound each observation without treating a slow reply as proof of nonownership.
// The registry retains installation and discovery replies until they can be observed.
const kWorkerAnswerMs = 1_000;

const probeRegistry = createSurfaceProbeRegistry(kWorkerAnswerMs);

async function evaluateInWorkers<T>(
  workers: Worker[],
  fn: () => T,
  answerMs = kWorkerAnswerMs,
): Promise<Array<Awaited<T> | null>> {
  return probeRegistry.evaluate(workers, fn, answerMs);
}

/**
 * Watch every canvas frame the page's workers acquire from now on.
 *
 * Installed per worker, so it must run after the editor has started its
 * workers. Returns the number of acknowledged installations. A bounded wait
 * can finish before an installation reply; that late acknowledgement remains
 * available to the next readiness check.
 */
export async function installSurfaceFrameProbe(page: Page): Promise<number> {
  return probeRegistry.install(page, installInWorker);
}

/** Whether \ref installSurfaceFrameProbe installed the probe in any of the page's workers. */
export function surfaceFrameProbeInstalled(page: Page): boolean {
  return probeRegistry.workers(page).length > 0;
}

/** Return the one worker that acquired the editor's canvas while the probe watched. */
export async function findCanvasOwnerWorker(page: Page): Promise<Worker | null> {
  return probeRegistry.findOwner(
    page,
    readInWorker,
    (state: WorkerProbeState | null) => (state?.frames ?? 0) > 0,
  );
}

/** Find the probe-installed application canvas owner before its next acquisition. */
function applicationCanvasOwnerInWorker(): boolean {
  const scope = globalThis as ProbeGlobal & { __donnerApplicationGpuSurfaceWorker?: boolean };
  return Boolean(
    scope.__donnerApplicationGpuSurfaceWorker && scope.__donnerSurfaceFrameProbe?.installed,
  );
}

export async function findProbedCanvasApplicationWorker(page: Page): Promise<Worker | null> {
  return probeRegistry.findOwner(page, applicationCanvasOwnerInWorker);
}

/** Wait for a unique acknowledged application owner within the caller's existing expect budget. */
export async function expectSurfaceFrameProbeReady(page: Page): Promise<void> {
  await expect.poll(async () => {
    await installSurfaceFrameProbe(page);
    return findProbedCanvasApplicationWorker(page);
  }, { message: "the application canvas owner must acknowledge the completion probe" }).not
    .toBeNull();
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
  return evaluateInWorkers(probeRegistry.workers(page), selfCheckInWorker, 4 * kWorkerAnswerMs);
}

// Control only the completion signal already returned to the app, for browser regressions that
// fill the app's presentation queue with held frames. No queue work is added or delayed on
// ordinary paths.
export async function holdCanvasCompletionForTest(
  _page: Page,
  workersForTest: Worker[],
): Promise<{
  observedCalls: () => Promise<number>;
  release: (owner: Worker) => Promise<void>;
}> {
  const workers = workersForTest;
  const armed = await evaluateInWorkers(workers, () => {
    const scope = globalThis as ProbeGlobal;
    let release!: () => void;
    const promise = new Promise<void>((resolve) => {
      release = resolve;
    });
    scope.__donnerSurfaceFrameProbeCompletionHold = { promise, release, calls: 0 };
    return true;
  });
  if (workers.length === 0 || armed.some((result) => result !== true)) {
    throw new Error("could not arm the canvas completion hold");
  }
  return {
    observedCalls: async () => {
      const counts = await evaluateInWorkers(
        workers,
        () => (globalThis as ProbeGlobal).__donnerSurfaceFrameProbeCompletionHold?.calls ?? 0,
      );
      return counts.reduce<number>((sum, count) => sum + (count ?? 0), 0);
    },
    release: async (owner) => {
      if (!workers.includes(owner)) {
        throw new Error("canvas completion owner was not armed");
      }
      // Only the worker that actually submitted the editor canvas can hold
      // the promise the screenshot awaits. Other probed workers may park and
      // stop answering evaluations; their response is not a release proof.
      const [released] = await evaluateInWorkers([owner], () => {
        const scope = globalThis as ProbeGlobal;
        scope.__donnerSurfaceFrameProbeCompletionHold?.release();
        delete scope.__donnerSurfaceFrameProbeCompletionHold;
        return true;
      });
      if (released !== true) {
        throw new Error("could not release the required canvas completion owner");
      }
    },
  };
}

/**
 * Delay the app's next canvas completion in \p owner until released, independently of any
 * completion hold. A frame submitted before a hold is armed is not held by it, so this models such
 * a frame completing only after the hold is in place.
 */
export async function delayNextCanvasCompletionForTest(
  owner: Worker,
): Promise<{ release: () => Promise<void> }> {
  const [armed] = await evaluateInWorkers([owner], () => {
    const scope = globalThis as ProbeGlobal;
    let release!: () => void;
    const promise = new Promise<void>((resolve) => {
      release = resolve;
    });
    scope.__donnerSurfaceFrameProbeLateCompletion = { promise, release, claimed: false };
    return true;
  });
  if (armed !== true) {
    throw new Error("could not arm the late canvas completion");
  }
  let released = false;
  return {
    release: async () => {
      if (released) return;
      released = true;
      const [done] = await evaluateInWorkers([owner], () => {
        const scope = globalThis as ProbeGlobal;
        scope.__donnerSurfaceFrameProbeLateCompletion?.release();
        delete scope.__donnerSurfaceFrameProbeLateCompletion;
        return true;
      });
      if (done !== true) {
        throw new Error("could not release the late canvas completion");
      }
    },
  };
}

export async function readSurfaceFrameProbe(page: Page): Promise<SurfaceFrameProbeReport> {
  return summarizeProbeStates(await evaluateInWorkers(probeRegistry.workers(page), readInWorker));
}

/**
 * Read only the canvas owner's probe, so a sample taken around one capture does not wait out the
 * answer bound of workers parked in a blocking wait. Null when the owner did not answer.
 */
export async function readCanvasOwnerSurfaceFrameProbe(
  owner: Worker,
): Promise<SurfaceFrameProbeReport | null> {
  const [state] = await evaluateInWorkers([owner], readInWorker);
  return state === null ? null : summarizeProbeStates([state]);
}

function summarizeProbeStates(answers: Array<WorkerProbeState | null>): SurfaceFrameProbeReport {
  const states = answers.filter(
    (state): state is WorkerProbeState => state !== null && state.installed,
  );
  return {
    canvasWorkers: states.filter((state) => state.frames > 0).length,
    frames: states.reduce((sum, state) => sum + state.frames, 0),
    inTaskSubmits: states.reduce((sum, state) => sum + state.inTaskSubmits, 0),
    lateSubmits: states.flatMap((state) => state.lateSubmits),
  };
}

/**
 * Fault injection for presentation-capture races: postpone GPU execution of the rest of every
 * \p everyFrames-th canvas frame in \p owner behind a fixed-size fragment job submitted right after
 * that frame's first canvas write. CPU frame timing and task boundaries are unchanged. Disarming
 * stops further injection and returns what was injected.
 */
export async function armCanvasGpuDelayForTest(
  owner: Worker,
  everyFrames: number,
  fragmentIterations: number,
  maxInjections = -1,
): Promise<{ disarm: () => Promise<CanvasGpuDelayResult | null> }> {
  if (!Number.isInteger(everyFrames) || everyFrames < 1) {
    throw new Error("the canvas GPU delay needs a positive frame interval");
  }
  const armed = await owner.evaluate(([frames, iterations, most]) => {
    const scope = globalThis as ProbeGlobal;
    if (!scope.__donnerSurfaceFrameProbe?.installed) return false;
    scope.__donnerSurfaceFrameProbeGpuDelay = {
      everyFrames: frames,
      fragmentIterations: iterations,
      injected: 0,
      maxInjections: most,
      pending: 0,
      settleSawPending: false,
      jobMs: [],
    };
    return true;
  }, [everyFrames, fragmentIterations, maxInjections]);
  if (armed !== true) {
    throw new Error("could not arm the canvas GPU delay");
  }
  return {
    disarm: async () => {
      return owner.evaluate(() => {
        const scope = globalThis as ProbeGlobal;
        const delay = scope.__donnerSurfaceFrameProbeGpuDelay;
        delete scope.__donnerSurfaceFrameProbeGpuDelay;
        return delay === undefined ? null : {
          injected: delay.injected,
          settleSawPending: delay.settleSawPending,
          jobMs: [...delay.jobMs],
        };
      });
    },
  };
}

/** What \ref armCanvasGpuDelayForTest injected. */
export interface CanvasGpuDelayResult {
  injected: number;
  /** Whether a settle wait began while an injected job was still running on the GPU. */
  settleSawPending: boolean;
  jobMs: number[];
}

/** A `<everyFrames>:<iterations>` canvas GPU delay read from the environment. */
export interface CanvasGpuDelaySpec {
  spec: string;
  everyFrames: number;
  iterations: number;
}

/**
 * Read the fault injection for \ref armCanvasGpuDelayForTest from environment variable \p name, as
 * `<everyFrames>:<iterations>`. Unset or empty leaves the injection off.
 */
export function canvasGpuDelayFromEnv(name: string): CanvasGpuDelaySpec | null {
  const spec = process.env[name];
  if (spec === undefined || spec === "") return null;
  const match = /^(\d+):(\d+)$/.exec(spec);
  const everyFrames = match ? Number(match[1]) : 0;
  const iterations = match ? Number(match[2]) : 0;
  if (!match || everyFrames < 1 || everyFrames > 1000 || iterations < 1 || iterations > 1_000_000) {
    throw new Error(`${name} must be <everyFrames 1-1000>:<iterations 1-1000000>`);
  }
  return { spec, everyFrames, iterations };
}

/**
 * Arm \ref armCanvasGpuDelayForTest in \p owner when environment variable \p variable sets it.
 * `finish()` disarms it and returns what was injected.
 */
export async function armCanvasGpuDelayFromEnv(
  owner: Worker,
  variable: string,
): Promise<{ finish: () => Promise<CanvasGpuDelayReport> } | null> {
  const config = canvasGpuDelayFromEnv(variable);
  if (config === null) return null;
  const delay = await armCanvasGpuDelayForTest(owner, config.everyFrames, config.iterations);
  return {
    finish: async () => ({ spec: config.spec, ...(await delay.disarm()) }),
  };
}

/** What \ref armCanvasGpuDelayFromEnv injected: its spec, and each injected job's GPU time. */
export interface CanvasGpuDelayReport {
  spec: string;
  injected?: number;
  jobMs?: number[];
}

/**
 * Wait in \p owner until the GPU has finished every canvas write submitted so far, with no new
 * canvas write submitted while waiting. Uses the browser's own queue completion, not the app's
 * (possibly held) completion promise.
 */
export function settleCanvasWrites(owner: Worker): Promise<CanvasSettleResult> {
  return owner.evaluate(async () => {
    const settle = (globalThis as ProbeGlobal).__donnerSurfaceFrameProbeSettle;
    if (settle === undefined) throw new Error("the surface frame probe is not installed");
    return settle();
  });
}

/** Canvas activity of one worker since the probe was installed. */
export interface CanvasActivity {
  /** Canvas-writing submissions. */
  canvasSubmits: number;
  /** Canvas frames acquired. */
  frames: number;
  /** Highest canvas-writing submission the GPU had finished when this was read. */
  canvasRetired: number;
}

/** Canvas-writing submissions and canvas frames \p owner has made since the probe was installed. */
export function readCanvasActivity(owner: Worker): Promise<CanvasActivity> {
  return owner.evaluate(() => {
    const state = (globalThis as ProbeGlobal).__donnerSurfaceFrameProbe;
    if (!state?.installed) throw new Error("the surface frame probe is not installed");
    return {
      canvasSubmits: state.canvasSubmits,
      frames: state.frames,
      canvasRetired: state.canvasRetired,
    };
  });
}

/**
 * Hold the app's canvas completions in \p owner until released, so its presentation queue stops
 * admitting new frames once three are pending. The GPU work itself is not delayed; only the
 * completion promises the app awaits are.
 */
export async function holdCanvasCompletions(owner: Worker): Promise<{ release: () => Promise<void> }> {
  const armed = await owner.evaluate(() => {
    const scope = globalThis as ProbeGlobal;
    if (!scope.__donnerSurfaceFrameProbe?.installed) return false;
    if (scope.__donnerSurfaceFrameProbeCompletionHold !== undefined) return false;
    let release!: () => void;
    const promise = new Promise<void>((resolve) => {
      release = resolve;
    });
    scope.__donnerSurfaceFrameProbeCompletionHold = { promise, release, calls: 0 };
    return true;
  });
  if (armed !== true) {
    throw new Error("could not hold the canvas completions");
  }
  return {
    release: async () => {
      await owner.evaluate(() => {
        const scope = globalThis as ProbeGlobal;
        scope.__donnerSurfaceFrameProbeCompletionHold?.release();
        delete scope.__donnerSurfaceFrameProbeCompletionHold;
      });
    },
  };
}

/**
 * Take one screenshot through \p capture of \p owner's quiescent canvas: under a hold of the app's
 * canvas completions, once the GPU has run every canvas write, accepted only if no canvas frame
 * was acquired or written until the screenshot ended (see canvas-quiescent-capture.mjs). Its only
 * time bound is the stall guard, unless `untilMs` names an earlier end on the `performance.now()`
 * clock. `timedOut` recognizes a capture result that ran out of time.
 */
export async function captureQuiescentCanvasFrame<T>(
  owner: Worker,
  capture: (timeoutMs: number) => Promise<T>,
  options: {
    timedOut?: (result: T) => boolean;
    untilMs?: number;
    retakeUnheld?: boolean;
    passes?: Array<Record<string, unknown>>;
  } = {},
): Promise<{ result: T; passes: Array<Record<string, unknown>> }> {
  return captureQuiescentCanvas(
    {
      hold: () => holdCanvasCompletions(owner),
      settle: () => settleCanvasWrites(owner),
      activity: () => readCanvasActivity(owner),
    },
    capture,
    {
      untilMs: options.untilMs ?? performance.now() + kStallGuardMs,
      timedOut: options.timedOut,
      retakeUnheld: options.retakeUnheld,
      passes: options.passes,
      isTimeoutError: (error: unknown) => error instanceof errors.TimeoutError,
    },
  );
}

/**
 * Fixed fragment iterations of the first attempt of \ref captureAfterDelayedFrame: about 100-400 ms
 * of GPU work on Apple Silicon, leaving a slower GPU a wide margin inside the app's 5 s completion
 * deadline. Each later attempt doubles it.
 */
export const kSingleFrameGpuDelayIterations = 20000;

/** Attempts \ref captureAfterDelayedFrame makes before giving up on a GPU too fast to delay. */
const kDelayedFrameAttempts = 3;

/**
 * Request one editor frame of \p owner's canvas with its GPU work postponed behind fixed fragment
 * work queued after its first canvas write, then run \p gate. Succeeds once the gate's settle wait
 * began while that work was still running, as the probe records in the worker itself; a GPU that
 * finished first proves nothing, so the frame is requested again with twice the work. Returns the
 * gate's result, the canvas activity when the frame was submitted, and what was injected.
 */
export async function captureAfterDelayedFrame<T>(
  page: Page,
  owner: Worker,
  gate: () => Promise<T>,
): Promise<{ result: T; frame: CanvasActivity; delay: CanvasGpuDelayResult }> {
  const attempts: Array<{ frame: CanvasActivity | null; delay: CanvasGpuDelayResult | null }> = [];
  for (let attempt = 0; attempt < kDelayedFrameAttempts; ++attempt) {
    const delay = await armCanvasGpuDelayForTest(
      owner,
      1,
      kSingleFrameGpuDelayIterations * 2 ** attempt,
      1,
    );
    let frame: CanvasActivity | null = null;
    let result: T;
    let report: CanvasGpuDelayResult | null;
    try {
      const before = await readCanvasActivity(owner);
      await page.evaluate(() => {
        (window as Window & { __donnerEditorFrameRequested?: boolean })
          .__donnerEditorFrameRequested = true;
      });
      await expect.poll(async () => {
        const activity = await readCanvasActivity(owner);
        if (activity.canvasSubmits > before.canvasSubmits) frame ??= activity;
        return activity.canvasSubmits;
      }, {
        message: "a frame is submitted with its GPU work delayed",
        timeout: kStallGuardMs,
      }).toBeGreaterThan(before.canvasSubmits);
      result = await gate();
    } finally {
      report = await delay.disarm();
    }
    attempts.push({ frame, delay: report });
    if (report !== null && report.injected > 0 && report.settleSawPending && frame !== null) {
      return { result, frame, delay: report };
    }
  }
  throw new Error(
    `the delayed frame's GPU work finished before the gate began: ${JSON.stringify(attempts)}`,
  );
}
