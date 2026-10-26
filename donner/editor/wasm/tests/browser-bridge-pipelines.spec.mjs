// Drives pipeline creation in the GPU bridge library against a stub device that models where a
// browser spends a pipeline compile, in a plain JavaScript runtime.
//
// A browser compiles a pipeline created with createRenderPipeline in order with the rest of the
// device's work, so every submission behind it waits for the compile; createRenderPipelineAsync
// compiles off that order and settles later. The stub keeps one in-order device timeline: a
// synchronous creation occupies it until the test finishes that compile, an asynchronous one
// settles when the test finishes it and occupies nothing, and a submission completes once
// everything ahead of it on the timeline has. No case waits on time: every compile finishes when
// the test says so.

import assert from "node:assert/strict";
import test from "node:test";

import { drain, loadLibrary, until } from "./bridge-library-harness.mjs";

const kFirst = 1;
const kSecond = 2;

const kShaderSource = "@vertex fn vs() {} @fragment fn fsA() {} @fragment fn fsB() {}";

/** A stub device with one in-order timeline of compiles and submissions. */
function createTimelineDevice({ asyncCompiles = true, refuseAsync = false } = {}) {
  const timeline = [];
  const compiles = new Map(); // Fragment entry point to { finished, settle: [] }.
  const created = { sync: [], async: [] };
  const compileFor = (descriptor) => {
    const name = descriptor.fragment
      ? descriptor.fragment.entryPoint
      : descriptor.compute.entryPoint;
    let compile = compiles.get(name);
    if (compile === undefined) {
      compile = { finished: false, settle: [] };
      compiles.set(name, compile);
    }
    return { name, compile };
  };
  const advance = () => {
    while (timeline.length > 0) {
      const head = timeline[0];
      if (head.kind === "compile" && !head.compile.finished) return;
      timeline.shift();
      if (head.kind === "done") head.resolve();
    }
  };
  const queue = {
    submissions: [],
    submit(buffers) {
      this.submissions.push(buffers);
    },
    writeBuffer() {},
    onSubmittedWorkDone() {
      return new Promise((resolve) => {
        timeline.push({ kind: "done", resolve });
        advance();
      });
    },
  };
  const pass = (draws) => ({
    pipeline: null,
    setPipeline(pipeline) {
      this.pipeline = pipeline;
    },
    draw() {
      draws.push(this.pipeline);
    },
    end() {},
  });
  const device = {
    queue,
    lost: new Promise(() => {}),
    limits: { maxTextureDimension2D: 8192 },
    draws: [],
    createShaderModule: (descriptor) => ({ descriptor }),
    createBindGroupLayout: (descriptor) => ({ descriptor }),
    createPipelineLayout: (descriptor) => ({ descriptor }),
    createTexture: (descriptor) => ({ descriptor, destroy() {}, createView: () => ({}) }),
    createRenderPipeline(descriptor) {
      const { name, compile } = compileFor(descriptor);
      created.sync.push(name);
      timeline.push({ kind: "compile", compile });
      return { name, synchronous: true };
    },
    createCommandEncoder() {
      return {
        beginRenderPass: () => pass(device.draws),
        finish: () => ({}),
      };
    },
  };
  if (asyncCompiles) {
    device.createRenderPipelineAsync = (descriptor) => {
      const { name, compile } = compileFor(descriptor);
      created.async.push(name);
      return new Promise((resolve, reject) => {
        compile.settle.push(() =>
          refuseAsync ? reject(new Error("refused")) : resolve({ name, synchronous: false })
        );
        if (compile.finished) compile.settle.splice(0).forEach((settle) => settle());
      });
    };
  }
  return {
    device,
    created,
    /** Finishes the compile of the pipeline whose fragment entry point is `name`. */
    finishCompile(name) {
      const compile = compiles.get(name);
      assert.ok(compile, `nothing has asked to compile ${name}`);
      compile.finished = true;
      compile.settle.splice(0).forEach((settle) => settle());
      advance();
    },
  };
}

