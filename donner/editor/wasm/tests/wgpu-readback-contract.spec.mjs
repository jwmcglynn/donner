import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

const source = await readFile(new URL("../../gui/EditorWindow.cc", import.meta.url), "utf8");
const workerRendererSource = await readFile(
  new URL("../../AsyncRenderer.cc", import.meta.url),
  "utf8",
);
const workerRendererHeader = await readFile(
  new URL("../../AsyncRenderer.h", import.meta.url),
  "utf8",
);
const renderCoordinatorSource = await readFile(
  new URL("../../RenderCoordinator.cc", import.meta.url),
  "utf8",
);
const editorShellSource = await readFile(
  new URL("../../EditorShell.cc", import.meta.url),
  "utf8",
);
const rotateCursorSource = await readFile(
  new URL("../../RotateCursorSet.cc", import.meta.url),
  "utf8",
);
const presentationRegressionSource = await readFile(
  new URL("./browser-presentation-regression.spec.ts", import.meta.url),
  "utf8",
);
const compositedDragSource = await readFile(
  new URL("./composited-drag-invariants.spec.ts", import.meta.url),
  "utf8",
);
const compositedViewportSource = await readFile(
  new URL("composited-invariants.spec.ts", import.meta.url),
  "utf8",
);
const geodeDeviceSource = await readFile(
  new URL("../../../svg/renderer/geode/GeodeDevice.cc", import.meta.url),
  "utf8",
);
const browserRootSource = await readFile(
  new URL("../../../svg/renderer/geode/GeodeBrowserRoot.cc", import.meta.url),
  "utf8",
);
const browserDeviceSource = await readFile(
  new URL("../../../gpu/browser/BrowserDevice.cc", import.meta.url),
  "utf8",
);
const browserBridgeSource = await readFile(
  new URL("../../../gpu/browser/EmscriptenBrowserBridge.cc", import.meta.url),
  "utf8",
);
const browserLibrarySource = await readFile(
  new URL("../../../gpu/browser/library_donner_gpu.js", import.meta.url),
  "utf8",
);

