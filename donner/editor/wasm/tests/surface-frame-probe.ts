import { expect, type Page, type Worker } from "@playwright/test";
import { createSurfaceProbeRegistry } from "./surface-probe-requests.mjs";

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
  lateSubmits: LateCanvasSubmit[];
}

type ProbeGlobal = typeof globalThis & {
  __donnerSurfaceFrameProbe?: WorkerProbeState;
  /** Resolves in a task that runs after every task boundary the probe has marked so far. */
  __donnerSurfaceFrameProbeNextTask?: () => Promise<void>;
  __donnerSurfaceFrameProbeQueue?: GPUQueue;
  __donnerSurfaceFrameProbeCompletion?: { submissions: number; promise: Promise<void> };
  __donnerSurfaceFrameProbeCompletionHold?: {
    promise: Promise<void>;
    release: () => void;
    calls: number;
    waits: number;
  };
  __donnerSurfaceFrameProbeLateCompletion?: {
    promise: Promise<void>;
    release: () => void;
    claimed: boolean;
  };
  __donnerSurfaceFrameProbeGpuDelay?: CanvasGpuDelayState;
};

/**
 * Test-only fault injection: after the first command buffer that writes every Nth canvas frame,
 * queue GPU work of a fixed size before the frame's remaining submissions. The frame's CPU task
 * and submission order are unchanged; only GPU execution of the rest of that frame is postponed.
 */
interface CanvasGpuDelayState {
  everyFrames: number;
  fragmentIterations: number;
  injected: number;
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
    void onSubmittedWorkDone.call(queue).then(() => {
      if (delay.jobMs.length < 256) delay.jobMs.push(performance.now() - startedAtMs);
    });
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
      scope.__donnerSurfaceFrameProbeCompletion = {
        submissions: pendingCanvasSubmit,
        promise,
      };
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
            && record.frame % delay.everyFrames === 0;
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
      pendingCanvasSubmit = ++state.canvasSubmits;
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

// Await the existing completion promise for the last canvas-writing submission.
// This proves submitted GPU work finished; it does not acknowledge compositing.
function waitForCanvasGpuWorkInWorker(): Promise<number> {
  const completion = (globalThis as ProbeGlobal).__donnerSurfaceFrameProbeCompletion;
  if (completion === undefined) {
    throw new Error("the probe has not observed a canvas completion promise");
  }
  const hold = (globalThis as ProbeGlobal).__donnerSurfaceFrameProbeCompletionHold;
  if (hold !== undefined) ++hold.waits;
  return completion.promise.then(() => completion.submissions);
}

export function waitForSubmittedCanvasGpuWork(owner: Worker): Promise<number> {
  return owner.evaluate(waitForCanvasGpuWorkInWorker);
}

/** Diagnostic timing for the same existing canvas completion promise; submits no GPU work. */
export async function observeSubmittedCanvasGpuWork(owner: Worker) {
  const dispatchedEpochMs = Date.now();
  const observed = await owner.evaluate(async () => {
    const enteredEpochMs = Date.now();
    const completion = (globalThis as ProbeGlobal).__donnerSurfaceFrameProbeCompletion;
    if (completion === undefined) {
      throw new Error("the probe has not observed a canvas completion promise");
    }
    const hold = (globalThis as ProbeGlobal).__donnerSurfaceFrameProbeCompletionHold;
    if (hold !== undefined) ++hold.waits;
    await completion.promise;
    return { enteredEpochMs, resolvedEpochMs: Date.now(), submissions: completion.submissions };
  });
  return { dispatchedEpochMs, ...observed, repliedEpochMs: Date.now() };
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

// Control only the completion signal already returned to the app. Used by a
// browser regression to prove CPU-published sample state cannot bypass the
// screenshot's GPU gate. No queue work is added or delayed on ordinary paths.
export async function holdCanvasCompletionForTest(
  _page: Page,
  workersForTest: Worker[],
): Promise<{
  observedCalls: () => Promise<number>;
  enteredWaits: () => Promise<number>;
  release: (owner: Worker) => Promise<void>;
}> {
  const workers = workersForTest;
  const armed = await evaluateInWorkers(workers, () => {
    const scope = globalThis as ProbeGlobal;
    let release!: () => void;
    const promise = new Promise<void>((resolve) => {
      release = resolve;
    });
    scope.__donnerSurfaceFrameProbeCompletionHold = { promise, release, calls: 0, waits: 0 };
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
    enteredWaits: async () => {
      const counts = await evaluateInWorkers(
        workers,
        () => (globalThis as ProbeGlobal).__donnerSurfaceFrameProbeCompletionHold?.waits ?? 0,
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
): Promise<{ disarm: () => Promise<{ injected: number; jobMs: number[] } | null> }> {
  if (!Number.isInteger(everyFrames) || everyFrames < 1) {
    throw new Error("the canvas GPU delay needs a positive frame interval");
  }
  const armed = await owner.evaluate(([frames, iterations]) => {
    const scope = globalThis as ProbeGlobal;
    if (!scope.__donnerSurfaceFrameProbe?.installed) return false;
    scope.__donnerSurfaceFrameProbeGpuDelay = {
      everyFrames: frames,
      fragmentIterations: iterations,
      injected: 0,
      jobMs: [],
    };
    return true;
  }, [everyFrames, fragmentIterations]);
  if (armed !== true) {
    throw new Error("could not arm the canvas GPU delay");
  }
  return {
    disarm: async () => {
      return owner.evaluate(() => {
        const scope = globalThis as ProbeGlobal;
        const delay = scope.__donnerSurfaceFrameProbeGpuDelay;
        delete scope.__donnerSurfaceFrameProbeGpuDelay;
        return delay === undefined ? null : { injected: delay.injected, jobMs: [...delay.jobMs] };
      });
    },
  };
}