/** Loads the library over `stub` and makes `handles` ready on its device. */
async function openOver(stub, handles, { userAgent = undefined } = {}) {
  const bridge = loadLibrary({
    requestAdapterForTesting: async () => ({ requestDevice: async () => stub.device }),
    userAgentForTesting: userAgent,
  });
  const { entryPoints, state } = bridge;
  for (const handle of handles) {
    assert.equal(entryPoints.donner_gpu_begin_device_request(handle), state.kSuccess);
  }
  await until(
    () => entryPoints.donner_gpu_device_request_state(handles[0]) === state.kRequestReady,
    "a ready device",
  );
  return bridge;
}

// Identifiers each logical device uses for the objects it builds.
const kModule = 1;
const kGroupLayout = 2;
const kLayout = 3;
const kPipelineA = 4;
const kPipelineB = 5;
const kTexture = 6;
const kView = 7;

/** Builds a shader module, its layouts and the pipelines named by `fragments` on `handle`. */
function buildPipelines(bridge, handle, fragments) {
  const { entryPoints: gpu, state } = bridge;
  const source = bridge.string(kShaderSource);
  assert.equal(
    gpu.donner_gpu_create_shader_module(handle, kModule, source.pointer, source.length),
    state.kSuccess,
  );
  assert.equal(gpu.donner_gpu_bind_group_layout_begin(handle), state.kSuccess);
  assert.equal(gpu.donner_gpu_bind_group_layout_entry(handle, 0, 3, 1, 0), state.kSuccess);
  assert.equal(gpu.donner_gpu_bind_group_layout_finish(handle, kGroupLayout), state.kSuccess);
  assert.equal(gpu.donner_gpu_pipeline_layout_begin(handle), state.kSuccess);
  assert.equal(gpu.donner_gpu_pipeline_layout_group(handle, kGroupLayout), state.kSuccess);
  assert.equal(gpu.donner_gpu_pipeline_layout_finish(handle, kLayout), state.kSuccess);
  const vertex = bridge.string("vs");
  for (const [id, name] of fragments) {
    const fragment = bridge.string(name);
    assert.equal(
      gpu.donner_gpu_render_pipeline_begin(
        handle,
        kLayout,
        kModule,
        vertex.pointer,
        vertex.length,
        kModule,
        fragment.pointer,
        fragment.length,
        1,
        1,
      ),
      state.kSuccess,
    );
    assert.equal(
      gpu.donner_gpu_render_pipeline_color_target(handle, 1, 0, 0, 0, 0, 0, 0, 0, 15),
      state.kSuccess,
    );
    assert.equal(gpu.donner_gpu_render_pipeline_finish(handle, id), state.kSuccess);
  }
  assert.equal(gpu.donner_gpu_create_texture(handle, kTexture, 4, 4, 1, 1), state.kSuccess);
  assert.equal(gpu.donner_gpu_create_texture_view(handle, kView, kTexture), state.kSuccess);
}

/** Records and submits one draw with `pipelineId` on `handle` as submission `serial`. */
function submitDraw(bridge, handle, serial, pipelineId) {
  const { entryPoints: gpu, state } = bridge;
  assert.equal(gpu.donner_gpu_begin_command_buffer(handle, serial, 0), state.kSuccess);
  assert.equal(gpu.donner_gpu_begin_render_pass(handle), state.kSuccess);
  assert.equal(
    gpu.donner_gpu_render_pass_attachment(handle, kView, 1, 1, 0, 0, 0, 0),
    state.kSuccess,
  );
  assert.equal(gpu.donner_gpu_begin_render_pass_finish(handle), state.kSuccess);
  assert.equal(gpu.donner_gpu_set_render_pipeline(handle, pipelineId), state.kSuccess);
  assert.equal(gpu.donner_gpu_draw(handle, 3, 1, 0, 0), state.kSuccess);
  assert.equal(gpu.donner_gpu_end_render_pass(handle), state.kSuccess);
  assert.equal(gpu.donner_gpu_end_command_buffer(handle, serial), state.kSuccess);
  assert.equal(gpu.donner_gpu_submit_command_buffers(handle, serial), state.kSuccess);
}

