#!/usr/bin/env node

/**
 * Real Apple Safari regression gate for the Geode Wasm editor.
 *
 * This test intentionally drives Safari through Apple's SafariDriver. Playwright's WebKit build is
 * useful compatibility coverage, but it is not Apple Safari and does not reproduce every Safari
 * WebAssembly, pthread, or WebGPU failure.
 *
 * Start SafariDriver and an HTTP server for an exact, already-built Geode package, then run:
 *
 *   safaridriver -p 4445
 *   DONNER_WASM_BASE_URL=http://127.0.0.1:8000 \
 *   DONNER_SAFARI_EXPECTED_WASM_SHA256=<64-lowercase-hex> \
 *     node donner/editor/wasm/tests/safari-geode-regression.mjs
 *
 * Environment:
 *   DONNER_WASM_BASE_URL          Served package URL (default http://127.0.0.1:8000).
 *   DONNER_SAFARI_DRIVER_URL      SafariDriver URL (default http://127.0.0.1:4445).
 *   DONNER_SAFARI_ARTIFACT_DIR    JSON/screenshots output directory (default under /tmp).
 *   DONNER_SAFARI_TIMEOUT_MS      Per-phase timeout in milliseconds (default 30000).
 *   DONNER_SAFARI_EXPECTED_WASM_SHA256
 *                                  Required lowercase SHA-256 of the served editor.wasm.
 *   DONNER_SAFARI_ALLOW_UNPINNED_PACKAGE=1
 *                                  Explicit exploratory-only bypass for the required digest.
 *   DONNER_SAFARI_REQUIRED=1      Treat unavailable Safari automation as failure, not a skip.
 *   DONNER_SAFARI_MEMORY_ONLY=1   Hold Donner Splash for five minutes and detect resource growth or
 *                                  a Safari significant-memory reload.
 */

import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import crypto from "node:crypto";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";

const kDriverUrl = process.env.DONNER_SAFARI_DRIVER_URL || "http://127.0.0.1:4445";
const kBaseUrl = process.env.DONNER_WASM_BASE_URL || "http://127.0.0.1:8000";
const kRequired = process.env.DONNER_SAFARI_REQUIRED === "1";
const kExpectedWasmSha256 = process.env.DONNER_SAFARI_EXPECTED_WASM_SHA256 || "";
const kAllowUnpinnedPackage = process.env.DONNER_SAFARI_ALLOW_UNPINNED_PACKAGE === "1";
const kMemoryOnly = process.env.DONNER_SAFARI_MEMORY_ONLY === "1";
const kMemoryDwellMs = 5 * 60 * 1_000;
const kMemoryMaxWebContentRssBytes = 512 * 1024 * 1024;
const kMemorySampleIntervalMs = 5_000;
const kTimeoutMs = Number(process.env.DONNER_SAFARI_TIMEOUT_MS || 30_000);
// Where the Basic Shapes blue rounded rectangle sits inside `#canvas` at the
// harness's 1600x1000 window. the single-canvas architecture draws the document inside the single
// canvas's WebGPU frame, so pointer targets are canvas offsets rather than
// fractions of a separate document element.
const kBlueRectOffset = { x: 408, y: 353 };
const kRunId = new Date().toISOString().replaceAll(/[^0-9A-Za-z]/g, "");
const kPageLifetimeToken = crypto.randomUUID();
const kArtifactDir = process.env.DONNER_SAFARI_ARTIFACT_DIR
  || path.join(os.tmpdir(), `donner-safari-geode-${kRunId}`);
const kFatalPattern =
  /Failed to wake Wasm renderer pthread|Wasm renderer pthread wake rejected|Aborted|Assertion failed|RuntimeError|Out of bounds|call_indirect|Unreachable code|UTILS_RELEASE_ASSERT|Pthread .* sent an error|getJsObject|No available adapters|WebGPU adapter (?:request )?(?:failed|unavailable)|Direct WebGPU import failed|Browser WebGPU import returned incomplete handles|Uncaptured WebGPU error|WebGPU device lost/i;

if (process.argv.includes("--help") || process.argv.includes("-h")) {
  console.log(`Usage: node ${path.basename(process.argv[1])}

Runs the Geode Wasm editor in real Apple Safari through an already-running SafariDriver.
See the source header for setup and environment variables.`);
  process.exit(0);
}

if (!Number.isFinite(kTimeoutMs) || kTimeoutMs <= 0) {
  throw new Error(`DONNER_SAFARI_TIMEOUT_MS must be positive, got ${kTimeoutMs}`);
}

function expectedWasmSha256ForRun() {
  if (process.env.DONNER_SAFARI_PACKAGE_ID) {
    throw new Error(
      "DONNER_SAFARI_PACKAGE_ID is no longer an arbitrary build label; use "
        + "DONNER_SAFARI_EXPECTED_WASM_SHA256 with the exact served Wasm digest",
    );
  }
  if (kExpectedWasmSha256) {
    if (!/^[0-9a-f]{64}$/.test(kExpectedWasmSha256)) {
      throw new Error(
        "DONNER_SAFARI_EXPECTED_WASM_SHA256 must be exactly 64 lowercase hexadecimal characters",
      );
    }
    return kExpectedWasmSha256;
  }
  if (kAllowUnpinnedPackage) {
    return null;
  }
  throw new Error(
    "DONNER_SAFARI_EXPECTED_WASM_SHA256 is required for the real Safari gate. "
      + "Set DONNER_SAFARI_ALLOW_UNPINNED_PACKAGE=1 only for an exploratory, non-gating run",
  );
}

class SafariAutomationUnavailable extends Error {}

function safariWebContentProcesses() {
  const output = execFileSync(
    "/bin/ps",
    ["-ax", "-o", "pid=,rss=,%cpu=,command="],
    { encoding: "utf8" },
  );
  const processes = [];
  for (const line of output.split("\n")) {
    const match = line.match(/^\s*(\d+)\s+(\d+)\s+([\d.]+)\s+(.+)$/);
    if (!match || !match[4].endsWith("/com.apple.WebKit.WebContent")) {
      continue;
    }
    processes.push({
      pid: Number(match[1]),
      rssBytes: Number(match[2]) * 1024,
      cpuPercent: Number(match[3]),
    });
  }
  return processes;
}

// Raising the automation window is a convenience, not a gate: the run's real
// visibility requirement is checked by `requireVisibleSafariAnimationFrame`.
//
// It has to be best-effort and time-bounded because the Apple Events
// permission for this script's parent process is granted interactively. Where
// that grant is missing, `osascript` does not fail: it blocks waiting on a
// consent decision that a non-interactive runner will never make, and the
// whole gate sat there for about nine minutes before anything else happened.
// Bound it, and say out loud that the window was not raised, rather than
// letting a cosmetic step decide how long the suite takes.
const kRaiseWindowTimeoutMs = 5_000;

function bringSafariAutomationWindowForward() {
  if (process.platform !== "darwin") {
    return;
  }
  try {
    execFileSync(
      "/usr/bin/osascript",
      [
        "-e",
        "tell application \"Safari\"",
        "-e",
        "activate",
        "-e",
        "if (count of windows) > 0 then set index of last window to 1",
        "-e",
        "end tell",
      ],
      { stdio: "ignore", timeout: kRaiseWindowTimeoutMs, killSignal: "SIGKILL" },
    );
  } catch (error) {
    const timedOut = error?.signal === "SIGKILL" || error?.code === "ETIMEDOUT";
    console.warn(
      `[safari-geode] skipped raising the Safari automation window (${
        timedOut
          ? `no response within ${kRaiseWindowTimeoutMs} ms, usually a missing Apple Events`
            + " permission for this terminal"
          : String(error?.message || error)
      }); continuing, the run's visibility requirement is enforced separately`,
    );
  }
}

