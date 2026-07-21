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
const geodeDeviceSource = await readFile(
  new URL("../../../svg/renderer/geode/GeodeDevice.cc", import.meta.url),
  "utf8",
);
const geodeDeviceHeader = await readFile(
  new URL("../../../svg/renderer/geode/GeodeDevice.h", import.meta.url),
  "utf8",
);

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

  const asyncReadback = source.match(
    /void BeginAsyncSmokeReadback\([\s\S]*?\n}\n#endif/,
  );
  assert.ok(asyncReadback, "expected the asynchronous diagnostic readback callback");
  assert.match(
    asyncReadback[0],
    /inFlight->store\(false,[^;]*\);\s*if \([^)]*ShouldRecheckPendingWgpuReadbackRequestsAfterCompletion[\s\S]*?WakeWasmEditorForPendingWgpuReadback\(\)/,
    "every live map completion must clear the in-flight gate before rechecking pending requests",
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

test("worker surface loss uses bounded recovery without aborting Wasm", () => {
  assert.match(workerRendererSource, /WGPUSurfaceGetCurrentTextureStatus_Timeout/);
  assert.match(workerRendererSource, /WGPUSurfaceGetCurrentTextureStatus_Outdated/);
  assert.match(workerRendererSource, /WGPUSurfaceGetCurrentTextureStatus_Lost/);
  assert.match(workerRendererSource, /WorkerSurfaceRecoveryDecisionFor/);
  assert.doesNotMatch(
    workerRendererSource,
    /UTILS_RELEASE_ASSERT_MSG\(\s*directSurfacePresented/,
  );
  assert.match(workerRendererHeader, /DirectSurfacePresentationOutcome[\s\S]*RetryAfterBackoff/);
  assert.match(
    renderCoordinatorSource,
    /DirectSurfacePresentationOutcome::RetryAfterBackoff[\s\S]*DirectSurfaceRetryBackoffForAttempt/,
  );
  assert.match(
    renderCoordinatorSource,
    /DirectSurfacePresentationOutcome::Unavailable[\s\S]*ReportDirectSurfaceUnavailable/,
  );
  assert.match(
    editorShellSource,
    /nextIdleWakeSeconds\(\)[\s\S]*nextDirectSurfaceRetryWakeSeconds\(\)/,
  );
});

test("worker surface diagnostics are independently opt-in and close after presentation", () => {
  const optIn = workerRendererSource.match(
    /EM_JS\(int, UseWorkerSurfaceDiagnostic, \(\),([\s\S]*?)\);/,
  );
  assert.ok(optIn, "expected a worker-surface-specific diagnostic probe");
  assert.match(optIn[1], /workerSurfaceDiagnostic/);
  assert.doesNotMatch(optIn[1], /wgpuReadbackStats/);

  assert.match(workerRendererHeader, /workerSurfaceDiagnosticAttempted_/);
  assert.match(
    workerRendererSource,
    /captureSurfaceDiagnostic\s*=\s*[\s\S]*!workerSurfaceDiagnosticPublished_[\s\S]*if\s*\(captureSurfaceDiagnostic\)[\s\S]*workerSurfaceDiagnosticAttempted_\s*=\s*true;[\s\S]*takeSnapshot\(\)/,
  );
  assert.match(
    workerRendererSource,
    /WorkerSurfacePresentDisposition::RetryNextWorkerTask[\s\S]*workerSurfaceDiagnosticAttempted_\s*=\s*false/,
  );
  assert.doesNotMatch(workerRendererSource, /if\s*\(\s*!surfaceDiagnostic\.empty\(\)\s*\)/);
  assert.match(
    workerRendererSource,
    /directSurfacePresented[\s\S]*PublishWorkerSurfaceDiagnostic[\s\S]*workerSurfaceDiagnosticPublished_\s*=\s*true/,
  );
});

test("direct worker surfaces cross a local event-loop task before acceptance", () => {
  const taskCompletion = workerRendererSource.match(
    /void AsyncRenderer::runWorkerTask\(void\* [^)]*\)([\s\S]*?)\n}\n\nvoid AsyncRenderer::acknowledgeDirectSurfaceTaskBoundary/,
  );
  assert.ok(taskCompletion, "expected the worker task-boundary handoff");
  assert.match(
    taskCompletion[1],
    /emscripten_set_timeout\(&AsyncRenderer::acknowledgeDirectSurfaceTaskBoundary,/,
    "a zero-delay worker-local timer must return through JavaScript before accepting the surface",
  );
  assert.doesNotMatch(taskCompletion[1], /emscripten_set_immediate/);
  assert.doesNotMatch(
    taskCompletion[1],
    /emscripten_proxy_async\([^;]*acknowledgeDirectSurfaceTaskBoundary/,
  );
  assert.match(taskCompletion[1], /frameToken\s*=\s*\*presentationBoundaryToken/);
  assert.match(
    workerRendererSource,
    /WasmDirectSurfaceTaskBoundaryCallbackContext[\s\S]*shared_ptr<WasmWorkerRuntimeInitControl>/,
    "the delayed callback must retain the shutdown owner gate",
  );
  const acknowledgment = workerRendererSource.match(
    /void AsyncRenderer::acknowledgeDirectSurfaceTaskBoundary\(void\* userdata\)([\s\S]*?)\n}\n\nWorkerTaskScheduleResult/,
  );
  assert.ok(acknowledgment, "expected the delayed task-boundary callback");
  assert.match(acknowledgment[1], /control->owner/);
  assert.match(
    acknowledgment[1],
    /acknowledgeDirectSurfaceTaskBoundaryLocked\(context->frameToken, wake\)/,
    "a stale callback must not acknowledge a replacement pending frame",
  );
  assert.match(acknowledgment[1], /presentationBoundaryPending/);
  assert.match(workerRendererHeader, /PendingDirectSurfaceTaskBoundaryState/);
});

test("ready worker shutdown keeps task-boundary callbacks attached through join", () => {
  const shutdown = workerRendererSource.match(
    /void AsyncRenderer::shutdown\(\)([\s\S]*?)\n}\n\n#ifdef DONNER_WASM_WORKER_SURFACE/,
  );
  assert.ok(shutdown, "expected the renderer shutdown path");
  assert.match(shutdown[1], /ChooseWasmWorkerOwnerDetachTiming/);

  const beforeJoinDetach = shutdown[1].indexOf(
    "if (ownerDetachTiming == WasmWorkerOwnerDetachTiming::BeforeWorkerJoin)",
  );
  const workerJoin = shutdown[1].indexOf("pthread_join(thread_, nullptr)");
  const afterJoinDetach = shutdown[1].indexOf(
    "if (ownerDetachTiming == WasmWorkerOwnerDetachTiming::AfterWorkerJoin)",
  );
  assert.ok(beforeJoinDetach >= 0 && beforeJoinDetach < workerJoin);
  assert.ok(workerJoin >= 0 && afterJoinDetach > workerJoin);
  assert.match(
    workerRendererHeader,
    /status == WasmWorkerRuntimeInitializationStatus::Ready[\s\S]*AfterWorkerJoin/,
  );
});

