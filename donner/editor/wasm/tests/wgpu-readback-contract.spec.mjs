import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";
import {
  sampleThumbnailLaneQuiescent,
  sampleThumbnailLaneSerial,
} from "./sample-thumbnail-quiescence.mjs";

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
const workerBridgeSource = await readFile(
  new URL("../../WholeAppWorkerBridge.cc", import.meta.url),
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
const surfaceRecoverySource = await readFile(
  new URL("./browser-surface-recovery.spec.ts", import.meta.url),
  "utf8",
);
const gpuResidencySource = await readFile(
  new URL("./browser-gpu-residency.ts", import.meta.url),
  "utf8",
);
const sampleThumbnailSettleSource = await readFile(
  new URL("./sample-thumbnail-settle.ts", import.meta.url),
  "utf8",
);
const smokeSource = await readFile(new URL("./smoke.spec.ts", import.meta.url), "utf8");
const backendSelectionSource = await readFile(
  new URL("./browser-backend-selection.spec.ts", import.meta.url),
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

function productionPredicates(sourceText, prefix) {
  const predicates = {};
  const pattern = /EM_JS\(int, ([A-Za-z_][\w]*),\s*\(([\s\S]*?)\), \{([\s\S]*?)\n {6}\}\);/g;
  for (const [, name, signature, body] of sourceText.matchAll(pattern)) {
    if (!name.startsWith(prefix)) continue;
    const parameters = signature.split(",").map((parameter) => parameter.trim().match(/[A-Za-z_]\w*$/)[0]);
    predicates[name] = new Function(...parameters, body);
  }
  assert.ok(Object.keys(predicates).length > 0, `expected production ${prefix} predicates`);
  return predicates;
}

const workerTimingPredicates = productionPredicates(renderCoordinatorSource, "WorkerResult");
const workerFramePredicates = productionPredicates(workerBridgeSource, "WorkerFrame");

function makeProductionPublisher(predicates, ...signatureAndBody) {
  const body = signatureAndBody.pop();
  return new Function(
    ...Object.keys(predicates),
    `return function(${signatureAndBody.join(",")}) {${body}\n};`,
  )(...Object.values(predicates));
}

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

function extractTest(sourceText, title) {
  const start = sourceText.indexOf(`\ntest("${title}"`);
  assert.ok(start >= 0, `expected the test "${title}"`);
  const next = sourceText.indexOf("\ntest(", start + 1);
  return sourceText.slice(start, next < 0 ? sourceText.length : next);
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

const frameAttemptFixture = {
  scheduledAttempts: 11,
  eventAttempts: 12,
  scheduledIdle: 13,
  eventIdle: 14,
  scheduledRendered: 15,
  eventRendered: 16,
  blockedByFrame: 17,
  blockedByGpu: 18,
  eventDeferrals: 19,
};

function makeFrameSamplePublisher(body) {
  const publish = new Function(
    "window",
    "performance",
    ...Array.from({ length: 15 }, (_, index) => `$${index}`),
    body,
  );
  return (
    window,
    clock,
    triggerBits,
    frameMs,
    callbacks,
    heapBytes,
    completedAtEpochMs,
    eventFrame = 1,
  ) =>
    publish(
      window,
      clock,
      triggerBits,
      frameMs,
      callbacks,
      heapBytes,
      completedAtEpochMs,
      ...Object.values(frameAttemptFixture),
      eventFrame,
    );
}

test("host timing consumes each current-frame draw snapshot once", () => {
  const underlayCallback = editorShellSource.match(
    /lastDirectPresentationCost_ = DrawDocumentPresentationToFramebuffer\([\s\S]*?PublishPendingUnderlayDrawStats\([\s\S]*?\);/,
  );
  assert.ok(underlayCallback, "expected direct presentation to publish its current draw counts");
  assert.match(underlayCallback[0], /activeTileDrawCount/);
  assert.match(source, /PublishImGuiDrawStats\(0, 0, 0\)/);

  const hostTiming = workerBridgeSource.match(
    /void PublishHostFrameTiming\([\s\S]*?\n}\n\nvoid PublishPinchZoomPolicy/,
  );
  assert.ok(hostTiming, "expected the browser host-frame timing publisher");
  const callback = hostTiming[0].match(
    /MAIN_THREAD_ASYNC_EM_ASM\(\s*\{([\s\S]*?)\n\s*\},\s*endFrameMs/,
  );
  assert.ok(callback, "expected executable host timing callback");
  const bindings = Array.from({ length: 10 }, (_, index) => `const $${index} = args[${index}];`)
    .join("\n");
  const publish = new Function("window", "args", `${bindings}\n${callback[1]}`);
  const windowState = {
    __donnerPendingUnderlayDrawStats: {
      sequence: 7,
      checkerboardDraws: 1,
      overviewTileDraws: 2,
      activeTileDraws: 3,
      directTotalMs: 4,
    },
    __donnerPendingImGuiDrawStats: { vertices: 12, indices: 18, commandLists: 2 },
    __donnerImGuiDrawStats: { vertices: 99, indices: 99, commandLists: 99 },
  };
  publish(windowState, [0, 0, 0, 0, 0, 0, 0, 0, 1, 1]);
  assert.deepEqual({
    frame: windowState.__donnerHostFrameTiming.frames,
    underlayFrame: windowState.__donnerHostFrameTiming.lastUnderlayHostFrame,
    underlaySequence: windowState.__donnerHostFrameTiming.lastUnderlaySequence,
    tileDraws: windowState.__donnerHostFrameTiming.lastActiveTileDraws,
    imguiVertices: windowState.__donnerHostFrameTiming.lastImguiVertices,
  }, { frame: 1, underlayFrame: 1, underlaySequence: 7, tileDraws: 3, imguiVertices: 12 });
  assert.equal(windowState.__donnerPendingUnderlayDrawStats, undefined);
  assert.equal(windowState.__donnerPendingImGuiDrawStats, undefined);
  assert.equal(windowState.__donnerImGuiDrawStats.vertices, 99);

  publish(windowState, [0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
  assert.deepEqual({
    underlayFrame: windowState.__donnerHostFrameTiming.lastUnderlayHostFrame,
    tileDraws: windowState.__donnerHostFrameTiming.lastActiveTileDraws,
    imguiVertices: windowState.__donnerHostFrameTiming.lastImguiVertices,
  }, { underlayFrame: 0, tileDraws: 0, imguiVertices: 0 });

  windowState.__donnerPendingImGuiDrawStats = { vertices: 8, indices: 9, commandLists: 1 };
  publish(windowState, [0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
  assert.equal(windowState.__donnerHostFrameTiming.lastUnderlayHostFrame, 0);
  assert.equal(windowState.__donnerHostFrameTiming.lastImguiVertices, 8);
  assert.equal(windowState.__donnerHostFrameTiming.lastSurfaceAcquired, false);
  assert.equal(windowState.__donnerHostFrameTiming.lastSurfacePresented, false);
  assert.equal(windowState.__donnerPendingImGuiDrawStats, undefined);
  assert.match(presentationRegressionSource, /underlayHostFrame/);
  assert.match(presentationRegressionSource, /activeTileDraws/);
});

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
    trace: () => {},
    epochMs: () => 0,
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

test("mid-drag GPU probe follows the retained scored frame and state", () => {
  const start = presentationRegressionSource.indexOf("async function readBasicShapesDragFrame(");
  const end = presentationRegressionSource.indexOf(
    "test(\"scored Firefox frame evidence",
    start,
  );
  assert.ok(start >= 0 && end > start, "expected the scored mid-drag frame helper");
  const dragFrame = presentationRegressionSource.slice(start, end);
  const steps = [
    "attachScoredMissingBluePng(",
    "attachMissingDragBlueState(",
    "readScoredCanvasProbe(",
    "attachMissingDragBlueCanvasEvidence(",
    "diagnosePostFailureGpuReadback(page, true)",
    "expect(geometry?.blue",
  ];
  for (let index = 1; index < steps.length; ++index) {
    assert.ok(
      dragFrame.indexOf(steps[index - 1]) >= 0
        && dragFrame.indexOf(steps[index - 1]) < dragFrame.indexOf(steps[index]),
      "mid-drag GPU request must follow scored screenshot, state and direct-canvas evidence",
    );
  }
  const failingBranch = dragFrame.indexOf("if (geometry.blue === null) {");
  const rawRequest = dragFrame.indexOf("diagnosePostFailureGpuReadback(page, true)");
  assert.ok(failingBranch >= 0 && failingBranch < rawRequest);
  assert.equal(dragFrame.split("diagnosePostFailureGpuReadback(page, true)").length - 1, 1);
  assert.ok(presentationRegressionSource.includes(
    "a passing drag must not request diagnostic GPU readback",
  ));
});

test("failure-only browser readback installs with no seeded request", () => {
  assert.match(workerBridgeSource, /readbackParams\.has\('wgpuReadbackOnFailure'\)/);
  assert.match(workerBridgeSource, /initialReadbackRequest = seededReadback \? 1 : 0/);
  assert.match(workerBridgeSource, /__donnerWgpuReadbackRequested'\] = initialReadbackRequest/);
  assert.match(workerBridgeSource, /HEAP32\[i32 \+ 9\] = initialReadbackRequest/);
  assert.match(presentationRegressionSource, /wgpuReadbackOnFailure/);
  assert.match(presentationRegressionSource, /requested: 0,[\s\S]*starts: 0/);
});

test("diagnostic readback requests remain pending until a capture completes", () => {
  assert.match(
    source,
    /int PeekWgpuReadbackRequest\(\) \{\s*return whole_app_worker::PeekReadbackRequest\(\);/,
  );
  const peek = workerBridgeSource.match(/int PeekReadbackRequest\(\) \{([\s\S]*?)\n\}/);
  assert.ok(peek, "expected a non-consuming diagnostic request probe");
  assert.match(peek[1], /readbackCompletedId/);
  assert.doesNotMatch(peek[1], /StoreRelaxed|\.store\(|exchange\(/);

  assert.doesNotMatch(source, /ConsumeWgpuReadbackRequest/);
  assert.match(workerBridgeSource, /__donnerWgpuReadbackCaptureStarts/);
  assert.match(workerBridgeSource, /__donnerWgpuReadbackCaptureCompletions/);
  assert.match(
    workerBridgeSource,
    /PublishReadbackStats[\s\S]*__donnerWgpuReadbackCompleted'\]\s*=\s*Math\.max/,
  );
});

test("diagnostic readback requests wake the event-driven main loop", () => {
  const requestHook = workerBridgeSource.match(
    /window\['__donnerRequestWgpuReadback'\]\s*=\s*function\(\)\s*\{([\s\S]*?)\n\s*\};/,
  );
  assert.ok(requestHook, "expected the explicit diagnostic request hook");
  // The app thread's frame loop consumes the shared-memory frame request, not the page flag.
  assert.match(requestHook[1], /HEAP32\[i32 \+ 2\] = 1/);
  assert.match(requestHook[1], /__donnerEditorFrameRequested'\]\s*=\s*true/);

  assert.match(
    source,
    /void WakeWasmEditorForPendingWgpuReadback\(\) \{\s*whole_app_worker::WakeForPendingReadback\(\);/,
  );
  const pendingWake = workerBridgeSource.match(
    /void WakeForPendingReadback\(\) \{([\s\S]*?)\n\}/,
  );
  assert.ok(pendingWake, "expected a completion-boundary pending-request wake");
  assert.match(
    pendingWake[1],
    /LoadRelaxed\(mirror\.readbackRequestId\)\s*>\s*LoadRelaxed\(mirror\.readbackCompletedId\)/,
  );
  assert.match(pendingWake[1], /StoreRelaxed\(mirror\.frameRequested, 1\)/);
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
    /device\.pollMapping\(/,
    "the deferred task must poll its own mapping without another Asyncify wait",
  );
  assert.doesNotMatch(runtimeReadback[1], /device\.waitForMapping\(/);
  assert.match(runtimeReadback[1], /state->deadline/);
  assert.match(
    runtimeReadback[1],
    /emscripten_async_call\(CompleteAsyncRuntimeSmokeReadback, state\.release\(\)/,
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
  assert.match(workerBridgeSource, /__donnerWgpuReadbackCaptureFailures/);
  assert.match(
    workerBridgeSource,
    /PublishReadbackFailure[\s\S]*__donnerWgpuReadbackCompleted'\]\s*=\s*Math\.max/,
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

test("worker poll timestamps retain their producer clock across delayed proxy delivery", () => {
  const publisher = renderCoordinatorSource.match(
    /void PublishWorkerTimingStats\([\s\S]*?\n\}/,
  )[0];
  const body = publisher.match(/MAIN_THREAD_ASYNC_EM_ASM\(\s*\{([\s\S]*?)\n\s*\},/)[1];
  const publish = makeProductionPublisher(workerTimingPredicates,
    "window",
    "$0",
    "$1",
    "$2",
    "HEAPF64",
    "_free",
    "UTF8ToString",
    "performance", "$3 = false", "$4 = 0", "$5 = 0", "$6 = 0",
    body,
  );
  const count = Number(/kValueCount = (\d+)/.exec(publisher)[1]);
  const pollClock = workerRendererSource.match(
    /polledAtEpochMs =\s*EM_ASM_DOUBLE\(\{([\s\S]*?)\}\)/,
  );
  assert.ok(pollClock, "the poll must capture its clock before asynchronous diagnostic delivery");
  const epochNow = new Function("performance", pollClock[1]);
  const polledAtEpochMs = epochNow({ timeOrigin: 700, now: () => 500 });
  for (const arrivalMs of [900, 200]) {
    const heap = new Float64Array(Math.max(count, 41));
    heap[40] = polledAtEpochMs;
    const window = {};
    publish(window, 0, 0, count, heap, () => {}, () => "none", {
      timeOrigin: 1000,
      now: () => arrivalMs,
    });
    assert.equal(
      window.__donnerWorkerStats.publishedAtMs,
      200,
      "the UI poll timestamp must exclude its asynchronous publication delay",
    );
    assert.equal(window.__donnerWorkerStats.publicationDelayMs, arrivalMs - 200);
  }
});

test("frame completion timestamps exclude proxy delay and retain successful admission", () => {
  const publisher = workerBridgeSource.match(/void RecordFrameSample\([\s\S]*?\n\}/)[0];
  const body = publisher.match(/MAIN_THREAD_ASYNC_EM_ASM\(\s*\{([\s\S]*?)\n\s*\},/)[1];
  const publish = makeFrameSamplePublisher(body);
  const frameClock = workerBridgeSource.match(
    /EM_JS\(double, WorkerEpochNowMs, \(\), \{([\s\S]*?)\}\);/,
  );
  assert.ok(frameClock, "the app worker must expose an epoch clock comparable with the page");
  const epochNow = new Function("performance", frameClock[1]);
  const completedAtEpochMs = epochNow({ timeOrigin: 700, now: () => 515 });
  assert.match(publisher, /const double completedAtEpochMs = WorkerEpochNowMs\(\)/);
  for (const arrivalMs of [1000, 215]) {
    const window = {
      __donnerFrameLoopStats: { uiFrameMsSamples: [] },
      __donnerWorkerStats: { acceptedForPresentation: false, publishReason: "render-result" },
      __donnerHostFrameTiming: { lastSurfacePresented: true },
    };
    const clock = { timeOrigin: 1000, now: () => arrivalMs };
    publish(window, clock, 1, 15, 1, 4096, completedAtEpochMs);
    assert.deepEqual(window.__donnerFrameAdmissionStats, frameAttemptFixture);
    assert.equal(
      window.__donnerWorkerStats.presentedAtMs,
      undefined,
      "an unaccepted result cannot acquire a presentation timestamp",
    );
    window.__donnerWorkerStats.acceptedForPresentation = true;
    window.__donnerHostFrameTiming.lastSurfacePresented = false;
    publish(window, clock, 1, 15, 1, 4096, completedAtEpochMs);
    assert.equal(
      window.__donnerWorkerStats.presentedAtMs,
      undefined,
      "a refused surface cannot acquire a presentation timestamp",
    );
    window.__donnerHostFrameTiming.lastSurfacePresented = true;
    const eventFrame = arrivalMs === 1000 ? 1 : 0;
    publish(window, clock, 1, 15, 1, 4096, completedAtEpochMs, eventFrame);
    assert.equal(window.__donnerFrameLoopStats.lastFrameAtMs, 215);
    assert.equal(
      window.__donnerWorkerStats.presentedAtMs,
      215,
      "a frame timestamp must reflect completion, not its diagnostic arrival",
    );
    assert.equal(window.__donnerWorkerStats.presentationPublicationDelayMs, arrivalMs - 215);
    const presentingFrame = eventFrame ? "event" : "scheduled";
    assert.equal(
      window.__donnerWorkerStats.presentingFrame,
      presentingFrame,
      "the presenting frame's kind is stamped with its presentation timestamp",
    );
    publish(window, clock, 1, 25, 1, 4096, completedAtEpochMs + 15, 1 - eventFrame);
    assert.equal(
      window.__donnerWorkerStats.presentedAtMs,
      215,
      "later frames cannot replace the consumed result's first presentation timestamp",
    );
    assert.equal(
      window.__donnerWorkerStats.presentingFrame,
      presentingFrame,
      "later frames cannot replace the presenting frame's kind",
    );
  }
});

test("the first canvas frame after a polled result must carry that result", () => {
  const publisher = workerBridgeSource.match(/void PublishPresentedDocumentFrame\([\s\S]*?\n\}/);
  assert.ok(publisher, "expected the presented-document-frame publisher");
  const body = publisher[0].match(/MAIN_THREAD_ASYNC_EM_ASM\(\s*\{([\s\S]*?)\n\s*\},/)[1];
  const statsHelper = workerBridgeSource.match(
    /EM_JS\(void, RecordPresentedDocumentFrameStats,[\s\S]*?int surfacePresented\), \{([\s\S]*?)\n {6}\}\);/,
  );
  assert.ok(statsHelper, "expected the production presentation stats helper");
  const recordStats = new Function(
    "window",
    "performance",
    "documentGeneration",
    "frameVersion",
    "presentedAtEpochMs",
    "surfacePresented",
    statsHelper[1],
  );
  const publish = makeProductionPublisher(workerFramePredicates,
    "window",
    "performance",
    "RecordPresentedDocumentFrameStats",
    "$0",
    "$1",
    "$2",
    "$3", "$4", "$5", "$6", "$7",
    body,
  );
  const frameClock = { timeOrigin: 1000, now: () => 250 };
  const presentFrame = (window, generation, version, epochMs = 1200, surfacePresented = true,
                        sequence = 2, producer = 1, capture = 22, currentProducer = true) =>
    publish(
      window,
      frameClock,
      (...values) => recordStats(window, frameClock, ...values),
      generation,
      version,
      epochMs,
      Number(surfacePresented), sequence, producer, capture, currentProducer,
    );
  const unpresented = {};
  presentFrame(unpresented, 2, 5, 1200, false);
  assert.equal(unpresented.__donnerPresentedDocumentStats, undefined);
  presentFrame(unpresented, 2, 5, 900, true);
  assert.equal(unpresented.__donnerPresentedDocumentStats, undefined);
  const window = {
    __donnerWorkerStats: { publishReason: "render-result", documentGeneration: 2, frameVersion: 5,
      captureId: 22, presentationBoundaryKnown: true, presentationAdmissionSequence: 1,
      presentationProducerGeneration: 1 },
  };
  const stats = window.__donnerWorkerStats;
  presentFrame(window, 2, 4);
  presentFrame(window, 1, 9);
  assert.deepEqual(
    { presented: stats.resultFramePresented, without: stats.framesPresentedWithoutResult },
    { presented: undefined, without: 2 },
    "frames that reach the canvas with another document frame are counted against the result",
  );
  presentFrame(window, 2, 5);
  assert.equal(window.__donnerPresentedDocumentStats.lastPresentedAtMs, 200);
  assert.equal(window.__donnerPresentedDocumentStats.lastPublicationDelayMs, 50);
  const presentedCount = window.__donnerPresentedDocumentStats.count;
  presentFrame(window, 2, 5);
  assert.equal(window.__donnerPresentedDocumentStats.count, presentedCount);
  presentFrame(window, 2, 4);
  assert.deepEqual(
    { presented: stats.resultFramePresented, without: stats.framesPresentedWithoutResult },
    { presented: true, without: 2 },
    "the count stops at the first frame that carries the result",
  );

  const prompt = { publishReason: "render-result", documentGeneration: 3, frameVersion: 1,
    captureId: 22, presentationBoundaryKnown: true, presentationAdmissionSequence: 1,
    presentationProducerGeneration: 1 };
  window.__donnerWorkerStats = prompt;
  presentFrame(window, 3, 1);
  assert.deepEqual(
    { presented: prompt.resultFramePresented, without: prompt.framesPresentedWithoutResult },
    { presented: true, without: 0 },
    "a result carried by the first presented frame records zero frames without it",
  );

  const backlog = { publishReason: "render-result", documentGeneration: 2, frameVersion: 5,
    captureId: 22, presentationBoundaryKnown: true, presentationAdmissionSequence: 10,
    presentationProducerGeneration: 2 };
  window.__donnerWorkerStats = backlog;
  presentFrame(window, 2, 5, 1200, true, 10, 2, 11);
  assert.deepEqual({ total: backlog.framesPresentedWithoutResult,
    backlog: backlog.preexistingFramesCompleted, eligible: backlog.postPollFramesCompletedWithoutResult,
    presented: backlog.resultFramePresented },
    { total: 1, backlog: 1, eligible: undefined, presented: undefined });
  presentFrame(window, 2, 5, 1210, true, 11, 2, 22);
  assert.deepEqual({ total: backlog.framesPresentedWithoutResult,
    backlog: backlog.preexistingFramesCompleted, eligible: backlog.postPollFramesCompletedWithoutResult,
    presented: backlog.resultFramePresented },
    { total: 1, backlog: 1, eligible: 0, presented: true },
    "retained older backlog is distinct from a correct first post-poll admission");

  assert.equal(backlog.resultFrameCompletedAtMs, 210,
    "same-version capture B publishes its own completion time independently of cadence A");
  assert.equal(window.__donnerPresentedDocumentStats.lastPresentedAtMs, 200,
    "version cadence remains deduplicated while capture completion advances");

  const reused = { ...backlog, resultFramePresented: undefined, framesPresentedWithoutResult: 0,
    preexistingFramesCompleted: 0, postPollFramesCompletedWithoutResult: 0 };
  window.__donnerWorkerStats = reused;
  presentFrame(window, 2, 5, 1200, true, 10, 1, 22, false);
  assert.equal(reused.resultFramePresented, undefined, "an old producer's token collision is backlog");
  assert.equal(reused.framesPresentedWithoutResult, 1);
  presentFrame(window, 2, 5, 1210, true, 11, 2, 11);
  assert.equal(reused.resultFramePresented, undefined, "equal versions cannot hide a stale capture");
  assert.equal(reused.postPollFramesCompletedWithoutResult, 1);
  presentFrame(window, 2, 5, 1220, true, 12, 2, 22);
  assert.equal(reused.resultFramePresented, true);
  assert.equal(reused.framesPresentedWithoutResult, 2);

  for (const invalidAt of [NaN, Infinity, 900]) {
    const invalid = { publishReason: "render-result", documentGeneration: 2, frameVersion: 5,
      captureId: 22, presentationBoundaryKnown: true, presentationAdmissionSequence: 1,
      presentationProducerGeneration: 1 };
    window.__donnerWorkerStats = invalid;
    presentFrame(window, 2, 5, invalidAt);
    assert.equal(invalid.resultFramePresented, undefined,
      "invalid or negative completion times cannot qualify a capture");
    assert.equal(invalid.resultFrameCompletedAtMs, undefined);
  }

  const detached = { publishReason: "render-result", documentGeneration: 2, frameVersion: 5,
    captureId: 22, presentationBoundaryKnown: true, presentationAdmissionSequence: 1,
    presentationProducerGeneration: 1 };
  window.__donnerWorkerStats = detached;
  presentFrame(window, 2, 5, 1200, true, 2, 1, 22, false);
  assert.equal(detached.resultFramePresented, undefined,
    "a detached producer cannot qualify even its still-published prior result");
  assert.equal(detached.preexistingFramesCompleted, 1);

  const unknown = { publishReason: "render-result", documentGeneration: 2, frameVersion: 5,
    captureId: 22, presentationBoundaryKnown: false };
  window.__donnerWorkerStats = unknown;
  presentFrame(window, 2, 5);
  assert.equal(unknown.presentationBoundaryUnknown, true);
  assert.equal(unknown.unattributedFramesCompleted, 1);
  assert.equal(unknown.resultFramePresented, undefined, "missing boundaries never qualify ordering");
  assert.equal(unknown.postPollFramesCompletedWithoutResult, undefined);

  const failure = { publishReason: "gpu-wait-failure", documentGeneration: 3, frameVersion: 1 };
  window.__donnerWorkerStats = failure;
  presentFrame(window, 3, 1);
  assert.equal(failure.resultFramePresented, undefined, "only render results can be presented");

  const failurePublisher = renderCoordinatorSource.match(
    /void PublishWorkerGpuWaitFailure\([\s\S]*?\n\}/,
  )[0];
  assert.match(failurePublisher, /delete stats\['resultFramePresented'\]/);
  assert.match(failurePublisher, /delete stats\['resultFrameCompletedAtMs'\]/);
  assert.match(failurePublisher, /delete stats\['framesPresentedWithoutResult'\]/);
  const completed = source.match(
    /void EditorWindow::recordCompletedDocumentFrame\([\s\S]*?\n\}/,
  )?.[0];
  assert.ok(completed, "the GPU-completion observer owns document publication");
  assert.match(completed, /if \(!frame\.fullFramePresented\) \{\s*return;/,
    "a partial frame is not a completed presentation");
  assert.match(completed,
    /isDeviceLost\(\)\) \{\s*observePresentationDeviceLoss\(device\);\s*return;/,
    "a failed GPU watermark cannot publish successful presentation stats or acknowledgment");
  assert.match(completed,
    /PublishPresentedDocumentFrame\(frame\.documentGeneration,[\s\S]*if \(!currentProducer\) \{\s*return;[\s\S]*lastFullyPresentedDocumentFrame_ = identity;/,
    "old producer completions stay visible without resurrecting their acknowledgment");
  const acceptedStart = source.indexOf("    if (!present(guard)) {");
  const acceptedEnd = source.indexOf("    completed = true;", acceptedStart);
  assert.ok(acceptedStart >= 0 && acceptedEnd > acceptedStart, "the real CPU acceptance path");
  assert.doesNotMatch(
    source.slice(acceptedStart, acceptedEnd),
    /lastFullyPresentedDocumentFrame_|PublishPresentedDocumentFrame|wakeEventLoopForPresentedDocument/,
    "CPU present acceptance alone cannot publish or discharge a document wake",
  );
});

test("ordinary and deferred observations both publish retired document frames", () => {
  const ordinary = source.match(
    /bool EditorWindow::observePresentationCompletion\(\) \{[\s\S]*?\n\}/,
  )?.[0];
  const deferred = source.match(
    /EditorWindow::PresentationProgress EditorWindow::pollDeferredPresentation\(\) \{[\s\S]*?\n\}/,
  )?.[0];
  for (const [path, observation] of [["ordinary", ordinary], ["deferred", deferred]]) {
    assert.ok(observation, `${path} completion path exists`);
    assert.match(observation, /recordCompletedDocumentFrame\(frame\)/,
      `${path} retirement delivers every frame to the document observer`);
    assert.match(observation, /device\.lastProgress\(\), onCompleted\)/,
      `${path} native observation retains its retirement observer`);
  }
  assert.match(ordinary, /\},\s*onCompleted\)/,
    "browser timeout confirmation retains the same per-frame observer");
  assert.match(deferred, /observe\(completed, now, std::nullopt, onCompleted\)/,
    "the non-drawing browser progress poll cannot silently consume completed documents");
  assert.match(deferred,
    /presentationWasDeferred_ \|\| presentationSubmissions_\.pendingCount\(\) != 0/,
    "accepted frames retain completion observation even below the admission bound");
  const commit = source.match(/struct SubmissionFenceCommit \{[\s\S]*?\n\};/)?.[0];
  assert.ok(commit, "the submission fence commit owns the progress pulse");
  assert.match(commit,
    /queue\.submitted\(stamp\);[\s\S]*if \(fullFramePresented\) \{\s*whole_app_worker::RequestPresentationProgress\(\)/,
    "a completed-frame obligation arms the existing pulse after its fence is queued");
});

test("an overdue browser UI frame defers presentation instead of declaring loss", () => {
  const ordinary = source.match(
    /bool EditorWindow::observePresentationCompletion\(\) \{[\s\S]*?\n\}/,
  )?.[0];
  assert.ok(ordinary, "the ordinary completion path exists");
  const timedOut = ordinary.match(
    /if \(admission == internal::PresentationSubmissionQueue::Admission::TimedOut\) \{\n#ifdef __EMSCRIPTEN__\n([\s\S]*?)#else\n([\s\S]*?)#endif\n\s*return false;/,
  );
  assert.ok(timedOut, "the timed-out admission splits browser and native handling");
  const [, browser, native] = timedOut;
  assert.doesNotMatch(browser, /declarePresentationTimeoutLoss|markDeviceLost/,
    "a browser stall must not declare a loss that nothing can recover");
  assert.match(browser, /deferOverduePresentation\(\);/,
    "the browser keeps the overdue frame deferred");
  const defer = source.match(
    /void EditorWindow::deferOverduePresentation\(\) \{[\s\S]*?\n\}/,
  )?.[0];
  assert.ok(defer, "the browser deferral exists");
  assert.doesNotMatch(defer, /declarePresentationTimeoutLoss|markDeviceLost/,
    "the deferral cannot declare a loss either");
  assert.match(defer, /presentationOverdueSerial_ = presentationSubmissions_\.oldestSerial\(\);/,
    "the browser records the overdue submission so later polls skip the blocking recheck");
  assert.match(native, /declarePresentationTimeoutLoss\(device, presentationTimeoutAge_\);/,
    "native backends still declare a stalled device lost");
});

test("the presentation timeline rides the queue-stats proxy call and stays bounded", () => {
  const observe = source.match(
    /bool EditorWindow::observePresentationCompletion\(\) \{[\s\S]*?\n\}/,
  )[0];
  const calls = [...observe.matchAll(/MAIN_THREAD_ASYNC_EM_ASM\(\s*\{([\s\S]*?)\n\s*\},/g)];
  assert.equal(calls.length, 2, "the timeline must not add a proxy call per frame");
  const body = calls.find((call) => call[1].includes("__donnerPresentationTimeline"));
  assert.ok(body, "the timeline sample must be published by the queue-stats call");
  assert.match(body[1], /__donnerPresentationQueueStats/);
  const publish = new Function(
    "window",
    "performance",
    ...Array.from({ length: 15 }, (_, index) => `$${index}`),
    body[1],
  );
  const window = {};
  const clock = { now: () => 12.34 };
  for (let frame = 0; frame < 600; ++frame) {
    publish(window, clock, 4, 9, 2, 0, 0, 0, 0, 0, 0, 0, 0, 3, 5, 0, 1);
  }
  assert.equal(window.__donnerPresentationTimeline.length, 512);
  assert.deepEqual(window.__donnerPresentationTimeline.at(-1), [12.3, 1, 2, 3, 9, 5]);
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
  const publishTiming = makeProductionPublisher(workerTimingPredicates,
    "window",
    "$0",
    "$1",
    "$2",
    "HEAPF64",
    "_free",
    "UTF8ToString",
    "performance", "$3 = false", "$4 = 0", "$5 = 0", "$6 = 0",
    body(timingPublisher[0]),
  );
  const publishAccepted = new Function("window", "$0", "$1", "$2", body(acceptedPublisher[0]));
  const count = Number(/kValueCount = (\d+)/.exec(timingPublisher[0])[1]);
  const heap = new Float64Array(count);
  const released = [];
  heap.set([11, 22, 33, 44, 0], 26);
  heap[32] = 1;
  heap.set([2, 3, 4, 5, 6], 35);
  heap[40] = 1200;
  const window = {
    __donnerWorkerStats: {
      completedResults: 8,
      presentedAtMs: 100,
      compositorReadbackTotal: 10,
      bitmapPayloadTileTotal: 3,
      texturePayloadTileTotal: 4,
      nothingToPresentTotal: 5,
    },
  };
  publishTiming(
    window,
    0,
    0,
    count,
    heap,
    (pointer) => {
      released.push(pointer);
      heap.fill(99);
    },
    () => "none",
    { timeOrigin: 1000, now: () => 200 },
  );
  assert.deepEqual(released, [0], "the asynchronous publication frees its owned buffer once");
  const stats = window.__donnerWorkerStats;
  assert.equal(stats.completedResults, 9);
  assert.equal(stats.presentationBoundaryKnown, false, "omitted native metadata stays unknown");
  assert.deepEqual([
    stats.compositorReadbackTotal,
    stats.tileHandoffReadbackTotal,
    stats.finalSnapshotReadbackTotal,
    stats.bitmapPayloadTileTotal,
    stats.texturePayloadTileTotal,
    stats.nothingToPresentTotal,
  ], [12, 3, 4, 8, 10, 6]);
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

test("a failed worker publication cannot inherit a prior frame's presentation admission", () => {
  const failure =
    renderCoordinatorSource.match(/void PublishWorkerGpuWaitFailure\([\s\S]*?\n\}/)[0];
  const frame = workerBridgeSource.match(/void RecordFrameSample\([\s\S]*?\n\}/)[0];
  const body = (method) => method.match(/MAIN_THREAD_ASYNC_EM_ASM\(\s*\{([\s\S]*?)\n\s*\},/)[1];
  const publishFailure = new Function(
    "window",
    "performance",
    "$0",
    "$1",
    "$2",
    "UTF8ToString",
    body(failure),
  );
  const publishFrame = makeFrameSamplePublisher(body(frame));
  const window = {
    __donnerFrameLoopStats: { uiFrameMsSamples: [] },
    __donnerHostFrameTiming: { lastSurfacePresented: true },
    __donnerWorkerStats: {
      completedResults: 3,
      acceptedForPresentation: true,
      presentedAtMs: 200,
      presentingFrame: "event",
      presentationPublicationDelayMs: 40,
      publicationDelayMs: 75,
      publishReason: "render-result",
    },
  };
  const previous = window.__donnerWorkerStats;
  const clock = { timeOrigin: 1000, now: () => 900 };
  publishFailure(window, clock, 1, 0, 5000, () => "queue-idle");
  publishFrame(window, clock, 1, 15, 1, 4096, 1215);
  const stats = window.__donnerWorkerStats;
  assert.equal(stats.presentingFrame, undefined, "a failed publish never presented a frame");
  assert.deepEqual({
    completedResults: stats.completedResults,
    acceptedForPresentation: stats.acceptedForPresentation,
    presentedAtMs: stats.presentedAtMs,
    presentationPublicationDelayMs: stats.presentationPublicationDelayMs,
    publicationDelayMs: stats.publicationDelayMs,
    deviceLost: stats.deviceLost,
    publishReason: stats.publishReason,
  }, {
    completedResults: 3,
    acceptedForPresentation: false,
    presentedAtMs: undefined,
    presentationPublicationDelayMs: undefined,
    publicationDelayMs: undefined,
    deviceLost: true,
    publishReason: "gpu-wait-failure",
  });
  assert.equal(previous.presentedAtMs, 200, "the failure must not modify the older frame object");
  assert.equal(
    previous.publicationDelayMs,
    75,
    "the original render keeps its own publication delay",
  );
  stats.acceptedForPresentation = true;
  publishFrame(window, clock, 1, 15, 1, 4096, 1230);
  assert.equal(
    stats.presentedAtMs,
    undefined,
    "failure publications cannot present even if an admission marker is stale",
  );
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
    /if \(!IsCurrentRenderResult\(resultOpt, app\)\) \{\s*rejectRenderResult\(resultOpt\);\s*return;\s*\}/,
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

test("the composited viewport sample gate waits for the document worker", () => {
  const helper = extractAsyncFunction(compositedViewportSource, "openDonnerSplash");
  assert.match(helper, /__donnerWorkerStats/);
  assert.match(helper, /acceptedForPresentation === true/);
  assert.match(helper, /typeof worker\.presentedAtMs === "number"/);
  assert.doesNotMatch(helper, /__donnerLayerThumbnailStats/);
});

test("the sample picker publishes whether its thumbnail lane has drained", () => {
  const bodyOf = (signature) => {
    const start = editorShellSource.indexOf(signature);
    assert.ok(start >= 0, `expected ${signature}`);
    return editorShellSource.slice(start, editorShellSource.indexOf("\n}\n", start));
  };
  const publisher = bodyOf("void EditorShell::publishSampleThumbnailStats() const {");
  assert.match(publisher, /sampleThumbnailLaneDrained\(stats\) \? 1 : 0/);
  assert.match(editorShellSource, /'drained' : Boolean\(\$12\)/);
  // Every visible sample finished and none set aside for a font: the idle counters alone also read
  // idle between two attempts and while a font-blocked thumbnail waits for its retry.
  const drained = bodyOf(
    "bool EditorShell::sampleThumbnailLaneDrained(const SampleThumbnailRenderStats& stats) const {",
  );
  for (
    const condition of [
      /!showSamplePicker_/,
      /visibleSamplePreviewIndices_\.empty\(\)/,
      /sampleThumbnailInFlightIndex_/,
      /stats\.pending/,
      /stats\.active/,
      /stats\.resultReady/,
      /finishedSamplePreviewIndices_\.contains\(index\)/,
      /!waitingSamplePreviews_\.contains\(index\)/,
    ]
  ) {
    assert.match(drained, condition, `the drained flag must require ${condition}`);
  }
});

test("thumbnail publication keeps drained state separate from explicit readback count", () => {
  const publisher = editorShellSource.match(/void PublishSampleThumbnailStats\([\s\S]*?\n\}/);
  assert.ok(publisher);
  const callback = publisher[0].match(
    /MAIN_THREAD_ASYNC_EM_ASM\(\s*\{([\s\S]*?)\n\s*\},([\s\S]*?)\);/,
  );
  assert.ok(callback);
  const argumentNames = callback[2].split(",").map((name) => name.trim());
  assert.ok(argumentNames.length <= 15, "the SDK proxy argument limit must be respected");
  const publish = new Function(
    "window",
    "performance",
    ...argumentNames.map((_, index) => `$${index}`),
    callback[1],
  );
  const window = {};
  for (const [drained, readbackCount] of [[1, 37], [0, 41]]) {
    const values = { drained, readbackCount, completed: 5, ready: 5, fontPreviewObligations: 3 };
    publish(window, { now: () => 123 }, ...argumentNames.map((name) => values[name] ?? 0));
    assert.equal(window.__donnerSampleThumbnailStats.drained, Boolean(drained));
    assert.equal(window.__donnerSampleThumbnailStats.explicitPreviewReadbackTotal, readbackCount);
    assert.equal(window.__donnerSampleThumbnailStats.fontPreviewObligations, 3);
  }
});

test("the shared sample thumbnail gate waits for a drained thumbnail lane", () => {
  const helper = extractAsyncFunction(
    sampleThumbnailSettleSource,
    "expectSampleThumbnailsToSettle",
  );
  assert.match(helper, /__donnerSampleThumbnailStats/);
  assert.match(helper, /stats\.drained === true/);
  assert.match(helper, /\(stats\.completed\s*\?\?\s*0\)\s*>\s*0/);
  assert.match(helper, /\(stats\.ready\s*\?\?\s*0\)\s*>\s*0/);
  assert.match(helper, /!stats\.active/);
  assert.match(helper, /!stats\.pending/);
  assert.match(helper, /!stats\.resultReady/);
  // A lane that never drains has to say whether the worker behind it is healthy.
  for (const field of ["completedResults", "deviceLost", "gpuWaitTimeoutSite", "publishReason"]) {
    assert.match(helper, new RegExp(`${field}: worker\\?\\.${field}`), `must print ${field}`);
  }
  // As with the worker-health gate, `toEqual` is what prints the whole thumbnail snapshot when the
  // lane never goes idle; `toMatchObject` would print only the flag.
  assert.match(helper, /\.toEqual\(expect\.objectContaining\(\{ settled: true \}\)\)/);
  assert.doesNotMatch(helper, /toMatchObject/);
});

test("post-picker thumbnail quiescence includes queued and font-blocked previews", () => {
  const settledAfterPickerClosed = {
    requested: 17,
    started: 17,
    completed: 17,
    pending: false,
    active: false,
    resultReady: false,
    drained: false,
    fontPreviewObligations: 0,
  };
  assert.equal(sampleThumbnailLaneQuiescent(settledAfterPickerClosed), true);
  assert.equal(
    sampleThumbnailLaneQuiescent({ ...settledAfterPickerClosed, requested: 0, completed: 0 }),
    true,
    "an unused lane is idle",
  );
  assert.equal(
    sampleThumbnailLaneQuiescent({ ...settledAfterPickerClosed, started: 16 }),
    true,
    "cancelling a queued request completes it without starting it",
  );
  for (const field of ["pending", "active", "resultReady"]) {
    assert.equal(
      sampleThumbnailLaneQuiescent({ ...settledAfterPickerClosed, [field]: true }),
      false,
      `${field} work is not quiescent`,
    );
  }
  assert.equal(
    sampleThumbnailLaneQuiescent({ ...settledAfterPickerClosed, completed: 16 }),
    false,
  );
  for (const obligation of ["a queued preview", "a font-blocked preview"]) {
    assert.equal(
      sampleThumbnailLaneQuiescent({ ...settledAfterPickerClosed, fontPreviewObligations: 1 }),
      false,
      `${obligation} can submit work after a font-store wake`,
    );
  }
  for (const value of [undefined, Number.NaN, -1]) {
    assert.equal(
      sampleThumbnailLaneQuiescent({ ...settledAfterPickerClosed, fontPreviewObligations: value }),
      false,
      "missing or invalid shell obligations cannot establish quiescence",
    );
  }
  assert.equal(sampleThumbnailLaneQuiescent(undefined), false);
  for (const field of ["pending", "active", "resultReady"]) {
    const missing = { ...settledAfterPickerClosed };
    delete missing[field];
    assert.equal(sampleThumbnailLaneQuiescent(missing), false, `missing ${field} is not idle`);
  }
  for (const value of [Number.NaN, -1]) {
    assert.equal(
      sampleThumbnailLaneQuiescent({ ...settledAfterPickerClosed, requested: value }),
      false,
    );
  }
  assert.notDeepEqual(
    sampleThumbnailLaneSerial(settledAfterPickerClosed),
    sampleThumbnailLaneSerial({ ...settledAfterPickerClosed, requested: 18, completed: 18 }),
    "a request completed between polls must restart readiness",
  );
  assert.match(presentationRegressionSource, /requireThumbnailQuiescence: true/);
});

test("sample loads that do not test the thumbnail handoff settle thumbnails first", () => {
  const gates = [
    ["openBasicShapes", extractAsyncFunction(presentationRegressionSource, "openBasicShapes")],
    ["openDonnerSplash", extractAsyncFunction(presentationRegressionSource, "openDonnerSplash")],
    ...[
      "Geode crown face paints at each held pointer position before release",
      "WebKit Geode survives a burst of drag wakeups without fatal errors",
    ].map((title) => [title, extractTest(presentationRegressionSource, title)]),
    ...["Firefox restores the Splash canvas after transient surface loss"]
      .map((title) => [title, extractTest(surfaceRecoverySource, title)]),
    ...["Firefox keeps Basic Shapes resize pixels and outline synchronized"]
      .map((title) => [title, extractTest(smokeSource, title)]),
    ...["the raster worker selects the backend its package was built for"]
      .map((title) => [title, extractTest(backendSelectionSource, title)]),
    ...[
      ["composited openDonnerSplash", compositedViewportSource, "openDonnerSplash"],
      ["composited drag openDonnerSplash", compositedDragSource, "openDonnerSplash"],
      ["composited drag openBasicShapes", compositedDragSource, "openBasicShapes"],
    ].map(([name, source, helper]) => [name, extractAsyncFunction(source, helper)]),
  ];
  for (const [name, body] of gates) {
    const settle = body.indexOf("await expectSampleThumbnailsToSettle(page, {");
    const sampleClick = body.indexOf("page.mouse.click");
    assert.ok(
      settle >= 0 && settle < sampleClick,
      `${name} must not replace a first-use thumbnail render before it settles`,
    );
    assert.match(body.slice(settle, sampleClick), /timeout:\s*(?:scaledMs\(20_000\)|20000)/, name);
  }
  const residencyStartup = extractAsyncFunction(gpuResidencySource, "openEditor");
  assert.match(residencyStartup, /await expectSampleThumbnailsToSettle\(page, \{/);
  assert.match(residencyStartup, /timeout: 20000/);
  for (
    const spec of [
      gpuResidencySource,
      presentationRegressionSource,
      surfaceRecoverySource,
      smokeSource,
      backendSelectionSource,
      compositedViewportSource,
      compositedDragSource,
    ]
  ) {
    assert.match(
      spec,
      /^import \{ expectSampleThumbnailsToSettle \} from "\.\/sample-thumbnail-settle";$/m,
    );
  }
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
    /OpenBrowserDevice\(root->lostState\(\), "runtime"\)/,
    "each logical context must reopen over the same loss condition with a fixed stage",
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
  // One raster std::thread owns C++ state; delaying its start protects owner-side UI GPU setup,
  // independently of the single-canvas ownership contract.
  assert.doesNotMatch(workerRendererSource, /pthread_create/);
  const startMode = renderCoordinatorSource.match(
    /constexpr AsyncRendererStartMode EditorRenderWorkerStartMode\(\) \{\s*#ifdef __EMSCRIPTEN__([\s\S]*?)#else([\s\S]*?)#endif\s*}/,
  );
  assert.ok(startMode, "expected the coordinator's platform-specific worker startup mode");
  assert.match(startMode[1], /return AsyncRendererStartMode::Deferred;/);
  assert.match(startMode[2], /return AsyncRendererStartMode::Immediate;/);
  assert.match(
    renderCoordinatorSource,
    /RenderWorkerBundle::RenderWorkerBundle\([\s\S]*?asyncRenderer\(EditorRenderWorkerStartMode\(\)\)/,
    "the actual worker bundle must use the platform startup mode",
  );

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

test("UI asm diagnostics stay within the compiler's sixteen argument slots", () => {
  for (const match of source.matchAll(/MAIN_THREAD_ASYNC_EM_ASM\(\s*\{([\s\S]*?)\n\s*\},/g)) {
    const slots = [...match[1].matchAll(/\$(\d+)/g)].map((slot) => Number(slot[1]));
    assert.ok(slots.every((slot) => slot < 16), `unsupported asm argument slots: ${slots}`);
  }
});

test("async presentation loss publication owns and always frees its reason payload", () => {
  const start = source.indexOf("void EditorWindow::observePresentationDeviceLoss(");
  const end = source.indexOf("\nbool EditorWindow::observePresentationCompletion()", start);
  assert.ok(start >= 0 && end > start, "expected the presentation loss publisher");
  const publisher = source.slice(start, end);
  assert.match(publisher, /std::malloc\(reasonBytes \+ 1\)/);
  assert.match(publisher, /const double firstObservedAbsoluteMs = emscripten_get_now\(\)/);
  assert.match(publisher, /'firstObservedAtMs'\s*:\s*\$6 - performance\.timeOrigin/);
  assert.match(publisher, /'pageTimeOriginMs'\s*:\s*performance\.timeOrigin/);
  assert.match(publisher, /'publishedAtMs'\s*:\s*performance\.now\(\)/);
  assert.match(publisher, /finally\s*\{[\s\S]*if \(\$5\)\s*\{\s*_free\(\$5\)/);
  assert.match(publisher, /'browserReason'\s*:\s*\$5 \? UTF8ToString\(\$5\) : String\(\)/);
  assert.doesNotMatch(publisher, /browserReason\.c_str\(\)/);
});

test("presented document wakes use the event driver while generic wakes stay rAF paced", () => {
  const presentedWake = source.match(
    /void EditorWindow::wakeEventLoopForPresentedDocument\(\) \{([\s\S]*?)\n\}/,
  );
  assert.ok(presentedWake, "expected the presentation-specific wake entry");
  assert.match(
    presentedWake[1],
    /#ifdef DONNER_EDITOR_WHOLE_APP_WORKER[\s\S]*whole_app_worker::RequestFrame\(\);[\s\S]*#endif/,
    "a matching presentation must schedule its held preview work even while worker rAF is delayed",
  );
  const genericWake = source.match(/void EditorWindow::wakeEventLoop\(\) \{([\s\S]*?)\n\}/);
  assert.ok(genericWake, "expected the ordinary UI wake entry");
  assert.doesNotMatch(
    genericWake[1],
    /whole_app_worker::RequestFrame/,
    "ordinary UI and animation wakes must retain their rAF cadence",
  );
});

test("selection chrome probe refreshes stale captures without flooding pending requests", async (t) => {
  const selectionTest = extractTest(
    smokeSource,
    "Geode WASM presents selection path overlay pixels",
  );
  const probeStart = selectionTest.indexOf("  try {\n");
  const probeEnd = selectionTest.indexOf("  } catch (error)", probeStart);
  assert.ok(probeStart >= 0 && probeEnd > probeStart, "expected the live selection chrome probe");
  const probe = selectionTest.slice(probeStart + "  try {\n".length, probeEnd);
  const runProbe = new Function(
    "page",
    "expect",
    "requestWgpuDiagnostic",
    "beforeSelectionChrome",
    "selectionRequest",
    `return (async () => {${probe}})();`,
  );

  for (const scenario of ["delayed capture", "missing overlay", "unacknowledged request"]) {
    await t.test(scenario, async () => {
      const state = { __donnerWgpuReadbackStats: { request: 3, selectionChromePixels: 384 } };
      const requests = [];
      const observations = [];
      let pending = null;
      const page = {
        evaluate: async (callback) => new Function("window", `return (${callback})()`)(state),
      };
      const expect = {
        poll: (callback, options) => ({
          toBeGreaterThan: async (threshold) => {
            assert.equal(threshold, 50, "retain the overlay pixel threshold");
            assert.equal(options.timeout, 20000, "retain the existing probe budget");
            for (let attempt = 0; attempt < 5; ++attempt) {
              const value = await callback();
              observations.push(value);
              if (value > threshold) return;
              if (pending && scenario !== "unacknowledged request" && ++pending.polls === 2) {
                state.__donnerWgpuReadbackStats = {
                  request: pending.request,
                  selectionChromePixels: scenario === "delayed capture" ? 608 : 384,
                };
                pending = null;
              }
            }
            assert.fail(`selection chrome stayed at stale capture: ${observations}`);
          },
        }),
      };
      const requestWgpuDiagnostic = async () => {
        assert.equal(pending, null, "a request must finish before another is sent");
        const request = state.__donnerWgpuReadbackStats.request + 1;
        requests.push(request);
        pending = { request, polls: 0 };
        return request;
      };
      const result = runProbe(page, expect, requestWgpuDiagnostic, 384, 3);
      if (scenario === "delayed capture") {
        await result;
        assert.deepEqual(requests, [4]);
        assert.deepEqual(observations, [0, 0, 224]);
      } else {
        await assert.rejects(result, /selection chrome stayed at stale capture/);
        assert.deepEqual(requests, scenario === "missing overlay" ? [4, 5, 6] : [4]);
        assert.deepEqual(observations, [0, 0, 0, 0, 0]);
      }
    });
  }
});


test("production worker boundary predicates reject missing and unsafe identities", () => {
  const known = workerTimingPredicates.WorkerResultBoundaryKnown;
  for (const [supplied, sequence, producer, capture, expected] of [
    [true, 0, 0, 1, true],
    [false, 0, 0, 1, false],
    [true, -1, 0, 1, false],
    [true, Number.NaN, 0, 1, false],
    [true, Number.MAX_SAFE_INTEGER + 1, 0, 1, false],
    [true, 1, Number.POSITIVE_INFINITY, 1, false],
    [true, 1, 0, 0, false],
    [true, 1, 0, Number.MAX_SAFE_INTEGER + 1, false],
  ]) {
    assert.equal(Boolean(known(supplied, sequence, producer, capture)), expected);
  }
  const completion = workerFramePredicates.WorkerFrameBoundaryKnown;
  assert.equal(Boolean(completion(true, 0, 0)), false);
  assert.equal(Boolean(completion(true, 1, 0)), true);
  assert.equal(Boolean(completion(true, 1, -1)), false);
  assert.equal(Boolean(completion(true, Number.NaN, 0)), false);
  assert.equal(Boolean(workerFramePredicates.WorkerFrameSameProducer(false, 1, 1)), false);
  assert.equal(Boolean(workerFramePredicates.WorkerFrameSameProducer(true, 1, 2)), false);
  assert.equal(Boolean(workerFramePredicates.WorkerFrameEligible(true, true, 2, 1)), true);
  assert.equal(Boolean(workerFramePredicates.WorkerFrameEligible(true, true, 1, 1)), false);
  assert.equal(Boolean(workerFramePredicates.WorkerFrameEligible(false, true, 2, 1)), false);
  assert.equal(Boolean(workerFramePredicates.WorkerFrameIdentityMatches(1, 2, 3, 1, 2, 4)), false);
});


test("production completion-time predicate preserves finite nonnegative timestamps", () => {
  const valid = workerFramePredicates.WorkerFrameCompletionTimeValid;
  for (const [completedAt, expected] of [
    [0, true], [-0, true], [10, true], [-1, false],
    [Number.NaN, false], [Number.POSITIVE_INFINITY, false], [undefined, false],
  ]) {
    assert.equal(Boolean(valid(completedAt)), expected);
  }
});