class SafariDriverClient {
  constructor(baseUrl) {
    this.baseUrl = baseUrl.replace(/\/$/, "");
    this.sessionId = null;
  }

  async request(method, requestPath, body) {
    const response = await fetch(`${this.baseUrl}${requestPath}`, {
      method,
      headers: body === undefined ? undefined : { "content-type": "application/json" },
      body: body === undefined ? undefined : JSON.stringify(body),
    });
    const responseText = await response.text();
    let payload = {};
    try {
      payload = responseText ? JSON.parse(responseText) : {};
    } catch {
      throw new Error(`${method} ${requestPath} returned ${response.status}: ${responseText}`);
    }
    if (!response.ok || payload.value?.error) {
      throw new Error(
        `${method} ${requestPath} failed (${response.status}): ${JSON.stringify(payload)}`,
      );
    }
    return payload.value;
  }

  async createSession() {
    const created = await this.request("POST", "/session", {
      capabilities: {
        alwaysMatch: {
          browserName: "safari",
          "safari:diagnose": true,
        },
      },
    });
    this.sessionId = created.sessionId;
    return created.capabilities;
  }

  async execute(script, args = []) {
    return this.request("POST", `/session/${this.sessionId}/execute/sync`, { script, args });
  }

  async navigate(url) {
    await this.request("POST", `/session/${this.sessionId}/url`, { url });
  }

  async screenshot(filePath) {
    const base64 = await this.request("GET", `/session/${this.sessionId}/screenshot`);
    fs.writeFileSync(filePath, Buffer.from(base64, "base64"));
  }

  async actions(sequence) {
    await this.request("POST", `/session/${this.sessionId}/actions`, {
      actions: [{
        type: "pointer",
        id: "donner-safari-regression-mouse",
        parameters: { pointerType: "mouse" },
        actions: sequence,
      }],
    });
  }

  async scroll(x, y, deltaX, deltaY) {
    await this.request("POST", `/session/${this.sessionId}/actions`, {
      actions: [{
        type: "wheel",
        id: "donner-safari-regression-wheel",
        actions: [{
          type: "scroll",
          duration: 0,
          x: Math.round(x),
          y: Math.round(y),
          deltaX,
          deltaY,
          origin: "viewport",
        }],
      }],
    });
  }

  async releaseActions() {
    await this.request("DELETE", `/session/${this.sessionId}/actions`);
  }

  async close() {
    if (!this.sessionId) {
      return;
    }
    await this.request("DELETE", `/session/${this.sessionId}`).catch(() => {});
    this.sessionId = null;
  }
}

function pointerMove(x, y, duration = 0) {
  return {
    type: "pointerMove",
    duration,
    x: Math.round(x),
    y: Math.round(y),
    origin: "viewport",
  };
}

async function click(driver, x, y) {
  // SafariDriver 26 can update the pointer location used by pointerDown without dispatching the
  // pointerMove/mousemove pair that Emscripten uses to update ImGui's mouse position. Prime only
  // that position with a synthetic motion event, then require the actual activation events below
  // to remain trusted WebDriver input.
  await driver.execute(
    `
    const [x, y] = arguments;
    const target = document.elementFromPoint(x, y);
    target?.dispatchEvent(new MouseEvent('mousemove', {
      bubbles: true,
      cancelable: true,
      clientX: x,
      clientY: y,
      view: window,
    }));
    `,
    [Math.round(x), Math.round(y)],
  );
  await driver.actions([
    pointerMove(Math.max(0, x - 40), Math.max(0, y - 40), 40),
    pointerMove(x, y, 80),
    { type: "pause", duration: 50 },
    { type: "pointerDown", button: 0 },
    { type: "pause", duration: 50 },
    { type: "pointerUp", button: 0 },
    { type: "pause", duration: 50 },
  ]);
  await driver.releaseActions();
}

async function poll(label, callback, timeoutMs = kTimeoutMs, intervalMs = 50) {
  const deadline = Date.now() + timeoutMs;
  let lastValue;
  while (Date.now() < deadline) {
    lastValue = await callback();
    if (lastValue) {
      return lastValue;
    }
    await new Promise((resolve) => setTimeout(resolve, intervalMs));
  }
  throw new Error(`${label} timed out after ${timeoutMs}ms; last=${JSON.stringify(lastValue)}`);
}

function assertNoFatal(label, states) {
  for (const state of states) {
    assert.equal(
      Number(state.wgpuReadbackCaptureFailures || 0),
      0,
      `${label}: WGPU readback capture failed`,
    );
  }
  const serialized = JSON.stringify(states);
  assert.doesNotMatch(serialized, kFatalPattern, `${label}: fatal renderer error observed`);
}

function assertTrustedClickAtPoint(label, events, point) {
  const matches = (event, types) =>
    event.isTrusted === true
    && event.target === "canvas"
    && types.includes(event.type)
    && Math.abs(Number(event.clientX) - point.x) <= 1
    && Math.abs(Number(event.clientY) - point.y) <= 1;
  assert.ok(
    events.some((event) => matches(event, ["pointerdown", "mousedown"])),
    `${label}: no trusted down event reached the canvas at the expected point`,
  );
  assert.ok(
    events.some((event) => matches(event, ["pointerup", "mouseup"])),
    `${label}: no trusted up event reached the canvas at the expected point`,
  );
}

function assertFlatHeap(label, state, expectedBytes) {
  assert.equal(
    Number(state.heapBytes || 0),
    expectedBytes,
    `${label}: shared Wasm heap grew after pthread startup`,
  );
}

function assertThumbnailDiagnostic(state, initialDeviceState) {
  const thumbnails = state.thumbnailStats;
  assert.ok(thumbnails, "thumbnail statistics were not published");
  assert.deepEqual(
    {
      rendered: thumbnails.rendered,
      ready: thumbnails.ready,
      pending: thumbnails.pending,
      active: thumbnails.active,
      resultReady: thumbnails.resultReady,
    },
    {
      rendered: 5,
      ready: 5,
      pending: false,
      active: false,
      resultReady: false,
    },
    "five SVG thumbnails did not settle cleanly",
  );
  // A thumbnail whose catalog font is still loading completes as FontsPending and is requested
  // again once the font arrives (Text and Style waits for Inter), so attempts may exceed five, as
  // the Playwright smoke suite allows. Every attempt must still have started and completed.
  assert.ok(
    thumbnails.requested >= 5,
    `expected at least five thumbnail requests, got ${thumbnails.requested}`,
  );
  assert.equal(thumbnails.started, thumbnails.requested, "a thumbnail request never started");
  assert.equal(thumbnails.completed, thumbnails.started, "a thumbnail attempt never completed");
  assert.ok(
    thumbnails.firstRequestFrame > thumbnails.carouselFrame,
    "thumbnail rendering did not start asynchronously after the carousel frame",
  );
  assert.equal(thumbnails.publicationFrames?.length, 5, "expected five publication epochs");
  for (let index = 1; index < thumbnails.publicationFrames.length; ++index) {
    assert.ok(
      thumbnails.publicationFrames[index] > thumbnails.publicationFrames[index - 1],
      `thumbnail publication epoch ${index} did not advance`,
    );
  }
  assert.deepEqual(
    { headlessDeviceCreations: state.headlessDeviceCreations },
    initialDeviceState,
    "thumbnail generation unexpectedly created another WebGPU device",
  );

  // The diagnostic readback probes four fixed card regions of the final frame even though the
  // carousel shows five cards (the publication counts above cover all five), as the Playwright
  // smoke suite expects. The fourth probed card is Text and Style.
  const diagnostics = state.wgpuReadback?.carouselThumbnails;
  assert.equal(diagnostics?.length, 4, "expected final readback diagnostics for four thumbnails");
  const fingerprints = new Set();
  for (const [index, stats] of diagnostics.entries()) {
    assert.ok(stats.maxChannel > 80, `thumbnail ${index} had no visible source pixels`);
    assert.ok(stats.coloredPixels > 32, `thumbnail ${index} looked like a flat placeholder`);
    assert.ok(
      stats.coloredPixels < stats.samples - 32,
      `thumbnail ${index} lacked nonuniform source art`,
    );
    fingerprints.add(stats.fingerprint);
  }
  assert.equal(fingerprints.size, 4, "the four sampled thumbnail fingerprints were not distinct");
  assert.ok(diagnostics[3].backgroundPixels > 1000, "text thumbnail lacked its background");
  assert.ok(diagnostics[3].glyphPixels > 20, "text thumbnail lacked rendered glyphs");
}

