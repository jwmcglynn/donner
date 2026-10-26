import assert from "node:assert/strict";
import test from "node:test";
import vm from "node:vm";
import { pipelineCompileFaultPrologue, readPipelineCompileFault,
  sumPipelineCompilesPending } from "./pipeline-compile-readiness.mjs";

test("pending compiles sum over the contexts that report them", () => {
  assert.deepEqual(sumPipelineCompilesPending([]), { pending: 0, reporting: 0 });
  assert.deepEqual(
    sumPipelineCompilesPending([null, undefined, {}, { pipelineCompilesPending: "1" }]),
    { pending: 0, reporting: 0 },
  );
  assert.deepEqual(
    sumPipelineCompilesPending([{ pipelineCompilesPending: 3 }, null, { pipelineCompilesPending: 0 }]),
    { pending: 3, reporting: 2 },
  );
  assert.deepEqual(
    sumPipelineCompilesPending([{ pipelineCompilesPending: Number.NaN }]),
    { pending: 0, reporting: 0 },
  );
});

/** A worker-like context with stub WebGPU classes and a clock the test advances. */
function createWorkerContext({ worker = true } = {}) {
  const timers = [];
  let clock = 0;
  const settlers = [];
  class GPUDevice {
    createRenderPipelineAsync(descriptor) {
      return new Promise((resolve, reject) => settlers.push({ descriptor, resolve, reject }));
    }
  }
  const completions = [];
  class GPUQueue {
    onSubmittedWorkDone() {
      return new Promise((resolve) => completions.push(resolve));
    }
  }
  class GPUBuffer {
    mapAsync() {
      return Promise.resolve("mapped");
    }
  }
  const sandbox = {
    GPUDevice,
    GPUQueue,
    GPUBuffer,
    performance: { now: () => clock },
    setTimeout: (callback, delay) => timers.push({ at: clock + delay, callback }),
  };
  if (worker) {
    sandbox.WorkerGlobalScope = class {
      static [Symbol.hasInstance]() {
        return true;
      }
    };
  }
  const context = vm.createContext(sandbox);
  return {
    context,
    sandbox,
    settlers,
    completions,
    run: (source) => vm.runInContext(source, context),
    /** Advances the clock to `to`, running each timer that becomes due, then drains promises. */
    async advance(to) {
      clock = to;
      for (;;) {
        const due = timers.findIndex(({ at }) => at <= clock);
        if (due < 0) break;
        timers.splice(due, 1)[0].callback();
        await new Promise((resolve) => setImmediate(resolve));
      }
      await new Promise((resolve) => setImmediate(resolve));
    },
  };
}

const kConfig = { compileDelayMs: 1000, completionDelayMs: 300 };
// Read through JSON: an object from the worker context has that context's prototypes.
const readFault = (worker) =>
  JSON.parse(worker.run(`JSON.stringify((${readPipelineCompileFault.toString()})())`));

test("the fault installs only in a worker with WebGPU, once", () => {
  const page = createWorkerContext({ worker: false });
  page.run(pipelineCompileFaultPrologue(kConfig));
  assert.equal(readFault(page), null);

  const worker = createWorkerContext();
  worker.run(pipelineCompileFaultPrologue(kConfig));
  const installed = worker.sandbox.GPUDevice.prototype.createRenderPipelineAsync;
  worker.run(pipelineCompileFaultPrologue(kConfig));
  assert.equal(worker.sandbox.GPUDevice.prototype.createRenderPipelineAsync, installed);
  assert.deepEqual(readFault(worker), {
    ...kConfig,
    compilesDelayed: 0,
    compilesPending: 0,
    completionsDelayed: 0,
    lastCompileSettledAtMs: 0,
  });
});

test("a compile settles no earlier than its delay and holds completions while it is pending", async () => {
  const worker = createWorkerContext();
  worker.run(pipelineCompileFaultPrologue(kConfig));
  const device = new worker.sandbox.GPUDevice();
  const queue = new worker.sandbox.GPUQueue();
  let pipeline = null;
  device.createRenderPipelineAsync({ name: "a" }).then((value) => {
    pipeline = value;
  });
  let refused = null;
  device.createRenderPipelineAsync({ name: "b" }).catch((error) => {
    refused = error.message;
  });

  // The browser finishes both compiles at once; the fault holds them to their deadline.
  worker.settlers[0].resolve("pipeline a");
  worker.settlers[1].reject(new Error("refused b"));
  await worker.advance(10);
  assert.equal(pipeline, null);
  assert.equal(readFault(worker).compilesPending, 2);

  // A completion requested while compiles are pending arrives late, after the browser's own.
  let completedAt = null;
  queue.onSubmittedWorkDone().then(() => {
    completedAt = worker.sandbox.performance.now();
  });
  worker.completions.shift()();
  await worker.advance(200);
  assert.equal(completedAt, null);
  await worker.advance(310);
  assert.equal(completedAt, 310);

  // A completion held near the end of the compiles is released when they drain, not later.
  await worker.advance(800);
  let lateCompletedAt = null;
  queue.onSubmittedWorkDone().then(() => {
    lateCompletedAt = worker.sandbox.performance.now();
  });
  worker.completions.shift()();
  await worker.advance(900);
  assert.equal(lateCompletedAt, null);

  await worker.advance(1000);
  assert.equal(lateCompletedAt, 1000);
  assert.equal(pipeline, "pipeline a");
  assert.equal(refused, "refused b");
  assert.deepEqual(readFault(worker), {
    ...kConfig,
    compilesDelayed: 2,
    compilesPending: 0,
    completionsDelayed: 2,
    lastCompileSettledAtMs: 1000,
  });

  // With nothing pending, completions are the browser's own.
  let completed = false;
  queue.onSubmittedWorkDone().then(() => {
    completed = true;
  });
  worker.completions.shift()();
  await worker.advance(1000);
  assert.equal(completed, true);
  assert.equal(readFault(worker).completionsDelayed, 2);
});