test("worker WebGPU startup imports one browser Promise chain and keeps pending work off the proxy queue", () => {
  assert.match(geodeDeviceHeader, /CreateHeadlessAsync/);
  assert.match(geodeDeviceSource, /\(async \(\) =>/);
  assert.doesNotMatch(geodeDeviceSource, /= >/);
  assert.match(geodeDeviceSource, /navigator\.gpu\.requestAdapter/);
  assert.match(geodeDeviceSource, /adapter\.requestDevice/);
  assert.match(geodeDeviceSource, /WebGPU\.importJsAdapter\(adapter, instance\)/);
  assert.match(geodeDeviceSource, /WebGPU\.importJsDevice\(device, adapterPtr\)/);
  assert.match(geodeDeviceSource, /Module\["_donnerGeodeCompleteHeadlessImport"\]/);
  assert.match(
    geodeDeviceSource,
    /setTimeout\(\(\) => \{\s*callUserCallback\(\(\) => \{\s*Module\["_donnerGeodeCompleteHeadlessImport"\]/,
    "normal pthread unwind must be consumed without hiding real Wasm traps",
  );
  assert.equal(
    [...geodeDeviceSource.matchAll(/Module\["_donnerGeodeCompleteHeadlessImport"\]/g)].length,
    1,
    "the exported C continuation must be invoked exactly once, outside the acquisition catch",
  );
  assert.match(geodeDeviceSource, /EMSCRIPTEN_KEEPALIVE void donnerGeodeCompleteHeadlessImport/);
  assert.doesNotMatch(geodeDeviceSource, /RequestAdapterCallbackInfo/);
  assert.doesNotMatch(geodeDeviceSource, /RequestDeviceCallbackInfo/);
  assert.equal(
    [...geodeDeviceSource.matchAll(/navigator\.gpu\.requestAdapter\(\)/g)].length,
    1,
    "browser adapter acquisition must have one Promise root",
  );
  assert.equal(
    [...geodeDeviceSource.matchAll(/adapter\.requestDevice\(/g)].length,
    1,
    "browser device acquisition must continue from that adapter exactly once",
  );

  const workerInitialization = workerRendererSource.match(
    /void AsyncRenderer::beginWasmWorkerRuntimeInitialization\(\)([\s\S]*?)\n}/,
  );
  assert.ok(workerInitialization, "expected an event-driven worker runtime initializer");
  assert.match(workerInitialization[1], /CreateHeadlessAsync/);
  assert.doesNotMatch(workerInitialization[1], /CreateHeadless\(/);
  assert.doesNotMatch(workerInitialization[1], /emscripten_proxy/);
  assert.match(workerRendererSource, /DeferUntilRuntimeReady/);
  assert.match(workerRendererSource, /wasmWorkerRuntimeInitControl_/);

  const deviceDestructor = geodeDeviceSource.match(
    /GeodeDevice::~GeodeDevice\(\)([\s\S]*?)\n}/,
  );
  assert.ok(deviceDestructor, "expected thread-affined WebGPU teardown");
  const emscriptenBranch = deviceDestructor[1].match(
    /#ifndef __EMSCRIPTEN__([\s\S]*?)#endif/,
  );
  assert.ok(emscriptenBranch, "expected native-only submitted-work wait");
  assert.match(emscriptenBranch[1], /WaitForSubmittedWork/);
});

test("renderer pthread startup waits for cursor setup and wake wiring", () => {
  const constructor = workerRendererSource.match(
    /AsyncRenderer::AsyncRenderer\([^)]*\)([\s\S]*?)\n}\n\nvoid AsyncRenderer::start/,
  );
  assert.ok(constructor, "expected the AsyncRenderer constructor");
  assert.doesNotMatch(
    constructor[1],
    /pthread_create/,
    "constructing renderer ownership must not race the main-thread WebGPU setup",
  );

  const startup = workerRendererSource.match(
    /void AsyncRenderer::start\(\)([\s\S]*?)\n}\n\nAsyncRenderer::~AsyncRenderer/,
  );
  assert.ok(startup, "expected explicit renderer-worker startup");
  assert.match(startup[1], /pthread_create/);
  assert.match(
    renderCoordinatorSource,
    /#ifdef DONNER_WASM_WORKER_SURFACE\s*return AsyncRendererStartMode::Deferred;/,
    "the browser editor coordinator must opt into deferred construction",
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
  const browserCursorApply = rotateCursorSource.match(
    /EM_JS\(bool, ApplyBrowserCursor,[\s\S]*?\n\}\);/,
  );
  assert.ok(browserCursorApply, "expected a browser cursor application hook");
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