async function installErrorCapture(driver) {
  await driver.execute(
    `
    const [pageLifetimeToken] = arguments;
    clearInterval(window.__donnerSafariRegressionTimer);
    if (window.__donnerSafariRegressionAnimationFrameRequest) {
      cancelAnimationFrame(window.__donnerSafariRegressionAnimationFrameRequest);
    }
    window.__donnerSafariRegressionErrors = [];
    window.__donnerSafariRegressionPageLifetimeToken = pageLifetimeToken;
    window.__donnerSafariRegressionAnimationFrames = 0;
    window.__donnerSafariRegressionTimerTicks = 0;
    window.__donnerSafariRegressionAnimationFrameRequest = requestAnimationFrame(() => {
      window.__donnerSafariRegressionAnimationFrames += 1;
      window.__donnerSafariRegressionAnimationFrameRequest = 0;
    });
    window.__donnerSafariRegressionTimer = setInterval(() => {
      window.__donnerSafariRegressionTimerTicks += 1;
    }, 100);
    const record = (kind, value) => {
      const text = value instanceof Error
        ? (value.stack || value.message || String(value))
        : String(value ?? '');
      window.__donnerSafariRegressionErrors.push({ kind, text, atMs: performance.now() });
    };
    window.addEventListener('error', (event) => {
      record(
        'error',
        (event.error?.stack || event.error || event.message)
          + ' at ' + event.filename + ':' + event.lineno + ':' + event.colno,
      );
    });
    window.addEventListener('unhandledrejection', (event) => {
      record('unhandledrejection', event.reason);
    });
    for (const level of ['error', 'warn']) {
      const original = console[level].bind(console);
      console[level] = (...args) => {
        record('console.' + level, args.map((arg) => {
          if (arg instanceof Error) return arg.stack || arg.message || String(arg);
          try { return typeof arg === 'string' ? arg : JSON.stringify(arg); }
          catch { return String(arg); }
        }).join(' '));
        return original(...args);
      };
    }
    return true;
  `,
    [kPageLifetimeToken],
  );
}

async function cleanupVisibleSafariAnimationFrameProbe(driver) {
  await driver.execute(`
    clearInterval(window.__donnerSafariRegressionTimer);
    window.__donnerSafariRegressionTimer = 0;
    if (window.__donnerSafariRegressionAnimationFrameRequest) {
      cancelAnimationFrame(window.__donnerSafariRegressionAnimationFrameRequest);
      window.__donnerSafariRegressionAnimationFrameRequest = 0;
    }
    return true;
  `);
}

async function readState(driver) {
  return driver.execute(`
    const canvas = document.getElementById('canvas');
    const loading = document.getElementById('loading-screen');
    const capability = document.getElementById('capability-error-detail');
    const rect = (element) => element ? (() => {
      const value = element.getBoundingClientRect();
      return { x: value.x, y: value.y, width: value.width, height: value.height };
    })() : null;
    return {
      activeSample: window.__donnerActiveSampleStats || null,
      backend: window.__donnerBackend || null,
      browserCursorStats: window.__donnerBrowserCursorStats || null,
      canStart: window.__donnerCanStartWasm,
      canvasRect: rect(canvas),
      capabilityError: capability?.textContent || '',
      devicePixelRatio: window.devicePixelRatio || 1,
      errors: window.__donnerSafariRegressionErrors || [],
      animationFrames: window.__donnerSafariRegressionAnimationFrames || 0,
      editorFrameRequested: window.__donnerEditorFrameRequested,
      firstFramePresented: Boolean(window.__donnerFirstFramePresented),
      frameSchedulingInitialized: Object.hasOwn(window, '__donnerEditorFrameRequested'),
      headlessDeviceCreations: window.__donnerHeadlessDeviceCreations || 0,
      heapBytes: window.HEAPU8?.length || window.Module?.wasmMemory?.buffer?.byteLength || 0,
      layerThumbnails: window.__donnerLayerThumbnailStats || null,
      loadingHidden: Boolean(loading?.hidden),
      mainLoopRenderedFrames: window.__donnerMainLoopRenderedFrames || 0,
      documentHasFocus: document.hasFocus(),
      timerTicks: window.__donnerSafariRegressionTimerTicks || 0,
      visibilityState: document.visibilityState,
      pageLifetimeToken: window.__donnerSafariRegressionPageLifetimeToken || null,
      pageTimeOrigin: performance.timeOrigin,
      presentationResources: window.__donnerPresentationResourceStats || null,
      runtimeInitializedAtMs: window.__donnerRuntimeInitializedAtMs || 0,
      canvasBacking: Array.from(document.querySelectorAll('canvas')).map((element) => ({
        backingBytes: Number(element.width || 0) * Number(element.height || 0) * 4,
        height: Number(element.height || 0),
        id: element.id || '',
        visible: getComputedStyle(element).display !== 'none'
          && getComputedStyle(element).visibility !== 'hidden',
        width: Number(element.width || 0),
      })),
      thumbnailStats: window.__donnerSampleThumbnailStats || null,
      wgpuReadback: window.__donnerWgpuReadbackStats || null,
      wgpuReadbackCaptureCompletions: window.__donnerWgpuReadbackCaptureCompletions || 0,
      wgpuReadbackCaptureFailures: window.__donnerWgpuReadbackCaptureFailures || 0,
      wgpuReadbackCaptureStarts: window.__donnerWgpuReadbackCaptureStarts || 0,
      wholeAppWorker: window.__donnerWholeAppWorker === true,
      viewport: window.__donnerViewportStats || null,
      worker: window.__donnerWorkerStats || null,
    };
  `);
}

async function waitForEditor(driver, label) {
  return poll(label, async () => {
    const state = await readState(driver);
    assertNoFatal(label, [state]);
    if (state.capabilityError) {
      throw new Error(`${label}: browser capability error: ${state.capabilityError}`);
    }
    return state.loadingHidden && state.firstFramePresented && state.canvasRect ? state : null;
  });
}

async function requireVisibleSafariAnimationFrame(driver) {
  try {
    return await poll("visible Safari automation window", async () => {
      const state = await readState(driver);
      assertNoFatal("visible Safari automation window", [state]);
      if (state.visibilityState === "visible" && state.animationFrames > 0) {
        return state;
      }
      if (state.timerTicks >= 3 && state.animationFrames === 0) {
        throw new SafariAutomationUnavailable(
          "Safari's automation document is hidden and requestAnimationFrame is suspended. "
            + "Wake an attached display and expose the Safari automation window before rerunning; "
            + "the editor intentionally does not render hidden frames.",
        );
      }
      return null;
    }, Math.min(kTimeoutMs, 5_000));
  } finally {
    await cleanupVisibleSafariAnimationFrameProbe(driver);
  }
}