test("a compiled pipeline's submission completes while another pipeline is still compiling", async () => {
  const stub = createTimelineDevice();
  const bridge = await openOver(stub, [kFirst]);
  buildPipelines(bridge, kFirst, [[kPipelineA, "fsA"], [kPipelineB, "fsB"]]);
  await drain();
  stub.finishCompile("fsA");
  await drain();

  submitDraw(bridge, kFirst, 1, kPipelineA);
  // fsB never finishes compiling in this case. A creation that compiled in order with the device
  // would hold this submission behind that compile forever.
  await until(
    () => bridge.entryPoints.donner_gpu_completed_serial(kFirst) === 1,
    "the submission using the compiled pipeline completes",
  );
  assert.deepEqual(stub.created, { sync: [], async: ["fsA", "fsB"] });
  assert.deepEqual(stub.device.draws.map((pipeline) => pipeline.name), ["fsA"]);
  assert.equal(bridge.state.debugStatistics()["pipelineCompilesOnFirstUse"], 0);
});

test("a pipeline used before its compile settles is created at that use, once", async () => {
  const stub = createTimelineDevice();
  const bridge = await openOver(stub, [kFirst]);
  buildPipelines(bridge, kFirst, [[kPipelineA, "fsA"]]);
  await drain();
  assert.deepEqual(stub.created.async, ["fsA"]);

  submitDraw(bridge, kFirst, 1, kPipelineA);
  submitDraw(bridge, kFirst, 2, kPipelineA);
  assert.deepEqual(stub.created.sync, ["fsA"]);
  assert.deepEqual(stub.device.draws.map((pipeline) => pipeline.synchronous), [true, true]);
  assert.equal(bridge.state.debugStatistics()["pipelineCompilesOnFirstUse"], 1);

  // The asynchronous compile settling afterwards does not replace the pipeline already in use.
  stub.finishCompile("fsA");
  await until(
    () => bridge.entryPoints.donner_gpu_completed_serial(kFirst) === 2,
    "both submissions complete once the compile finishes",
  );
  submitDraw(bridge, kFirst, 3, kPipelineA);
  assert.deepEqual(stub.device.draws.map((pipeline) => pipeline.synchronous), [true, true, true]);
});

test("identical pipelines on two logical devices compile once and share the result", async () => {
  const stub = createTimelineDevice();
  const bridge = await openOver(stub, [kFirst, kSecond]);
  buildPipelines(bridge, kFirst, [[kPipelineA, "fsA"]]);
  buildPipelines(bridge, kSecond, [[kPipelineA, "fsA"], [kPipelineB, "fsB"]]);
  await drain();
  assert.deepEqual(stub.created.async, ["fsA", "fsB"]);
  stub.finishCompile("fsA");
  await drain();

  submitDraw(bridge, kFirst, 1, kPipelineA);
  submitDraw(bridge, kSecond, 1, kPipelineA);
  const [first, second] = stub.device.draws;
  assert.equal(first.synchronous, false);
  assert.equal(first, second, "both logical devices drew with the one compiled pipeline");
  const statistics = bridge.state.debugStatistics();
  assert.deepEqual(
    {
      compiles: statistics["pipelineCompiles"],
      shared: statistics["pipelineCompilesShared"],
      onFirstUse: statistics["pipelineCompilesOnFirstUse"],
    },
    { compiles: 2, shared: 1, onFirstUse: 0 },
  );
});