function extractAsyncFunction(sourceText, functionName) {
  const signature = `async function ${functionName}(`;
  const start = sourceText.indexOf(signature);
  assert.ok(start >= 0, `expected the ${functionName} helper`);
  const nextFunction = sourceText.slice(start + signature.length).search(
    /\nasync function [A-Za-z_$][\w$]*\s*\(/,
  );
  const end = nextFunction < 0
    ? sourceText.length
    : start + signature.length + nextFunction;
  return sourceText.slice(start, end);
}

function extractBrowserLibraryFunction(name, nextName, bridge) {
  const start = browserLibrarySource.indexOf(`${name}: function(`);
  const end = browserLibrarySource.indexOf("\n  },", start);
  const next = browserLibrarySource.indexOf(`\n  ${nextName}__deps:`, end);
  assert.ok(
    start >= 0 && end > start && next > end,
    `expected the live ${name} browser bridge before ${nextName}`,
  );
  const expression = browserLibrarySource.slice(start, end + 4)
    .replace(`${name}: `, "")
    .trim();
  return new Function("DonnerGpu", "GPUMapMode", `return (${expression});`)(
    bridge,
    { READ: 1 },
  );
}

test("late browser map rejection cannot change a reused mapping ID", () => {
  const pending = [];
  const buffer = {
    mapAsync() {
      const callback = {};
      pending.push(callback);
      return {
        then(resolve) {
          callback.resolve = resolve;
          return {
            catch(reject) {
              callback.reject = reject;
            },
          };
        },
      };
    },
    getMappedRange() {
      return new Uint8Array([1, 2, 3, 4]).buffer;
    },
    unmap() {},
  };
  const record = { objects: new Map(), mappings: new Map() };
  const bridge = {
    kSuccess: 0,
    kFailed: 1,
    kBuffer: 2,
    kBufferMapping: 3,
    kMapPending: 4,
    kMapReady: 5,
    kMapFailed: 6,
    kMapDeviceLost: 7,
    lost: false,
    logicalFor: () => record,
    lookup: () => buffer,
    guard: () => 0,
    guardRelease: () => 0,
    refusalFor: (_, __, id) => record.objects.has(id) ? 0 : 1,
    register: (_, __, id, mapping) => {
      if (record.objects.has(id)) return 1;
      record.objects.set(id, mapping);
      return 0;
    },
  };
  const mapAsync = extractBrowserLibraryFunction(
    "donner_gpu_map_buffer_async",
    "donner_gpu_mapping_state",
    bridge,
  );
  const unmap = extractBrowserLibraryFunction(
    "donner_gpu_unmap_buffer",
    "donner_gpu_create_surface",
    bridge,
  );

  assert.equal(mapAsync(1, 7, 5, 0, 4), 0, "first map must start");
  const oldMapping = record.mappings.get(7);
  assert.ok(oldMapping, "first map must register its own state");
  assert.equal(unmap(1, 7), 0, "first map must release its ID");
  assert.equal(mapAsync(1, 7, 5, 0, 4), 0, "reused ID must start a new map");
  const currentMapping = record.mappings.get(7);
  assert.notEqual(currentMapping, oldMapping, "reused ID must own a new mapping record");

  pending[0].reject(new Error("old map rejected late"));
  assert.equal(
    record.mappings.get(7),
    currentMapping,
    "old rejection must not remove the new mapping",
  );
  assert.equal(
    currentMapping.state,
    bridge.kMapPending,
    "old rejection must not settle the new mapping",
  );
  pending[1].resolve();
  assert.equal(
    currentMapping.state,
    bridge.kMapReady,
    "new map must still complete after the old rejection",
  );
});

test("diagnostic readback requests remain pending until a capture completes", () => {
  const peek = source.match(
    /EM_JS\(int, PeekWgpuReadbackRequest, \(\), \{([\s\S]*?)\n\}\);/,
  );
  assert.ok(peek, "expected a non-consuming diagnostic request probe");
  assert.match(peek[1], /__donnerWgpuReadbackCompleted/);
  assert.doesNotMatch(peek[1], /__donnerWgpuReadbackCompleted'\]\s*=/);

  assert.doesNotMatch(source, /ConsumeWgpuReadbackRequest/);
  assert.match(source, /__donnerWgpuReadbackCaptureStarts/);
  assert.match(source, /__donnerWgpuReadbackCaptureCompletions/);
  assert.match(
    source,
    /PublishWgpuReadbackStats[\s\S]*__donnerWgpuReadbackCompleted'\]\s*=\s*Math\.max/,
  );
});

test("diagnostic readback requests wake the event-driven main loop", () => {
  const requestHook = source.match(
    /window\['__donnerRequestWgpuReadback'\]\s*=\s*function\(\)\s*\{([\s\S]*?)\n\s*\};/,
  );
  assert.ok(requestHook, "expected the explicit diagnostic request hook");
  assert.match(requestHook[1], /__donnerEditorFrameRequested'\]\s*=\s*true/);

  const pendingWake = source.match(
    /EM_JS\(void, WakeWasmEditorForPendingWgpuReadback, \(\), \{([\s\S]*?)\n\}\);/,
  );
  assert.ok(pendingWake, "expected a completion-boundary pending-request wake");
  assert.match(pendingWake[1], /request\s*>\s*completed/);
  assert.match(pendingWake[1], /__donnerEditorFrameRequested'\]\s*=\s*true/);
  assert.match(
    source,
    /inFlight->store\(false,[\s\S]*WakeWasmEditorForPendingWgpuReadback\(\)/,
  );
  assert.match(
    source,
    /struct AsyncSmokeReadbackSetupAttempt[\s\S]*WakeWasmEditorForPendingWgpuReadback\(\)/,
  );

  const runtimeReadback = source.match(
    /void CompleteAsyncRuntimeSmokeReadback\(void\* userdata\) \{([\s\S]*?)\n}\n\nbool StartAsyncRuntimeSmokeReadback/,
  );
  assert.ok(runtimeReadback, "expected the browser-runtime diagnostic completion task");
  assert.match(
    runtimeReadback[1],
    /device\.waitForMapping\(/,
    "the deferred task must await its own runtime mapping",
  );
  assert.match(
    runtimeReadback[1],
    /device\.mappedBytes\(/,
    "a ready mapping must publish the captured bytes",
  );
  assert.match(
    runtimeReadback[1],
    /device\.unmapBuffer\(/,
    "the completion task must release the mapping on every outcome",
  );
  const releaseInFlight = runtimeReadback[1].indexOf("state->inFlight->store(false");
  const recheckPending = runtimeReadback[1].indexOf(
    "ShouldRecheckPendingWgpuReadbackRequestsAfterCompletion",
  );
  const wakePending = runtimeReadback[1].indexOf("WakeWasmEditorForPendingWgpuReadback()");
  assert.ok(
    releaseInFlight >= 0 && releaseInFlight < recheckPending
      && recheckPending < wakePending,
    "every runtime-map completion must release the in-flight gate before waking pending requests",
  );
});

test("failed diagnostic maps terminate after bounded retries", () => {
  assert.match(source, /__donnerWgpuReadbackCaptureFailures/);
  assert.match(
    source,
    /PublishWgpuReadbackFailure[\s\S]*__donnerWgpuReadbackCompleted'\]\s*=\s*Math\.max/,
  );
  assert.match(
    source,
    /WgpuDiagnosticReadbackDecisionFor[\s\S]*PublishWgpuReadbackFailure/,
  );
});

test("pre-map diagnostic setup failures use the same bounded completion policy", () => {
  const attemptCompletion = source.match(
    /CompleteWgpuDiagnosticReadbackAttempt\([\s\S]*?\n}/,
  );
  assert.ok(attemptCompletion, "expected one shared diagnostic-attempt completion path");
  assert.match(attemptCompletion[0], /fetch_add/);
  assert.match(attemptCompletion[0], /WgpuDiagnosticReadbackDecisionFor/);
  assert.match(attemptCompletion[0], /PublishWgpuReadbackFailure/);

  const setupGuard = source.match(
    /struct AsyncSmokeReadbackSetupAttempt[\s\S]*?\n\s*};/,
  );
  assert.ok(setupGuard, "expected a pre-map setup-attempt guard");
  assert.match(
    setupGuard[0],
    /CompleteWgpuDiagnosticReadbackAttempt\(\s*false/,
    "an abandoned setup attempt must count toward the bounded failure policy",
  );
  assert.match(setupGuard[0], /WakeWasmEditorForPendingWgpuReadback/);
  assert.doesNotMatch(source, /struct AsyncSmokeReadbackRetry/);
});

test("worker stats carry the GPU wait outcome that ended the frame", () => {
  const publisher = renderCoordinatorSource.match(
    /void PublishWorkerTimingStats\([\s\S]*?\n\}/,
  );
  assert.ok(publisher, "expected the completed-frame worker stats publisher");
  assert.match(publisher[0], /stats\['deviceLost'\]/);
  assert.match(publisher[0], /stats\['gpuWaitTimeoutSite'\] = UTF8ToString/);
  assert.match(publisher[0], /stats\['gpuWaitTimeoutMs'\]/);
  assert.match(publisher[0], /stats\['publishReason'\] = 'render-result'/);
  // When the worker rendered nothing, the poll keeps presenting the previous frame; the page must
  // still be able to tell that iteration from one that presented.
  assert.match(publisher[0], /stats\['nothingToPresent'\] = heap\[b \+ 32\] > 0/);
  assert.match(publisher[0], /stats\['nothingToPresentTotal'\] =/);

  // The site names are what a failing run prints, so they are part of the
  // contract rather than an implementation detail.
  assert.match(renderCoordinatorSource, /GpuWaitTimeoutSite::None: return "none"/);
  assert.match(renderCoordinatorSource, /GpuWaitTimeoutSite::ReadbackMap: return "readback-map"/);
  assert.match(renderCoordinatorSource, /GpuWaitTimeoutSite::QueueIdle: return "queue-idle"/);

  // clang-format splits a bare `!==` into `!= =`; the publishers must not
  // depend on an operator that a formatting pass can silently corrupt.
  assert.doesNotMatch(renderCoordinatorSource, /!\s*=\s+=/);
});

test("formatted worker diagnostics remain valid JavaScript", () => {
  const bodies = [...renderCoordinatorSource.matchAll(
    /MAIN_THREAD_ASYNC_EM_ASM\(\s*\{([\s\S]*?)\n\s*\},/g,
  )];
  assert.ok(bodies.length >= 3, "expected the completion, acceptance, and GPU-failure publishers");
  for (const [, body] of bodies) {
    assert.doesNotThrow(
      () => new Function(body),
      "formatted EM_ASM must still parse as JavaScript",
    );
  }
});

test("worker acceptance requires the fresh render identity and no old presentation timestamp", () => {
  const timingPublisher = renderCoordinatorSource.match(
    /void PublishWorkerTimingStats\([\s\S]*?\n\}/,
  );
  const acceptedPublisher = renderCoordinatorSource.match(
    /void PublishAcceptedWorkerResult\([\s\S]*?\n\}/,
  );
  assert.ok(timingPublisher);
  assert.ok(acceptedPublisher);
  const body = (method) => method.match(/MAIN_THREAD_ASYNC_EM_ASM\(\s*\{([\s\S]*?)\n\s*\},/)[1];
  const publishTiming = new Function(
    "window",
    "$0",
    "$1",
    "HEAPF64",
    "UTF8ToString",
    "performance",
    body(timingPublisher[0]),
  );
  const publishAccepted = new Function("window", "$0", "$1", "$2", body(acceptedPublisher[0]));
  const heap = new Float64Array(31);
  heap.set([11, 22, 33, 44, 0], 26);
  const window = { __donnerWorkerStats: { completedResults: 8, presentedAtMs: 100 } };
  publishTiming(window, 0, 0, heap, () => "none", { now: () => 200 });
  const stats = window.__donnerWorkerStats;
  assert.equal(stats.completedResults, 9);
  assert.equal(stats.acceptedForPresentation, false);
  assert.equal(stats.presentedAtMs, undefined);
  assert.deepEqual([
    stats.documentGeneration,
    stats.frameVersion,
    stats.fontResourceRevision,
    stats.sourceVersion,
    stats.undoEntryCount,
  ], [11, 22, 33, 44, 0]);
  for (const stale of [[0, 22, 33], [11, 0, 33], [11, 22, 0]]) {
    publishAccepted(window, ...stale);
    assert.equal(stats.acceptedForPresentation, false, `stale result ${stale} must be ignored`);
  }
  publishAccepted(window, 11, 22, 33);
  assert.equal(stats.acceptedForPresentation, true);
  assert.equal(stats.presentedAtMs, undefined, "admission must not manufacture a presentation");
});

test("a GPU wait failure publishes worker stats even though no frame completed", () => {
  const failurePublisher = renderCoordinatorSource.match(
    /void PublishWorkerGpuWaitFailure\([\s\S]*?\n\}/,
  );
  assert.ok(failurePublisher, "expected a publisher for frames that never completed");
  assert.match(
    failurePublisher[0],
    /window\['__donnerWorkerStats'\] = stats/,
    "the failure path must write the same stats object the harness reads",
  );
  assert.match(failurePublisher[0], /stats\['publishReason'\] = 'gpu-wait-failure'/);
  assert.match(
    failurePublisher[0],
    /previous \? Object\.assign\(\{\}, previous\) : \(\{'completedResults' : 0\}\)/,
    "a failure before any frame landed must still create the stats object",
  );
  assert.doesNotMatch(
    failurePublisher[0],
    /'completedResults' : previous/,
    "the completed-frame counter orders presentation samples and must count frames only",
  );
  assert.match(
    failurePublisher[0],
    /delete stats\['presentedAtMs'\]/,
    "a merged publish must not inherit the previous object's presentation stamp; "
      + "the editor stamps it once per fresh stats object and probes pair the two timestamps",
  );

  const poll = renderCoordinatorSource.match(
    /void RenderCoordinator::pollRenderResult\([\s\S]*?\n {2}const auto& result = \*resultOpt;/,
  );
  assert.ok(poll, "expected the UI-thread result poll");
  const failureCall = poll[0].indexOf("PublishWorkerGpuWaitFailure(");
  assert.ok(failureCall >= 0, "the poll must report a GPU wait failure");
  assert.match(
    poll[0],
    /if \(!resultOpt\.has_value\(\)\) \{\s*PublishWorkerGpuWaitFailure\(/,
    "the separate failure publish must apply only when no frame completed",
  );
  const earlyReturn = poll[0].match(
    /if \(!IsCurrentRenderResult\(resultOpt, app\)\) \{\s*rejectPixelCaptureResult\(resultOpt\);\s*return;\s*\}/,
  );
  assert.ok(earlyReturn, "the poll must return when no current frame completed");
  assert.ok(
    failureCall < earlyReturn.index,
    "the failure report must happen before the no-result early return",
  );
  assert.match(poll[0], /gpuWaitFailure\.generation != publishedGpuWaitGeneration_/);
});

test("the render worker records a GPU wait failure on the abandoned-frame path", () => {
  const abandoned = workerRendererSource.match(
    /if \(!renderCompleted\) \{[\s\S]*?cancelledRenderCount_\.fetch_add/,
  );
  assert.ok(abandoned, "expected the worker's abandoned-iteration path");
  assert.match(abandoned[0], /noteGpuWaitOutcome\(requestRenderer\.consumeReadbackStats\(\)\)/);

  assert.match(
    workerRendererHeader,
    /struct GpuWaitFailure \{[\s\S]*?bool deviceLost[\s\S]*?timedOutWaitSite[\s\S]*?timedOutWaitMs[\s\S]*?generation/,
    "the worker's failure record must carry the flag, the wait site, and the elapsed wait",
  );
  assert.match(
    workerRendererHeader,
    /bool deviceLost = false;[\s\S]*?svg::GpuWaitTimeoutSite timedOutWaitSite[\s\S]*?int timedOutWaitMs/,
    "the per-frame timing breakdown must carry the same three fields",
  );
});

test("the presentation regression gates print the worker health they wait on", () => {
  const helper = presentationRegressionSource.match(
    /async function expectWorkerResultsToReach\([\s\S]*?\n\}/,
  );
  assert.ok(helper, "expected one shared render gate for the presentation suite");
  assert.match(helper[0], /readWorkerHealth\(page\)/);
  // Playwright prints the whole received value for `toEqual`, but only the
  // keys it was asked to match for `toMatchObject`. With the latter the health
  // fields never reach the log and the helper is pure overhead, so the matcher
  // is part of the contract rather than a style choice.
  assert.match(helper[0], /\.toEqual\(expect\.objectContaining\(\{ reached: true \}\)\)/);
  assert.doesNotMatch(helper[0], /toMatchObject/);

  const health = presentationRegressionSource.match(
    /async function readWorkerHealth\([\s\S]*?\n\}/,
  );
  assert.ok(health, "expected a worker health snapshot helper");
  for (
    const field of [
      "deviceLost",
      "gpuWaitTimeoutSite",
      "gpuWaitTimeoutMs",
      "publishReason",
      "sampleThumbnail",
      "sampleThumbnailPublishedAtMs",
      "sampleThumbnailPublicationGeneration",
      "frameLoop",
      "interaction",
    ]
  ) {
    assert.match(health[0], new RegExp(`${field}:`), `health snapshot must carry ${field}`);
  }

  // Every render gate in the suite has to go through the helper: one that
  // still polls the bare counter is exactly the gate that reports nothing when
  // the worker stops publishing.
  assert.doesNotMatch(
    presentationRegressionSource,
    /\.poll\((?:async )?\(\) => page\.evaluate\(\(\) => window\.__donnerWorkerStats\?\.completedResults/,
  );
});

test("the composited drag sample gate waits for the document worker, not sidebar thumbnails", () => {
  const helperStart = compositedDragSource.indexOf("async function openDonnerSplash(");
  const helperEnd = compositedDragSource.indexOf("* The Donner_D left stem", helperStart);
  assert.ok(helperStart >= 0 && helperEnd > helperStart, "expected the Splash open helper");
  const helper = compositedDragSource.slice(helperStart, helperEnd);

  assert.match(helper, /__donnerWorkerStats/);
  assert.match(helper, /\.toEqual\(expect\.objectContaining\(\{ reached: true \}\)\)/);
  assert.doesNotMatch(helper, /__donnerLayerThumbnailStats/);
});

test("the composited drag gate does not cancel first-use offscreen WebGPU work", () => {
  const helperStart = compositedDragSource.indexOf("async function openDonnerSplash(");
  const helperEnd = compositedDragSource.indexOf("* The Donner_D left stem", helperStart);
  assert.ok(helperStart >= 0 && helperEnd > helperStart, "expected the Splash open helper");
  const helper = compositedDragSource.slice(helperStart, helperEnd);

  const thumbnailPrecondition = helper.indexOf("__donnerSampleThumbnailStats");
  const sampleClick = helper.indexOf("page.mouse.click");
  assert.ok(
    thumbnailPrecondition >= 0 && thumbnailPrecondition < sampleClick,
    "the Firefox drag gate must let first-use offscreen rendering settle before replacing it",
  );
  assert.match(helper, /completed\s*>\s*0/);
  assert.match(helper, /\(stats\.ready\s*\?\?\s*0\)\s*>\s*0/);
  assert.match(helper, /!stats\.active/);
  assert.match(helper, /!stats\.pending/);
  assert.match(helper.slice(0, sampleClick), /timeout:\s*scaledMs\(20_000\)/);
});

test("the composited viewport sample gate waits for settled thumbnails and the document worker", () => {
  const helper = extractAsyncFunction(compositedViewportSource, "openDonnerSplash");
  const sampleClick = helper.indexOf("page.mouse.click");
  const thumbnailPrecondition = helper.indexOf("__donnerSampleThumbnailStats");
  assert.ok(
    thumbnailPrecondition >= 0 && thumbnailPrecondition < sampleClick,
    "viewport gestures must not replace an active first-use thumbnail render",
  );
  assert.match(helper, /__donnerWorkerStats/);
  assert.match(helper, /acceptedForPresentation === true/);
  assert.match(helper, /typeof worker\.presentedAtMs === "number"/);
  assert.doesNotMatch(helper, /__donnerLayerThumbnailStats/);
});

test("shared Basic Shapes visual gates settle first-use thumbnails before replacement", () => {
  const helper = extractAsyncFunction(presentationRegressionSource, "openBasicShapes");

  const thumbnailPrecondition = helper.indexOf("__donnerSampleThumbnailStats");
  const sampleClick = helper.indexOf("page.mouse.click");
  assert.ok(
    thumbnailPrecondition >= 0 && thumbnailPrecondition < sampleClick,
    "visual-only gates must not replace a first-use thumbnail render before it settles",
  );
  assert.match(helper, /completed\s*>\s*0/);
  assert.match(helper, /\(stats\.ready\s*\?\?\s*0\)\s*>\s*0/);
  assert.match(helper, /!stats\.active/);
  assert.match(helper, /!stats\.pending/);
  assert.match(helper.slice(0, sampleClick), /timeout:\s*scaledMs\(20_000\)/);
});

test("browser GPU startup owns one bounded request and shared logical roots", () => {
  const beginStart = browserLibrarySource.indexOf(
    "donner_gpu_begin_device_request: function(handle) {",
  );
  const beginEnd = browserLibrarySource.indexOf("donner_gpu_release_device__deps:", beginStart);
  assert.ok(
    beginStart >= 0 && beginEnd > beginStart,
    "expected the live logical-device request hook",
  );
  const begin = browserLibrarySource.slice(beginStart, beginEnd);
  assert.match(
    begin,
    /if \(DonnerGpu\.requestStarted\) \{[\s\S]*?return DonnerGpu\.kSuccess;/,
    "later logical devices must join the worker's existing browser device request",
  );
  const requestStart = browserLibrarySource.indexOf("requestBrowserDevice: function() {");
  const installStart = browserLibrarySource.indexOf("installDevice: function(device) {");
  const releaseStart = browserLibrarySource.indexOf("releaseSharedDevice: function() {");
  assert.ok(
    requestStart >= 0 && installStart > requestStart && releaseStart > installStart,
    "expected the live browser request, installation, and release hooks",
  );
  const request = browserLibrarySource.slice(requestStart, installStart);
  const install = browserLibrarySource.slice(installStart, releaseStart);
  const release = browserLibrarySource.slice(
    releaseStart,
    browserLibrarySource.indexOf("releaseProducerHold: function", releaseStart),
  );

  assert.equal(
    [...request.matchAll(/navigator\.gpu\.requestAdapter\(\)/g)].length,
    1,
    "one browser adapter request must serve each device request",
  );
  assert.equal(
    [...request.matchAll(/adapter\.requestDevice\(\)/g)].length,
    1,
    "the browser device must come from the selected adapter exactly once",
  );
  assert.equal(
    [...request.matchAll(/generation === DonnerGpu\.requestGeneration/g)].length,
    2,
    "both late success and late failure must ignore a released request generation",
  );
  assert.match(
    install,
    /device\.onuncapturederror\s*=/,
    "an uncaptured browser validation error needs a diagnostic handler",
  );
  assert.match(
    install,
    /if \(DonnerGpu\.device === device\)/,
    "a late device-loss callback must not mark its replacement lost",
  );
  assert.match(
    release,
    /DonnerGpu\.requestGeneration \+= 1/,
    "releasing the last logical device must invalidate its outstanding request",
  );

  assert.match(
    browserRootSource,
    /BrowserDeviceRequest::Begin\(\s*std::make_unique<gpu::browser::EmscriptenBrowserBridge>\(\)\)/,
    "browser root selection must use the owned runtime bridge",
  );
  assert.match(
    browserRootSource,
    /request\.settle\(kBrowserDeviceSettleSeconds\)/,
    "device acquisition must stop after its bounded wait",
  );
  assert.match(
    browserRootSource,
    /std::move\(request\)\.take\(std::move\(lostState\)\)/,
    "the selected device must retain the root's loss condition",
  );
  assert.match(
    browserRootSource,
    /OpenBrowserDevice\(root->lostState\(\)\)/,
    "each logical context must reopen over the same loss condition",
  );
  assert.doesNotMatch(
    browserRootSource,
    /GeodeWgpuAdapterDevice|webgpu\/webgpu/,
    "the configured browser root must not import the transitional C wrapper",
  );

  const deviceDestructor = geodeDeviceSource.match(
    /GeodeDevice::~GeodeDevice\(\)([\s\S]*?)\n}/,
  );
  assert.ok(deviceDestructor, "expected thread-affined GPU teardown");
  const nativeDrain = deviceDestructor[1].match(/#ifndef __EMSCRIPTEN__([\s\S]*?)#endif/);
  assert.ok(nativeDrain, "expected native-only submitted-work drain");
  assert.match(
    nativeDrain[1],
    /waitForQueueIdle/,
    "native teardown must use the bounded queue-idle wait",
  );
  assert.doesNotMatch(
    geodeDeviceSource,
    /WaitForSubmittedWork/,
    "teardown must not use an unbounded submitted-work wait",
  );
  assert.doesNotMatch(
    browserRootSource,
    /WaitForSubmittedWork/,
    "the browser root must not block teardown on submitted work",
  );
  assert.doesNotMatch(
    deviceDestructor[1],
    /poll\(true/,
    "teardown must never block inside the driver without a deadline",
  );
});

test("browser readback yields bounded event-loop slices for mapping completion", () => {
  const wait = browserDeviceSource.match(
    /MapSliceReport BrowserDevice::onWaitMappingSlice\([\s\S]*?\n}\n\nResult<std::span<const uint8_t>> BrowserDevice::onMappedBytes/,
  );
  assert.ok(wait, "expected the browser runtime's mapping wait");
  assert.match(wait[0], /MapWaitKind::Polled/, "browser mappings settle through promise polling");
  assert.match(
    wait[0],
    /if \(yielding_\)/,
    "nested waits must be refused during an existing stack unwind",
  );
  assert.match(
    wait[0],
    /bridge_->yieldToBrowser\(ClampYieldSeconds\(sliceSeconds, kMaxYieldSeconds\)\)/,
    "each event-loop yield must be bounded by the slice limit",
  );
  const yieldCall = wait[0].indexOf("bridge_->yieldToBrowser(");
  assert.ok(yieldCall >= 0, "expected a bounded browser yield before mapping revalidation");
  assert.match(
    wait[0].slice(yieldCall),
    /objects_\.find\(BrowserObjectKind::BufferMapping, mappingSlotIndex\)/,
    "the mapping ID must be checked again after yielding",
  );
  const yieldHook = browserBridgeSource.match(
    /void EmscriptenBrowserBridge::yieldToBrowser\(double seconds\) \{([\s\S]*?)\n}/,
  );
  assert.ok(yieldHook, "expected the Emscripten event-loop yield hook");
  assert.match(
    yieldHook[1],
    /emscripten_sleep\(static_cast<unsigned int>\(milliseconds\)\)/,
    "the mapping promise needs an event-loop turn to settle",
  );
  assert.match(
    yieldHook[1],
    /kMaxSleepMilliseconds/,
    "the bridge must bound the duration it passes to Emscripten",
  );
  assert.match(
    browserDeviceSource,
    /BrowserDeviceRequest::settle\(double timeoutSeconds\)[\s\S]*?deadline[\s\S]*?ClampYieldSeconds/,
    "startup must also yield under a deadline",
  );
});

test("renderer thread startup waits for cursor setup and wake wiring", () => {
  const constructor = workerRendererSource.match(
    /AsyncRenderer::AsyncRenderer\([^)]*\)([\s\S]*?)\n}\n\nvoid AsyncRenderer::start/,
  );
  assert.ok(constructor, "expected the AsyncRenderer constructor");
  assert.doesNotMatch(
    constructor[1],
    /thread_ = std::thread/,
    "constructing renderer ownership must not race the main-thread WebGPU setup",
  );

  const startup = workerRendererSource.match(
    /void AsyncRenderer::start\(\)([\s\S]*?)\n}\n\nAsyncRenderer::~AsyncRenderer/,
  );
  assert.ok(startup, "expected explicit renderer-worker startup");
  assert.match(startup[1], /thread_ = std::thread/);
  // the single-canvas architecture: one raster std::thread on every platform. The browser build
  // used to create its own pthread with a transferred document canvas and defer
  // that creation until the main-thread WebGPU device existed; there is no
  // worker-owned canvas to transfer any more, so there is no deferred-start
  // opt-in either.
  assert.doesNotMatch(workerRendererSource, /pthread_create/);
  assert.doesNotMatch(renderCoordinatorSource, /AsyncRendererStartMode::Deferred/);

  const shellConstructor = editorShellSource.match(
    /EditorShell::EditorShell\([\s\S]*?\)\s*\n\s*:[\s\S]*?\{([\s\S]*?)\n}/,
  );
  assert.ok(shellConstructor, "expected the EditorShell constructor body");
  const cursorInitialization = shellConstructor[1].indexOf("rotateCursorSet_.initialize");
  const wakeCallback = shellConstructor[1].indexOf("setWakeCallback");
  const workerStartup = shellConstructor[1].indexOf("asyncRenderer().start()");
  assert.ok(cursorInitialization >= 0, "expected main-thread cursor initialization");
  assert.ok(wakeCallback > cursorInitialization, "wake callback should follow UI GPU setup");
  assert.ok(
    workerStartup > wakeCallback,
    "worker must start only after wake callback installation",
  );

  const wasmCursorInitialization = rotateCursorSource.match(
    /#ifdef __EMSCRIPTEN__([\s\S]*?)#else/,
  );
  assert.ok(wasmCursorInitialization, "expected a browser-native Wasm cursor path");
  assert.doesNotMatch(
    wasmCursorInitialization[1],
    /Render(?:Editor|Rotate|Scale|Pan|Pen|Select|Path)/,
  );
  assert.doesNotMatch(wasmCursorInitialization[1], /takeSnapshot|mapAsync/);
  assert.doesNotMatch(rotateCursorSource, /\$\s+\{/, "C++ formatting must not corrupt JavaScript");
  assert.doesNotMatch(
    rotateCursorSource,
    /!\s*=\s+=/,
    "JavaScript strict inequality must stay intact",
  );
  assert.match(rotateCursorSource, /data:image\/svg\+xml;base64,/);
  assert.equal(
    [...rotateCursorSource.matchAll(/const key = cursorId \+ ":" \+ cornerIndex;/g)].length,
    2,
    "registration and application must use the same exact cursor key",
  );
  // The cursor is CSS on the page, which only the browser main thread can
  // write, so the application hook is a main-thread-proxied `EM_ASM` rather than
  // a plain `EM_JS` that would run in whichever thread happened to call it. On
  // the whole-app-worker build that thread is the app pthread, where the write
  // lands on an inert stand-in and never reaches the page.
  const browserCursorApply = rotateCursorSource.match(
    /void ApplyBrowserCursor\(int cursorId, int cornerIndex\) \{[\s\S]*?\n\}/,
  );
  assert.ok(browserCursorApply, "expected a browser cursor application hook");
  assert.match(browserCursorApply[0], /MAIN_THREAD_ASYNC_EM_ASM/);
  assert.match(
    browserCursorApply[0],
    /document\.getElementById\("canvas"\)/,
    "the cursor must be written to the page canvas, not to the worker's stand-in",
  );
  assert.match(browserCursorApply[0], /registry\.active && registry\.activeKey === key/);
  assert.ok(
    browserCursorApply[0].indexOf("registry.activeKey === key")
      < browserCursorApply[0].indexOf("canvas.style.setProperty(\"cursor\""),
    "same-key cursor requests must return before mutating the canvas style",
  );
  assert.match(browserCursorApply[0], /diagnostics\.redundantApplySkips \+= 1/);
  assert.match(
    rotateCursorSource,
    /const svgCssValue =[^;]*hotspotX \+ " " \+ hotspotY \+ ", " \+ fallback;/,
    "the browser cursor CSS must include its explicit hotspot and semantic fallback",
  );
  assert.match(
    rotateCursorSource,
    /RegisterBrowserCursor\([\s\S]*hotspot\.x, hotspot\.y,[\s\S]*fallback\.data\(\),\s*static_cast<int>\(fallback\.size\(\)\)/,
    "every registered SVG cursor must pass its hotspot and fallback into JavaScript",
  );
});