// Waits for a new completed document render.
async function waitForDocumentRender(driver, label, minimumResults) {
  return poll(label, async () => {
    const state = await readState(driver);
    assertNoFatal(label, [state]);
    return Number(state.worker?.completedResults || 0) > minimumResults ? state : null;
  });
}

// An active drag, a resize at unchanged zoom, and other presentation-only changes draw UI frames
// that composite existing document textures without a new document render, so their signal is the
// app-thread frame counter.
async function waitForUiFrame(driver, label, minimumFrames, accept = () => true) {
  return poll(label, async () => {
    const state = await readState(driver);
    assertNoFatal(label, [state]);
    return Number(state.mainLoopRenderedFrames || 0) > minimumFrames && accept(state)
      ? state
      : null;
  });
}

async function readInteraction(driver) {
  return driver.execute(`
    return {
      completedResults: window.__donnerWorkerStats?.completedResults || 0,
      dragging: Boolean(window.__donnerInteractionStats?.dragging),
      moved: Boolean(window.__donnerInteractionStats?.moved),
      pendingClick: window.__donnerInteractionStats?.pendingClick ?? true,
      selectedCount: window.__donnerInteractionStats?.selectedCount ?? -1,
      workerBusy: window.__donnerInteractionStats?.workerBusy ?? true,
    };
  `);
}

// Waits until the worker is idle and its completed-result count has held for 250 ms, so a later
// step does not read a still-landing render (such as a selection prewarm) as its own.
async function waitForSettledWorker(driver, label, accept = () => true) {
  let lastCompleted = -1;
  let stableSince = Date.now();
  return poll(label, async () => {
    const interaction = await readInteraction(driver);
    const now = Date.now();
    if (interaction.workerBusy || interaction.completedResults !== lastCompleted) {
      lastCompleted = interaction.completedResults;
      stableSince = now;
      return null;
    }
    return now - stableSince >= 250 && accept(interaction) ? interaction : null;
  });
}

async function capture(driver, name) {
  const filePath = path.join(kArtifactDir, `${name}.png`);
  await driver.screenshot(filePath);
  return filePath;
}

// Clicks at `point`, then drags a short way from it, and waits for the document render the gesture
// causes. Requires the trusted down and up events to reach the canvas. Appends the captured input
// to `wakeInputs` before checking it, so a failed run keeps the evidence.
async function wakeGesture(driver, point, direction, label, initialHeapBytes, wakeInputs) {
  const inputBefore = await driver.execute(`
    return {
      eventCount: (window.__donnerSafariClickProbeEvents || []).length,
      mainLoopRenderedFrames: window.__donnerMainLoopRenderedFrames || 0,
    };
  `);
  await click(driver, point.x, point.y);
  await new Promise((resolve) => setTimeout(resolve, 20));
  const beforeResults = Number((await readState(driver)).worker?.completedResults || 0);
  await driver.actions([
    pointerMove(point.x, point.y, 40),
    { type: "pause", duration: 30 },
    { type: "pointerDown", button: 0 },
    { type: "pause", duration: 30 },
  ]);
  try {
    const moves = [];
    for (let step = 1; step <= 12; ++step) {
      moves.push(pointerMove(
        point.x + direction * step * 3,
        point.y + direction * step * 2,
        4,
      ));
    }
    await driver.actions(moves);
  } finally {
    await driver.actions([{ type: "pointerUp", button: 0 }]).catch(() => {});
    await driver.releaseActions().catch(() => {});
  }
  const wakeInput = await driver.execute(
    `
    const [eventOffset, beforeResults] = arguments;
    return {
      beforeResults,
      events: (window.__donnerSafariClickProbeEvents || []).slice(eventOffset),
      activeSample: window.__donnerActiveSampleStats || null,
      editorFrameRequested: Boolean(window.__donnerEditorFrameRequested),
      mainLoopRenderedFrames: window.__donnerMainLoopRenderedFrames || 0,
      worker: window.__donnerWorkerStats || null,
    };
  `,
    [inputBefore.eventCount, beforeResults],
  );
  wakeInputs.push(wakeInput);
  assertTrustedClickAtPoint(label, wakeInput.events, point);
  const after = await waitForDocumentRender(driver, label, beforeResults);
  assertFlatHeap(label, after, initialHeapBytes);
  return after;
}