test("a compile the browser refuses is created at first use, where the browser reports it", async () => {
  const stub = createTimelineDevice({ refuseAsync: true });
  const bridge = await openOver(stub, [kFirst]);
  buildPipelines(bridge, kFirst, [[kPipelineA, "fsA"]]);
  await drain();
  stub.finishCompile("fsA");
  await drain();

  submitDraw(bridge, kFirst, 1, kPipelineA);
  assert.deepEqual(stub.created, { sync: ["fsA"], async: ["fsA"] });
  assert.equal(bridge.state.debugStatistics()["pipelineCompilesOnFirstUse"], 1);
});

test("a browser without asynchronous creation creates each pipeline at first use", async () => {
  const stub = createTimelineDevice({ asyncCompiles: false });
  const bridge = await openOver(stub, [kFirst]);
  buildPipelines(bridge, kFirst, [[kPipelineA, "fsA"], [kPipelineB, "fsB"]]);
  await drain();
  assert.deepEqual(stub.created.sync, []);

  submitDraw(bridge, kFirst, 1, kPipelineB);
  assert.deepEqual(stub.created.sync, ["fsB"]);
});

test("a compile that settles after its device is let go installs nothing", async () => {
  const stub = createTimelineDevice();
  const bridge = await openOver(stub, [kFirst]);
  buildPipelines(bridge, kFirst, [[kPipelineA, "fsA"]]);
  await drain();
  const { state, entryPoints } = bridge;
  const compile = state.pipelines.values().next().value;
  entryPoints.donner_gpu_release_device(kFirst);
  stub.finishCompile("fsA");
  await drain();
  assert.equal(compile.pipeline, null);
  assert.equal(state.pipelines.size, 0);
});

test("a pipeline created and used within one task is compiled once, at that use", async () => {
  const stub = createTimelineDevice();
  const bridge = await openOver(stub, [kFirst]);
  // Pipelines built on demand are created by the pass that first needs them. Starting an
  // asynchronous compile as well would make the browser compile them twice.
  buildPipelines(bridge, kFirst, [[kPipelineA, "fsA"]]);
  submitDraw(bridge, kFirst, 1, kPipelineA);
  await drain();
  assert.deepEqual(stub.created, { sync: ["fsA"], async: [] });
});

test("a device let go before its compiles start starts none of them", async () => {
  const first = createTimelineDevice();
  const bridge = await openOver(first, [kFirst]);
  buildPipelines(bridge, kFirst, [[kPipelineA, "fsA"]]);
  bridge.entryPoints.donner_gpu_release_device(kFirst);
  await drain();
  assert.deepEqual(first.created.async, []);
  assert.equal(bridge.state.debugStatistics()["pipelineCompiles"], 0);
});

test("a pipeline the browser refuses at first use is not asked for again", async () => {
  const stub = createTimelineDevice({ asyncCompiles: false });
  stub.device.createRenderPipeline = () => {
    throw new Error("refused");
  };
  const bridge = await openOver(stub, [kFirst]);
  buildPipelines(bridge, kFirst, [[kPipelineA, "fsA"]]);
  const { entryPoints: gpu, state } = bridge;
  for (const serial of [1, 2]) {
    assert.equal(gpu.donner_gpu_begin_command_buffer(kFirst, serial, 0), state.kSuccess);
    assert.equal(gpu.donner_gpu_begin_render_pass(kFirst), state.kSuccess);
    assert.equal(
      gpu.donner_gpu_render_pass_attachment(kFirst, kView, 1, 1, 0, 0, 0, 0),
      state.kSuccess,
    );
    assert.equal(gpu.donner_gpu_begin_render_pass_finish(kFirst), state.kSuccess);
    assert.equal(gpu.donner_gpu_set_render_pipeline(kFirst, kPipelineA), state.kFailed);
  }
  assert.equal(state.debugStatistics()["pipelineCompilesOnFirstUse"], 1);
});

test("an asynchronous compile the browser refuses is reported on the console", async () => {
  const stub = createTimelineDevice({ refuseAsync: true });
  const bridge = await openOver(stub, [kFirst]);
  buildPipelines(bridge, kFirst, [[kPipelineA, "fsA"]]);
  await drain();
  stub.finishCompile("fsA");
  await drain();
  assert.deepEqual(bridge.consoleErrors, ["[Geode/browser] Pipeline compile refused: refused"]);
});