async function runRegression(driver, editorUrl, result) {
  const initialWebContentPids = kMemoryOnly
    ? new Set(safariWebContentProcesses().map((process) => process.pid))
    : new Set();
  const capabilities = await driver.createSession();
  result.capabilities = capabilities;
  assert.match(
    String(capabilities.browserName),
    /safari/i,
    "SafariDriver opened a non-Safari browser",
  );

  await driver.request("POST", `/session/${driver.sessionId}/window/rect`, {
    width: 1600,
    height: 1000,
    x: 20,
    y: 20,
  }).catch((error) => {
    result.windowRectWarning = String(error);
  });
  await driver.navigate(editorUrl);
  bringSafariAutomationWindowForward();
  await installErrorCapture(driver);
  result.visibleAutomation = await requireVisibleSafariAnimationFrame(driver);

  result.initial = await waitForEditor(driver, "initial Safari editor startup");
  assert.equal(result.initial.backend, "geode", "package was not Geode");
  assert.equal(
    result.initial.wholeAppWorker,
    true,
    "Safari did not start the whole application on the app pthread",
  );
  assert.ok(
    result.initial.headlessDeviceCreations >= 1,
    "the app pthread created no WebGPU device",
  );
  const initialHeapBytes = Number(result.initial.heapBytes || 0);
  assert.ok(initialHeapBytes >= 64 * 1024 * 1024, "Geode started below the 64 MiB heap fence");
  result.initialScreenshot = await capture(driver, "01-initial-carousel");

  const initialDeviceState = {
    headlessDeviceCreations: result.initial.headlessDeviceCreations,
  };
  result.thumbnailsSettled = await poll(
    "asynchronous SVG thumbnails",
    async () => {
      const state = await readState(driver);
      assertNoFatal("asynchronous SVG thumbnails", [state]);
      return state.thumbnailStats?.ready === 5 ? state : null;
    },
    Math.max(kTimeoutMs, 20_000),
    25,
  );
  result.thumbnailsScreenshot = await capture(driver, "02-thumbnails-settled");
  assertFlatHeap("asynchronous SVG thumbnails", result.thumbnailsSettled, initialHeapBytes);

  if (kMemoryOnly) {
    result.thumbnailDiagnostic = result.thumbnailsSettled;
  } else {
    result.thumbnailDiagnosticRequest = await driver.execute(`
      if (!window.__donnerRequestWgpuReadback) {
        throw new Error('WGPU diagnostic hook is unavailable; serve with wgpuReadbackStats=1');
      }
      return window.__donnerRequestWgpuReadback();
    `);
    result.thumbnailDiagnostic = await poll(
      "thumbnail WGPU diagnostic readback",
      async () => {
        const state = await readState(driver);
        assertNoFatal("thumbnail WGPU diagnostic readback", [state]);
        return Number(state.wgpuReadback?.request || 0) >= result.thumbnailDiagnosticRequest
          ? state
          : null;
      },
      5_000,
      25,
    );
    assertThumbnailDiagnostic(result.thumbnailDiagnostic, initialDeviceState);
    assertFlatHeap(
      "thumbnail WGPU diagnostic readback",
      result.thumbnailDiagnostic,
      initialHeapBytes,
    );
  }

  const canvas = result.thumbnailDiagnostic.canvasRect;
  const beforeSampleResults = Number(result.thumbnailDiagnostic.worker?.completedResults || 0);
  const sample = kMemoryOnly
    ? { id: "donner-splash", label: "Donner Splash", xFraction: 0.24 }
    : { id: "basic-shapes", label: "Basic Shapes", xFraction: 0.76 };
  const sampleClickPoint = {
    x: canvas.x + canvas.width * sample.xFraction,
    y: canvas.y + 282,
  };
  result.sampleClickProbe = await driver.execute(
    `
    const [x, y] = arguments;
    const describe = (element) => element ? {
      id: element.id || '',
      tagName: element.tagName || '',
      pointerEvents: getComputedStyle(element).pointerEvents,
      rect: (() => {
        const value = element.getBoundingClientRect();
        return { x: value.x, y: value.y, width: value.width, height: value.height };
      })(),
    } : null;
    window.__donnerSafariClickProbeEvents = [];
    const record = (event) => {
      window.__donnerSafariClickProbeEvents.push({
        type: event.type,
        isTrusted: event.isTrusted,
        target: event.target?.id || event.target?.tagName || '',
        clientX: event.clientX,
        clientY: event.clientY,
        button: event.button,
        buttons: event.buttons,
        timeStamp: event.timeStamp,
      });
    };
    for (const type of [
      'pointermove', 'mousemove', 'pointerdown', 'mousedown', 'pointerup', 'mouseup', 'click'
    ]) {
      window.addEventListener(type, record, { capture: true, once: false });
    }
    return {
      point: { x, y },
      devicePixelRatio,
      hitTarget: describe(document.elementFromPoint(x, y)),
      canvas: describe(document.getElementById('canvas')),
      mainLoopRenderedFrames: window.__donnerMainLoopRenderedFrames || 0,
      editorFrameRequested: Boolean(window.__donnerEditorFrameRequested),
    };
  `,
    [sampleClickPoint.x, sampleClickPoint.y],
  );
  await click(driver, sampleClickPoint.x, sampleClickPoint.y);
  await new Promise((resolve) => setTimeout(resolve, 250));
  result.sampleClickProbe.after = await driver.execute(`
    return {
      events: window.__donnerSafariClickProbeEvents || [],
      activeSample: window.__donnerActiveSampleStats || null,
      mainLoopRenderedFrames: window.__donnerMainLoopRenderedFrames || 0,
      editorFrameRequested: Boolean(window.__donnerEditorFrameRequested),
      worker: window.__donnerWorkerStats || null,
    };
  `);
  assertTrustedClickAtPoint(
    `${sample.label} click`,
    result.sampleClickProbe.after.events,
    sampleClickPoint,
  );
  result.selectedSample = await poll(`${sample.label} sample presentation`, async () => {
    const state = await readState(driver);
    assertNoFatal(`${sample.label} sample presentation`, [state]);
    if (
      state.activeSample?.sampleId === sample.id
      && Number(state.worker?.completedResults || 0) > beforeSampleResults
      && Number(state.mainLoopRenderedFrames || 0) > 0
    ) {
      return state;
    }
    return null;
  });
  assertFlatHeap(sample.label, result.selectedSample, initialHeapBytes);
  result.selectedSampleScreenshot = await capture(driver, `03-${sample.id}`);

  if (kMemoryOnly) {
    let previousResourceSignature = "";
    let stableResourceSamples = 0;
    result.memorySettle = [];
    result.memoryBaseline = await poll(
      "Splash layer thumbnails and presentation resources",
      async () => {
        const state = await readState(driver);
        assertNoFatal("Splash layer thumbnails and presentation resources", [state]);
        const layers = state.layerThumbnails || {};
        const resources = state.presentationResources || {};
        const signature = JSON.stringify({
          bitmapBytes: Number(layers.bitmapBytes || 0),
          bitmapCount: Number(layers.bitmapCount || 0),
          textureSnapshotCount: Number(layers.textureSnapshotCount || 0),
          lifetimeBufferCreates: Number(resources.lifetimeBufferCreates || 0),
          lifetimeTextureCreates: Number(resources.lifetimeTextureCreates || 0),
          textureCount: Number(layers.textureCount || 0),
          totalTrackedBytes: Number(resources.totalTrackedBytes || 0),
          workerResults: Number(state.worker?.completedResults || 0),
        });
        result.memorySettle.push({
          deferredCount: Number(layers.deferredCount || 0),
          sampledAtMs: Date.now(),
          signature,
        });
        // Layer thumbnails are GPU texture snapshots; bitmaps remain only as a fallback.
        const thumbnailsReady = Number(layers.rowCount || 0) > 0
          && Number(layers.deferredCount || 0) === 0
          && (Number(layers.textureSnapshotCount || 0) > 0 || Number(layers.bitmapCount || 0) > 0);
        if (!thumbnailsReady) {
          previousResourceSignature = "";
          stableResourceSamples = 0;
          return null;
        }
        if (signature === previousResourceSignature) {
          stableResourceSamples += 1;
        } else {
          previousResourceSignature = signature;
          stableResourceSamples = 1;
        }
        return stableResourceSamples >= 4 ? state : null;
      },
      Math.max(kTimeoutMs, 45_000),
      500,
    );
    result.memoryBaselineScreenshot = await capture(driver, "03b-memory-baseline");
    result.memoryWebContentProcess = await poll(
      "Safari automation WebContent process",
      async () => {
        const candidates = safariWebContentProcesses()
          .filter((process) => !initialWebContentPids.has(process.pid))
          .sort((left, right) => right.rssBytes - left.rssBytes);
        return candidates[0] || null;
      },
      5_000,
      100,
    );
    // WebContent RSS stays well above its steady state for a while after the editor loads (about
    // 750 MB, falling to about 100 MB within 30 s on Safari 26), so the ceiling applies from the
    // first sample under it through the whole dwell. The highest RSS seen before that sample is
    // recorded for diagnosis only; sampling starts after the baseline, so it is not the load peak.
    result.memoryWebContentPostBaselinePeakRssBytes = result.memoryWebContentProcess.rssBytes;
    let lastWebContentRssBytes = result.memoryWebContentProcess.rssBytes;
    try {
      result.memoryWebContentSettled = await poll(
        "Safari WebContent RSS falling under the ceiling",
        async () => {
          const webContentSample = safariWebContentProcesses()
            .find((process) => process.pid === result.memoryWebContentProcess.pid);
          assert.ok(webContentSample, "Safari WebContent process exited before the memory dwell");
          lastWebContentRssBytes = webContentSample.rssBytes;
          result.memoryWebContentPostBaselinePeakRssBytes = Math.max(
            result.memoryWebContentPostBaselinePeakRssBytes,
            webContentSample.rssBytes,
          );
          return webContentSample.rssBytes <= kMemoryMaxWebContentRssBytes
            ? webContentSample
            : null;
        },
        60_000,
        1_000,
      );
    } catch (error) {
      if (error instanceof assert.AssertionError) {
        throw error;
      }
      throw new Error(
        `Safari WebContent RSS stayed above ${kMemoryMaxWebContentRssBytes} bytes for 60 s; `
          + `last ${lastWebContentRssBytes} bytes`,
        { cause: error },
      );
    }
    result.memoryWebContentSamples = [result.memoryWebContentSettled];
    assert.equal(
      result.memoryBaseline.pageLifetimeToken,
      kPageLifetimeToken,
      "Safari memory baseline did not preserve the harness page lifetime token",
    );
    const settledWorkerResults = Number(result.memoryBaseline.worker?.completedResults || 0);
    const settledTextureCreates = Number(
      result.memoryBaseline.presentationResources?.lifetimeTextureCreates || 0,
    );
    const settledBufferCreates = Number(
      result.memoryBaseline.presentationResources?.lifetimeBufferCreates || 0,
    );
    const settledTrackedBytes = Number(
      result.memoryBaseline.presentationResources?.totalTrackedBytes || 0,
    );
    await driver.execute(`
      clearInterval(window.__donnerSafariMemoryTimer);
      window.__donnerSafariMemorySamples = [];
      const capture = () => {
        const resources = window.__donnerPresentationResourceStats || {};
        window.__donnerSafariMemorySamples.push({
          heapBytes: window.HEAPU8?.length || window.Module?.wasmMemory?.buffer?.byteLength || 0,
          mainLoopRenderedFrames: Number(window.__donnerMainLoopRenderedFrames || 0),
          pageTimeOrigin: performance.timeOrigin,
          resourceStats: { ...resources },
          sampledAtMs: performance.now(),
          workerResults: Number(window.__donnerWorkerStats?.completedResults || 0),
        });
      };
      capture();
      window.__donnerSafariMemoryTimer = setInterval(capture, ${kMemorySampleIntervalMs});
      return true;
    `);
    const memoryDeadline = Date.now() + kMemoryDwellMs;
    while (Date.now() < memoryDeadline) {
      await new Promise((resolve) =>
        setTimeout(resolve, Math.min(kMemorySampleIntervalMs, memoryDeadline - Date.now()))
      );
      const webContentSample = safariWebContentProcesses()
        .find((process) => process.pid === result.memoryWebContentProcess.pid);
      assert.ok(webContentSample, "Safari WebContent process exited during the memory dwell");
      result.memoryWebContentSamples.push(webContentSample);
      assert.ok(
        webContentSample.rssBytes <= kMemoryMaxWebContentRssBytes,
        `Safari WebContent RSS ${webContentSample.rssBytes} exceeded `
          + `${kMemoryMaxWebContentRssBytes} bytes during the memory dwell`,
      );
    }
    // The absolute ceiling is several times the steady state, so also reject growth across the
    // dwell: the final sample must stay within 128 MiB of the lowest one.
    const dwellRss = result.memoryWebContentSamples.map((sample) => sample.rssBytes);
    assert.ok(
      dwellRss.at(-1) <= Math.min(...dwellRss) + 128 * 1024 * 1024,
      `Safari WebContent RSS grew during the memory dwell: lowest ${Math.min(...dwellRss)}, `
        + `final ${dwellRss.at(-1)} bytes`,
    );
    const finalState = await readState(driver);
    result.memoryDwell = await driver.execute(`
      clearInterval(window.__donnerSafariMemoryTimer);
      window.__donnerSafariMemoryTimer = 0;
      return window.__donnerSafariMemorySamples || [];
    `);
    assertNoFatal("Safari memory dwell", [finalState]);
    assert.equal(
      finalState.pageLifetimeToken,
      kPageLifetimeToken,
      "Safari reloaded the editor during the memory dwell",
    );
    assert.equal(
      finalState.activeSample?.sampleId,
      sample.id,
      "Safari returned to the picker during the memory dwell",
    );
    assertFlatHeap("Safari memory dwell", finalState, initialHeapBytes);
    // Safari can stretch an idle page's 5 s interval timer to about 6 s, so require samples across
    // the whole dwell rather than an exact count.
    const sampleTimes = result.memoryDwell.map((sample) => Number(sample.sampledAtMs || 0));
    assert.ok(
      sampleTimes.length >= 2
        && sampleTimes.at(-1) - sampleTimes[0] >= kMemoryDwellMs - 2 * kMemorySampleIntervalMs,
      "Safari memory timer stopped during the dwell",
    );
    const sampleGaps = sampleTimes.slice(1).map((time, index) => time - sampleTimes[index]);
    assert.ok(
      Math.max(...sampleGaps) <= 3 * kMemorySampleIntervalMs,
      "Safari memory timer stalled during the dwell",
    );
    assert.equal(
      Number(finalState.worker?.completedResults || 0),
      settledWorkerResults,
      "Safari kept producing document results while the Splash was idle",
    );
    assert.equal(
      Number(finalState.presentationResources?.lifetimeTextureCreates || 0),
      settledTextureCreates,
      "Safari kept allocating WebGPU textures while the Splash was idle",
    );
    assert.equal(
      Number(finalState.presentationResources?.lifetimeBufferCreates || 0),
      settledBufferCreates,
      "Safari kept allocating WebGPU buffers while the Splash was idle",
    );
    assert.equal(
      Number(finalState.presentationResources?.totalTrackedBytes || 0),
      settledTrackedBytes,
      "Safari presentation resources grew while the Splash was idle",
    );
    result.memoryDwellScreenshot = await capture(driver, "04-memory-dwell");
    return;
  }

  result.basicShapes = result.selectedSample;
  result.basicShapesScreenshot = result.selectedSampleScreenshot;

  const cursorProbePoint = {
    x: canvas.x + canvas.width * 0.55,
    y: canvas.y + canvas.height * 0.72,
  };
  await driver.actions([
    pointerMove(cursorProbePoint.x, cursorProbePoint.y, 80),
    { type: "pause", duration: 50 },
  ]);
  await driver.releaseActions();
  result.browserCursor = await poll("browser-native SVG cursor", async () => {
    const state = await driver.execute(`
      const canvas = Module['canvas'];
      return {
        computedCursor: getComputedStyle(canvas).cursor,
        inlineCursor: canvas.style.getPropertyValue('cursor'),
        mainLoopRenderedFrames: window.__donnerMainLoopRenderedFrames || 0,
        stats: window.__donnerBrowserCursorStats || null,
      };
    `);
    return state.stats?.lastKey === "0:0"
        && state.computedCursor.includes("data:image/svg+xml;base64,")
      ? state
      : null;
  });
  assert.equal(result.browserCursor.stats.registered, 16, "browser cursor set was incomplete");
  assert.equal(
    result.browserCursor.stats.svgSupported,
    16,
    "Safari rejected one or more embedded SVG cursors",
  );
  assert.deepEqual(result.browserCursor.stats.lastHotspot, [5, 4], "select hotspot changed");
  assert.equal(result.browserCursor.stats.lastFallback, "default", "select fallback changed");
  assert.equal(result.browserCursor.stats.lastSvgSupported, true, "select SVG cursor fell back");
  assert.match(
    result.browserCursor.computedCursor,
    /data:image\/svg\+xml;base64,[^)]+\)\s+5\s+4,\s*default$/,
    "Safari computed cursor lost its SVG source, hotspot, or semantic fallback",
  );

  result.browserCursorIdleFrames = [];
  const cursorDomMutations = Number(result.browserCursor.stats.domMutations || 0);
  const cursorSkips = Number(result.browserCursor.stats.redundantApplySkips || 0);
  for (let iteration = 0; iteration < 3; ++iteration) {
    const beforeFrame = Number((await readState(driver)).mainLoopRenderedFrames || 0);
    await driver.execute(`window.__donnerEditorFrameRequested = true; return true;`);
    const state = await poll(`idle cursor frame ${iteration + 1}`, async () => {
      const candidate = await readState(driver);
      return Number(candidate.mainLoopRenderedFrames || 0) > beforeFrame ? candidate : null;
    });
    assertFlatHeap(`idle cursor frame ${iteration + 1}`, state, initialHeapBytes);
    result.browserCursorIdleFrames.push({
      domMutations: Number(state.browserCursorStats?.domMutations || 0),
      frame: Number(state.mainLoopRenderedFrames || 0),
      redundantApplySkips: Number(state.browserCursorStats?.redundantApplySkips || 0),
    });
  }
  assert.ok(
    result.browserCursorIdleFrames.every((state) => state.domMutations === cursorDomMutations),
    "same browser cursor caused a redundant canvas style mutation",
  );
  assert.ok(
    result.browserCursorIdleFrames.at(-1).redundantApplySkips >= cursorSkips + 3,
    "same-key browser cursor requests did not use the no-DOM-mutation fast path",
  );

  const dragStart = {
    x: result.basicShapes.canvasRect.x + kBlueRectOffset.x,
    y: result.basicShapes.canvasRect.y + kBlueRectOffset.y,
  };
  await click(driver, dragStart.x, dragStart.y);
  // An active drag presents by transforming the prewarmed selected-layer texture inside UI frames;
  // the worker does not re-rasterize the document until the pointer releases, as the Playwright
  // smoke suite's retained-drag test also asserts. The click selects the shape and schedules one
  // prewarm render of its layer, so let that land before pressing. Pressing the selected shape
  // starts a redrag without a render, and each step must then draw a UI frame with the document
  // count unchanged.
  result.dragPressReady = await waitForSettledWorker(
    driver,
    "Safari drag press readiness",
    (interaction) => interaction.selectedCount === 1 && !interaction.pendingClick,
  );
  await driver.actions([pointerMove(dragStart.x, dragStart.y), { type: "pointerDown", button: 0 }]);
  result.dragFrames = [];
  try {
    const pressed = await waitForSettledWorker(driver, "Safari drag press");
    assert.equal(
      pressed.completedResults,
      result.dragPressReady.completedResults,
      "pressing the selected shape re-rasterized the document",
    );
    const resultsDuringDrag = pressed.completedResults;
    result.dragResultsDuringDrag = resultsDuringDrag;
    let previousFrames = Number((await readState(driver)).mainLoopRenderedFrames || 0);
    for (let step = 1; step <= 16; ++step) {
      await driver.actions([pointerMove(dragStart.x + step * 6, dragStart.y + step * 3, 8)]);
      const state = await waitForUiFrame(driver, `Safari drag frame ${step}`, previousFrames);
      assertFlatHeap(`Safari drag frame ${step}`, state, initialHeapBytes);
      const interaction = await readInteraction(driver);
      assert.ok(interaction.dragging, `Safari drag step ${step} is not an active drag`);
      assert.equal(
        Number(state.worker?.completedResults || 0),
        resultsDuringDrag,
        `Safari drag step ${step} re-rasterized the document before the release`,
      );
      previousFrames = Number(state.mainLoopRenderedFrames || 0);
      if (step >= 2) {
        assert.ok(interaction.moved, `Safari drag step ${step} did not move the selected shape`);
      }
      result.dragFrames.push({
        completedResults: Number(state.worker?.completedResults || 0),
        heapBytes: state.heapBytes,
        mainLoopRenderedFrames: previousFrames,
        moved: interaction.moved,
      });
      if (step % 4 === 0) {
        await capture(driver, `04-drag-${String(step).padStart(2, "0")}`);
      }
    }
    const beforeRelease = await readInteraction(driver);
    assert.ok(
      beforeRelease.dragging && beforeRelease.moved,
      "the Safari drag was not active and moved before the release",
    );
    assert.equal(
      beforeRelease.completedResults,
      resultsDuringDrag,
      "the Safari drag re-rasterized the document before the release",
    );
  } finally {
    await driver.actions([{ type: "pointerUp", button: 0 }]).catch(() => {});
    await driver.releaseActions().catch(() => {});
  }
  result.dragRelease = await waitForDocumentRender(
    driver,
    "Safari drag release render",
    result.dragResultsDuringDrag,
  );
  assertFlatHeap("Safari drag release render", result.dragRelease, initialHeapBytes);
  assertNoFatal("Safari drag release render", [result.dragRelease]);
  // Release queues a settle render and a selection refresh; the worker must then go idle.
  result.dragReleaseSettled = await waitForSettledWorker(driver, "Safari drag release settle");

  result.wakeBurst = [];
  result.wakeInput = [];
  let wakePoint = { x: dragStart.x + 96, y: dragStart.y + 48 };
  for (let iteration = 0; iteration < 4; ++iteration) {
    const direction = iteration % 2 === 0 ? 1 : -1;
    const after = await wakeGesture(
      driver,
      wakePoint,
      direction,
      `renderer pthread wake burst ${iteration + 1}`,
      initialHeapBytes,
      result.wakeInput,
    );
    result.wakeBurst.push({
      completedResults: Number(after.worker?.completedResults || 0),
      heapBytes: after.heapBytes,
      mainLoopRenderedFrames: Number(after.mainLoopRenderedFrames || 0),
    });
    wakePoint = {
      x: wakePoint.x + direction * 36,
      y: wakePoint.y + direction * 24,
    };
  }
  result.afterWakeBurst = await readState(driver);
  assertNoFatal("renderer pthread wake burst", [result.afterWakeBurst]);
  assertFlatHeap("renderer pthread wake burst", result.afterWakeBurst, initialHeapBytes);
  result.afterWakeBurstScreenshot = await capture(driver, "05-after-wake-burst");

  // Repeated viewport-size churn used to retain every superseded Geode primary target until
  // Safari's JavaScript/GPU garbage collection caught up, eventually triggering the browser's
  // significant-memory reload. Exercise enough distinct targets to cross the old unbounded-growth
  // shape while proving presentation, the Wasm heap, the page lifetime and, at the end, the render
  // worker remain healthy.
  result.resizeChurn = [];
  // A resize at unchanged zoom recomposites existing document tiles in UI frames and need not start
  // a document render. Each target therefore waits for a UI frame drawn after the resize request in
  // which the editor's own published pane size has changed; the editor publishes that size from
  // inside the frame, in order with the frame counter.
  const paneSize = (state) => ({
    height: Number(state.viewport?.paneHeight || 0),
    width: Number(state.viewport?.paneWidth || 0),
  });
  const paneResizedFrom = (previous) => (state) => {
    const current = paneSize(state);
    return current.width > 0
      && (current.width !== previous.width || current.height !== previous.height);
  };
  let resizePane = paneSize(result.afterWakeBurst);
  const initialPageLifetimeToken = result.initial.pageLifetimeToken;
  assert.equal(
    initialPageLifetimeToken,
    kPageLifetimeToken,
    "Safari did not preserve the harness page lifetime token",
  );
  for (let iteration = 0; iteration < 24; ++iteration) {
    const width = 1400 + (iteration % 12) * 13;
    const height = 850 + Math.floor(iteration / 12) * 37 + (iteration % 3) * 7;
    const resizeFrames = Number((await readState(driver)).mainLoopRenderedFrames || 0);
    await driver.request("POST", `/session/${driver.sessionId}/window/rect`, {
      width,
      height,
      x: 20,
      y: 20,
    });
    const state = await waitForUiFrame(
      driver,
      `Safari primary-target resize churn ${iteration + 1}`,
      resizeFrames,
      paneResizedFrom(resizePane),
    );
    assertFlatHeap(`Safari primary-target resize churn ${iteration + 1}`, state, initialHeapBytes);
    assert.equal(
      state.pageLifetimeToken,
      initialPageLifetimeToken,
      `Safari reloaded the page under resize memory pressure at iteration ${iteration + 1}`,
    );
    resizePane = paneSize(state);
    result.resizeChurn.push({
      pane: resizePane,
      completedResults: Number(state.worker?.completedResults || 0),
      heapBytes: state.heapBytes,
      height,
      mainLoopRenderedFrames: Number(state.mainLoopRenderedFrames || 0),
      width,
    });
  }
  const restoreFrames = Number((await readState(driver)).mainLoopRenderedFrames || 0);
  await driver.request("POST", `/session/${driver.sessionId}/window/rect`, {
    width: 1600,
    height: 1000,
    x: 20,
    y: 20,
  });
  result.afterResizeChurn = await waitForUiFrame(
    driver,
    "Safari resize-churn restore",
    restoreFrames,
    paneResizedFrom(resizePane),
  );
  assertFlatHeap("Safari resize-churn restore", result.afterResizeChurn, initialHeapBytes);
  assert.equal(
    result.afterResizeChurn.pageLifetimeToken,
    initialPageLifetimeToken,
    "Safari reloaded the page while restoring the viewport after resize churn",
  );
  result.afterResizeChurnScreenshot = await capture(driver, "06-after-resize-churn");
  // UI frames keep flowing even if the render worker wedged after a surface reconfigure, so finish
  // the churn by requiring the worker to go idle and then render the document for one gesture.
  result.afterResizeChurnSettled = await waitForSettledWorker(
    driver,
    "Safari resize-churn worker settle",
  );
  result.afterResizeChurnWakeInput = [];
  result.afterResizeChurnRender = await wakeGesture(
    driver,
    wakePoint,
    1,
    "Safari resize-churn document render",
    initialHeapBytes,
    result.afterResizeChurnWakeInput,
  );

  await driver.request("POST", `/session/${driver.sessionId}/refresh`, {});
  await installErrorCapture(driver);
  result.visibleAutomationAfterReload = await requireVisibleSafariAnimationFrame(driver);
  result.afterReload = await waitForEditor(driver, "Safari reload after renderer teardown");
  assert.equal(
    result.afterReload.wholeAppWorker,
    true,
    "reload did not restart the whole application on the app pthread",
  );
  assert.ok(
    result.afterReload.headlessDeviceCreations >= 1,
    "reload created no WebGPU device",
  );
  assertNoFatal("Safari reload after renderer teardown", [result.afterReload]);
  assertFlatHeap("Safari reload after renderer teardown", result.afterReload, initialHeapBytes);
  result.afterReloadSettled = await poll(
    "Safari reload thumbnails",
    async () => {
      const state = await readState(driver);
      assertNoFatal("Safari reload thumbnails", [state]);
      assertFlatHeap("Safari reload thumbnails", state, initialHeapBytes);
      return state.thumbnailStats?.ready === 5 ? state : null;
    },
    Math.max(kTimeoutMs, 20_000),
    25,
  );
  result.afterReloadScreenshot = await capture(driver, "07-after-reload");

  result.maxObservedHeapBytes = Math.max(
    result.initial.heapBytes,
    result.thumbnailsSettled.heapBytes,
    result.thumbnailDiagnostic.heapBytes,
    result.basicShapes.heapBytes,
    ...result.dragFrames.map((sample) => sample.heapBytes),
    ...result.wakeBurst.map((sample) => sample.heapBytes),
    ...result.resizeChurn.map((sample) => sample.heapBytes),
    result.afterWakeBurst.heapBytes,
    result.afterResizeChurn.heapBytes,
    result.afterReload.heapBytes,
    result.afterReloadSettled.heapBytes,
  );

  await driver.navigate("about:blank");
  await new Promise((resolve) => setTimeout(resolve, 250));
  result.teardownUrl = await driver.request("GET", `/session/${driver.sessionId}/url`);
  assert.equal(result.teardownUrl, "about:blank", "Safari session failed during worker teardown");
}