test("a compile refused after its device is let go reports nothing", async () => {
  const stub = createTimelineDevice({ refuseAsync: true });
  const bridge = await openOver(stub, [kFirst]);
  buildPipelines(bridge, kFirst, [[kPipelineA, "fsA"]]);
  await drain();
  bridge.entryPoints.donner_gpu_release_device(kFirst);
  stub.finishCompile("fsA");
  await drain();
  assert.deepEqual(bridge.consoleErrors, []);
});

// Representative user agents in each browser's format.
const kSafariMac =
  "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) "
  + "Version/26.5 Safari/605.1.15";
const kSafariIPhone =
  "Mozilla/5.0 (iPhone; CPU iPhone OS 26_5 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) "
  + "Version/26.5 Mobile/15E148 Safari/604.1";
const kChromeIPhone =
  "Mozilla/5.0 (iPhone; CPU iPhone OS 26_5 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) "
  + "CriOS/156.0.8078.4 Mobile/15E148 Safari/604.1";
const kFirefoxIPhone =
  "Mozilla/5.0 (iPhone; CPU iPhone OS 26_5 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) "
  + "FxiOS/157.0 Mobile/15E148 Safari/605.1.15";
const kChromeMac =
  "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) "
  + "Chrome/156.0.0.0 Safari/537.36";
const kEdgeWindows =
  "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
  + "Chrome/156.0.0.0 Safari/537.36 Edg/156.0.0.0";
const kChromiumLinux =
  "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Ubuntu "
  + "Chromium/156.0.0.0 Chrome/156.0.0.0 Safari/537.36";
const kHeadlessChrome =
  "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) "
  + "HeadlessChrome/156.0.0.0 Safari/537.36";
const kOperaWindows =
  "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
  + "Chrome/156.0.0.0 Safari/537.36 OPR/141.0.0.0";
const kSamsungAndroid =
  "Mozilla/5.0 (Linux; Android 15; SM-S928B) AppleWebKit/537.36 (KHTML, like Gecko) "
  + "SamsungBrowser/29.0 Chrome/156.0.0.0 Mobile Safari/537.36";
const kAndroidWebView =
  "Mozilla/5.0 (Linux; Android 15; Pixel 9; wv) AppleWebKit/537.36 (KHTML, like Gecko) "
  + "Version/4.0 Chrome/156.0.0.0 Mobile Safari/537.36";
const kEdgeIPhone =
  "Mozilla/5.0 (iPhone; CPU iPhone OS 26_5 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) "
  + "EdgiOS/156.0.0.0 Version/26.0 Mobile/15E148 Safari/604.1";
const kFirefoxMac =
  "Mozilla/5.0 (Macintosh; Intel Mac OS X 10.15; rv:157.0) Gecko/20100101 Firefox/157.0";

test("only WebKit, recognized by its user agent, skips the compile ahead of use", () => {
  const aheadOfUse = (userAgent) =>
    loadLibrary({ userAgentForTesting: userAgent }).state.compilesAheadOfUse();
  const webKit = [kSafariMac, kSafariIPhone, kChromeIPhone, kFirefoxIPhone, kEdgeIPhone];
  const other = [
    kChromeMac,
    kHeadlessChrome,
    kEdgeWindows,
    kOperaWindows,
    kSamsungAndroid,
    kAndroidWebView,
    kChromiumLinux,
    kFirefoxMac,
    "",
    undefined,
  ];
  assert.deepEqual(webKit.map(aheadOfUse), webKit.map(() => false));
  assert.deepEqual(other.map(aheadOfUse), other.map(() => true));
});

test("WebKit starts no compile ahead of use and creates each pipeline once, at its first use", async () => {
  const stub = createTimelineDevice();
  const bridge = await openOver(stub, [kFirst, kSecond], { userAgent: kSafariMac });
  buildPipelines(bridge, kFirst, [[kPipelineA, "fsA"], [kPipelineB, "fsB"]]);
  buildPipelines(bridge, kSecond, [[kPipelineA, "fsA"]]);
  await drain();
  assert.deepEqual(stub.created, { sync: [], async: [] });

  submitDraw(bridge, kFirst, 1, kPipelineA);
  submitDraw(bridge, kSecond, 1, kPipelineA);
  assert.deepEqual(stub.created, { sync: ["fsA"], async: [] });
  const [first, second] = stub.device.draws;
  assert.equal(first, second, "both logical devices drew with the one pipeline");
  const statistics = bridge.state.debugStatistics();
  assert.deepEqual(
    {
      compiles: statistics["pipelineCompiles"],
      shared: statistics["pipelineCompilesShared"],
      onFirstUse: statistics["pipelineCompilesOnFirstUse"],
    },
    { compiles: 0, shared: 1, onFirstUse: 1 },
  );
});

test("Chromium and Firefox still compile ahead of use", async () => {
  for (const userAgent of [kChromeMac, kFirefoxMac]) {
    const stub = createTimelineDevice();
    const bridge = await openOver(stub, [kFirst], { userAgent });
    buildPipelines(bridge, kFirst, [[kPipelineA, "fsA"], [kPipelineB, "fsB"]]);
    await drain();
    assert.deepEqual(stub.created, { sync: [], async: ["fsA", "fsB"] }, userAgent);
    assert.equal(bridge.state.debugStatistics()["pipelineCompiles"], 2, userAgent);
  }
});

test("pending compiles count each compile until it settles, even after its device is let go", async () => {
  const stub = createTimelineDevice();
  const bridge = await openOver(stub, [kFirst]);
  const pending = () => bridge.state.debugStatistics()["pipelineCompilesPending"];
  assert.equal(pending(), 0);
  buildPipelines(bridge, kFirst, [[kPipelineA, "fsA"], [kPipelineB, "fsB"]]);
  // Waiting for the creating task to end still counts: the compiles are about to start.
  assert.equal(pending(), 2);
  await drain();
  assert.deepEqual(stub.created.async, ["fsA", "fsB"]);
  assert.equal(pending(), 2);
  stub.finishCompile("fsA");
  await drain();
  assert.equal(pending(), 1);
  // A compile that outlives its device still occupies the browser until it settles.
  bridge.entryPoints.donner_gpu_release_device(kFirst);
  assert.equal(pending(), 1);
  stub.finishCompile("fsB");
  await drain();
  assert.equal(pending(), 0);
});

test("a refused compile stops counting as pending once the browser answers", async () => {
  const stub = createTimelineDevice({ refuseAsync: true });
  const bridge = await openOver(stub, [kFirst]);
  buildPipelines(bridge, kFirst, [[kPipelineA, "fsA"]]);
  await drain();
  assert.equal(bridge.state.debugStatistics()["pipelineCompilesPending"], 1);
  stub.finishCompile("fsA");
  await drain();
  assert.equal(bridge.state.debugStatistics()["pipelineCompilesPending"], 0);
});

test("pipelines created at first use, or in WebKit, leave nothing pending", async () => {
  const onUse = createTimelineDevice();
  const used = await openOver(onUse, [kFirst]);
  buildPipelines(used, kFirst, [[kPipelineA, "fsA"]]);
  submitDraw(used, kFirst, 1, kPipelineA);
  await drain();
  assert.equal(used.state.debugStatistics()["pipelineCompilesPending"], 0);

  const webKit = createTimelineDevice();
  const bridge = await openOver(webKit, [kFirst], { userAgent: kSafariMac });
  buildPipelines(bridge, kFirst, [[kPipelineA, "fsA"], [kPipelineB, "fsB"]]);
  await drain();
  assert.equal(bridge.state.debugStatistics()["pipelineCompilesPending"], 0);
});