async function checkHttpEndpoint(url, label) {
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 3_000);
  try {
    const response = await fetch(url, { signal: controller.signal });
    if (!response.ok) {
      throw new Error(`${label} returned HTTP ${response.status}`);
    }
    return response;
  } finally {
    clearTimeout(timeout);
  }
}

async function artifactEvidence(url) {
  const response = await checkHttpEndpoint(url, `package artifact ${url}`);
  const bytes = Buffer.from(await response.arrayBuffer());
  return {
    bytes: bytes.length,
    sha256: crypto.createHash("sha256").update(bytes).digest("hex"),
    url,
  };
}

async function main() {
  fs.mkdirSync(kArtifactDir, { recursive: true });
  const editorUrl = new URL(kBaseUrl);
  editorUrl.searchParams.set("safari-geode-regression", "1");
  if (!kMemoryOnly) {
    editorUrl.searchParams.set("wgpuReadbackStats", "1");
  }
  const result = {
    artifactDir: kArtifactDir,
    driverUrl: kDriverUrl,
    editorUrl: editorUrl.href,
    scope: kMemoryOnly ? "significant-memory" : "full",
    startedAt: new Date().toISOString(),
  };
  const driver = new SafariDriverClient(kDriverUrl);

  try {
    if (process.platform !== "darwin") {
      throw new SafariAutomationUnavailable(`Apple Safari requires macOS, got ${process.platform}`);
    }
    try {
      await checkHttpEndpoint(`${kDriverUrl.replace(/\/$/, "")}/status`, "SafariDriver");
    } catch (error) {
      throw new SafariAutomationUnavailable(
        `SafariDriver is unavailable at ${kDriverUrl}: ${error.message}. `
          + "Start it with `safaridriver -p 4445` and enable Safari remote automation.",
      );
    }
    const expectedWasmSha256 = expectedWasmSha256ForRun();
    result.expectedWasmSha256 = expectedWasmSha256;
    result.packageIdentityMode = expectedWasmSha256 === null ? "exploratory-unpinned" : "pinned";
    await checkHttpEndpoint(editorUrl.href, "served Donner package");
    result.packageArtifacts = {
      javascript: await artifactEvidence(new URL("editor.js", editorUrl).href),
      wasm: await artifactEvidence(new URL("editor.wasm", editorUrl).href),
    };
    if (expectedWasmSha256 !== null) {
      assert.equal(
        result.packageArtifacts.wasm.sha256,
        expectedWasmSha256,
        "served transitioned editor.wasm did not match DONNER_SAFARI_EXPECTED_WASM_SHA256",
      );
    }
    try {
      await runRegression(driver, editorUrl.href, result);
    } catch (error) {
      if (
        !driver.sessionId && /POST \/session|session not created|automation/i.test(error.message)
      ) {
        throw new SafariAutomationUnavailable(
          `Safari automation session unavailable: ${error.message}`,
        );
      }
      throw error;
    }
    result.completedAt = new Date().toISOString();
    result.passed = true;
    fs.writeFileSync(path.join(kArtifactDir, "result.json"), JSON.stringify(result, null, 2));
    const passLabel = kMemoryOnly
      ? "real Apple Safari five-minute Splash memory dwell"
      : "real Apple Safari Geode startup, thumbnails, drag, wake burst, and teardown";
    console.log(`PASS: ${passLabel}; artifacts=${kArtifactDir}`);
  } catch (error) {
    if (error instanceof SafariAutomationUnavailable && !kRequired) {
      result.completedAt = new Date().toISOString();
      result.passed = false;
      result.skipped = true;
      result.skipReason = error.message;
      fs.writeFileSync(path.join(kArtifactDir, "result.json"), JSON.stringify(result, null, 2));
      console.log(`SKIP: ${error.message} Set DONNER_SAFARI_REQUIRED=1 to make this fatal.`);
      return;
    }
    result.completedAt = new Date().toISOString();
    result.failure = error.stack || String(error);
    result.lastState = driver.sessionId
      ? await readState(driver).catch((stateError) => ({ error: String(stateError) }))
      : null;
    result.failureScreenshot = driver.sessionId
      ? path.join(kArtifactDir, "99-failure.png")
      : null;
    if (result.failureScreenshot) {
      await driver.screenshot(result.failureScreenshot).catch(() => {
        result.failureScreenshot = null;
      });
    }
    result.passed = false;
    fs.writeFileSync(path.join(kArtifactDir, "result.json"), JSON.stringify(result, null, 2));
    console.error(`FAIL: real Apple Safari Geode regression: ${error.stack || error}`);
    console.error(`Artifacts: ${kArtifactDir}`);
    process.exitCode = 1;
  } finally {
    await driver.close();
  }
}

await main();
