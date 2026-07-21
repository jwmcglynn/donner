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
 *   DONNER_SAFARI_SEAM_ONLY=1     Stop after the fractional-pan compositor pixel probe.
 *   DONNER_SAFARI_LIVE_PAN_ONLY=1 Capture trusted wheel-pan frames and fail on any edge leak.
 */

import assert from "node:assert/strict";
import crypto from "node:crypto";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import zlib from "node:zlib";

const kDriverUrl = process.env.DONNER_SAFARI_DRIVER_URL || "http://127.0.0.1:4445";
const kBaseUrl = process.env.DONNER_WASM_BASE_URL || "http://127.0.0.1:8000";
const kRequired = process.env.DONNER_SAFARI_REQUIRED === "1";
const kExpectedWasmSha256 = process.env.DONNER_SAFARI_EXPECTED_WASM_SHA256 || "";
const kAllowUnpinnedPackage = process.env.DONNER_SAFARI_ALLOW_UNPINNED_PACKAGE === "1";
const kSeamOnly = process.env.DONNER_SAFARI_SEAM_ONLY === "1";
const kLivePanOnly = process.env.DONNER_SAFARI_LIVE_PAN_ONLY === "1";
const kTimeoutMs = Number(process.env.DONNER_SAFARI_TIMEOUT_MS || 30_000);
const kRunId = new Date().toISOString().replaceAll(/[^0-9A-Za-z]/g, "");
const kArtifactDir = process.env.DONNER_SAFARI_ARTIFACT_DIR
  || path.join(os.tmpdir(), `donner-safari-geode-${kRunId}`);
const kFatalPattern =
  /Failed to wake Wasm renderer pthread|Wasm renderer pthread wake rejected|Aborted|Assertion failed|RuntimeError|Out of bounds|call_indirect|Unreachable code|UTILS_RELEASE_ASSERT|Pthread .* sent an error|getJsObject|No available adapters|WebGPU adapter (?:request )?(?:failed|unavailable)|Direct WebGPU import failed|Browser WebGPU import returned incomplete handles|Uncaptured WebGPU error|WebGPU device lost|Donner (?:worker surface|bitmap bridge)/i;

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
  await driver.actions([
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

function displayedFrame(state) {
  return state.surfaceMode === "bitmap-bridge"
    ? Number(state.surface?.bitmapFrame || 0)
    : Number(state.surface?.directFrame || 0);
}

function assertNoFatal(label, states) {
  for (const state of states) {
    assert.notEqual(state.runtime?.failed, true, `${label}: worker runtime reported failure`);
    assert.equal(
      state.wakeFailure,
      null,
      `${label}: renderer pthread wake failure stats published`,
    );
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

function assertAcceptedEpoch(label, state, minimumToken = 0) {
  const token = Number(state.accepted?.token || 0);
  assert.equal(state.accepted?.kind, "geode", `${label}: accepted presentation was not Geode`);
  assert.ok(
    token > minimumToken,
    `${label}: accepted token ${token} did not exceed ${minimumToken}`,
  );
  assert.equal(
    displayedFrame(state),
    token,
    `${label}: displayed surface did not match accepted presentation`,
  );
  assert.equal(state.visibleSurfaceCount, 1, `${label}: expected exactly one accepted surface`);
  assert.ok(state.surface?.width > 0 && state.surface?.height > 0, `${label}: empty surface`);
  const scale = Number(state.devicePixelRatio || 0);
  const rect = state.surface?.rect;
  const clip = state.surface?.clipInsets;
  assert.ok(scale > 0 && rect && clip, `${label}: missing device-pixel surface geometry`);
  assert.ok(
    Math.abs(rect.width * scale - state.surface.width) <= 1e-4,
    `${label}: ${state.surface.width} backing texels were stretched across ${
      rect.width * scale
    } device pixels`,
  );
  assert.ok(
    Math.abs(rect.height * scale - state.surface.height) <= 1e-4,
    `${label}: ${state.surface.height} backing texels were stretched across ${
      rect.height * scale
    } device pixels`,
  );
  const deviceClipEdges = [
    rect.x + clip.left,
    rect.y + clip.top,
    rect.x + rect.width - clip.right,
    rect.y + rect.height - clip.bottom,
  ].map((edge) => edge * scale);
  for (const edge of deviceClipEdges) {
    assert.ok(
      Math.abs(edge - Math.round(edge)) <= 1e-6,
      `${label}: worker clip edge ${edge} did not land on the shared device-pixel grid`,
    );
  }
}

function decodeScreenshotPng(filePath) {
  const bytes = fs.readFileSync(filePath);
  assert.deepEqual(
    [...bytes.subarray(0, 8)],
    [137, 80, 78, 71, 13, 10, 26, 10],
    `${filePath}: SafariDriver screenshot was not a PNG`,
  );

  let width = 0;
  let height = 0;
  let bitDepth = 0;
  let colorType = 0;
  let interlace = 0;
  const compressed = [];
  for (let offset = 8; offset < bytes.length;) {
    const length = bytes.readUInt32BE(offset);
    const type = bytes.toString("ascii", offset + 4, offset + 8);
    const data = bytes.subarray(offset + 8, offset + 8 + length);
    if (type === "IHDR") {
      width = data.readUInt32BE(0);
      height = data.readUInt32BE(4);
      bitDepth = data[8];
      colorType = data[9];
      interlace = data[12];
    } else if (type === "IDAT") {
      compressed.push(data);
    } else if (type === "IEND") {
      break;
    }
    offset += length + 12;
  }

  assert.ok(width > 0 && height > 0, `${filePath}: PNG omitted its dimensions`);
  assert.equal(bitDepth, 8, `${filePath}: expected an 8-bit Safari screenshot`);
  assert.ok([2, 6].includes(colorType), `${filePath}: unsupported PNG color type ${colorType}`);
  assert.equal(interlace, 0, `${filePath}: interlaced Safari screenshots are unsupported`);
  const channels = colorType === 6 ? 4 : 3;
  const rowBytes = width * channels;
  const filtered = zlib.inflateSync(Buffer.concat(compressed));
  assert.equal(
    filtered.length,
    height * (rowBytes + 1),
    `${filePath}: unexpected decoded PNG byte count`,
  );
  const pixels = Buffer.alloc(height * rowBytes);
  const paeth = (left, above, upperLeft) => {
    const prediction = left + above - upperLeft;
    const leftDistance = Math.abs(prediction - left);
    const aboveDistance = Math.abs(prediction - above);
    const upperLeftDistance = Math.abs(prediction - upperLeft);
    if (leftDistance <= aboveDistance && leftDistance <= upperLeftDistance) return left;
    return aboveDistance <= upperLeftDistance ? above : upperLeft;
  };
  for (let y = 0; y < height; ++y) {
    const filter = filtered[y * (rowBytes + 1)];
    const sourceOffset = y * (rowBytes + 1) + 1;
    const destOffset = y * rowBytes;
    for (let byte = 0; byte < rowBytes; ++byte) {
      const encoded = filtered[sourceOffset + byte];
      const left = byte >= channels ? pixels[destOffset + byte - channels] : 0;
      const above = y > 0 ? pixels[destOffset + byte - rowBytes] : 0;
      const upperLeft = y > 0 && byte >= channels
        ? pixels[destOffset + byte - rowBytes - channels]
        : 0;
      let predictor = 0;
      if (filter === 1) predictor = left;
      else if (filter === 2) predictor = above;
      else if (filter === 3) predictor = Math.floor((left + above) / 2);
      else if (filter === 4) predictor = paeth(left, above, upperLeft);
      else assert.equal(filter, 0, `${filePath}: unsupported PNG filter ${filter}`);
      pixels[destOffset + byte] = (encoded + predictor) & 0xff;
    }
  }

  return {
    height,
    pixel(x, y) {
      assert.ok(x >= 0 && x < width && y >= 0 && y < height, `pixel outside screenshot: ${x},${y}`);
      const offset = (y * width + x) * channels;
      return [...pixels.subarray(offset, offset + 3)];
    },
    width,
  };
}

function analyzeSolidDocumentEdges(filePath, state) {
  const screenshot = decodeScreenshotPng(filePath);
  const canvas = state.canvasRect;
  const surface = state.surface;
  assert.ok(
    canvas && surface?.rect && surface?.clipInsets,
    "missing surface geometry for seam probe",
  );
  const scaleX = screenshot.width / canvas.width;
  const scaleY = screenshot.height / canvas.height;
  assert.ok(Math.abs(scaleX - scaleY) <= 1e-6, "Safari screenshot used nonuniform scaling");
  assert.ok(
    Math.abs(scaleX - state.devicePixelRatio) <= 1e-6,
    `Safari screenshot scale ${scaleX} did not match DPR ${state.devicePixelRatio}`,
  );

  const rect = surface.rect;
  const clip = surface.clipInsets;
  const edges = {
    bottom: (rect.y + rect.height - clip.bottom - canvas.y) * scaleY,
    left: (rect.x + clip.left - canvas.x) * scaleX,
    right: (rect.x + rect.width - clip.right - canvas.x) * scaleX,
    top: (rect.y + clip.top - canvas.y) * scaleY,
  };
  const samples = [
    { axis: "x", edge: "left", fixed: edges.top + (edges.bottom - edges.top) * 0.45, sign: 1 },
    { axis: "x", edge: "right", fixed: edges.top + (edges.bottom - edges.top) * 0.45, sign: -1 },
    { axis: "y", edge: "top", fixed: edges.left + (edges.right - edges.left) * 0.9, sign: 1 },
    { axis: "y", edge: "bottom", fixed: edges.left + (edges.right - edges.left) * 0.9, sign: -1 },
  ];
  const maxChannelDelta = (left, right) =>
    Math.max(...left.map((value, i) => Math.abs(value - right[i])));
  const diagnostics = [];
  for (const sample of samples) {
    const edgeCoordinate = edges[sample.edge];
    const insideCoordinate = sample.sign > 0
      ? Math.ceil(edgeCoordinate - 0.5)
      : Math.floor(edgeCoordinate - 0.5);
    const referenceCoordinate = insideCoordinate + sample.sign * 4;
    const fixedCoordinate = Math.round(sample.fixed - 0.5);
    const insidePixel = sample.axis === "x"
      ? screenshot.pixel(insideCoordinate, fixedCoordinate)
      : screenshot.pixel(fixedCoordinate, insideCoordinate);
    const referencePixel = sample.axis === "x"
      ? screenshot.pixel(referenceCoordinate, fixedCoordinate)
      : screenshot.pixel(fixedCoordinate, referenceCoordinate);
    diagnostics.push({
      deviceCoordinate: edgeCoordinate,
      edge: sample.edge,
      insidePixel,
      maxChannelDelta: maxChannelDelta(insidePixel, referencePixel),
      referencePixel,
    });
  }
  return diagnostics;
}

function scanSolidDocumentEdges(filePath, state) {
  const screenshot = decodeScreenshotPng(filePath);
  const canvas = state.canvasRect;
  const surface = state.surface;
  assert.ok(
    canvas && surface?.rect && surface?.clipInsets,
    "missing surface geometry for live seam scan",
  );
  const scaleX = screenshot.width / canvas.width;
  const scaleY = screenshot.height / canvas.height;
  assert.ok(Math.abs(scaleX - scaleY) <= 1e-6, "Safari screenshot used nonuniform scaling");
  const rect = surface.rect;
  const clip = surface.clipInsets;
  const edges = {
    bottom: (rect.y + rect.height - clip.bottom - canvas.y) * scaleY,
    left: (rect.x + clip.left - canvas.x) * scaleX,
    right: (rect.x + rect.width - clip.right - canvas.x) * scaleX,
    top: (rect.y + clip.top - canvas.y) * scaleY,
  };
  const maxChannelDelta = (left, right) =>
    Math.max(...left.map((value, i) => Math.abs(value - right[i])));
  const diagnostics = [];
  for (const edge of ["left", "right", "top", "bottom"]) {
    const vertical = edge === "left" || edge === "right";
    const leading = edge === "left" || edge === "top";
    const deviceCoordinate = edges[edge];
    const insideCoordinate = leading
      ? Math.ceil(deviceCoordinate - 0.5)
      : Math.floor(deviceCoordinate - 0.5);
    const referenceCoordinate = insideCoordinate + (leading ? 4 : -4);
    const variableStart = Math.ceil((vertical ? edges.top : edges.left) + 8);
    const variableEnd = Math.floor((vertical ? edges.bottom : edges.right) - 8);
    const midpoint = Math.floor((variableStart + variableEnd) * 0.5);
    const midpointPixel = vertical
      ? screenshot.pixel(insideCoordinate, midpoint)
      : screenshot.pixel(midpoint, insideCoordinate);
    const midpointReferencePixel = vertical
      ? screenshot.pixel(referenceCoordinate, midpoint)
      : screenshot.pixel(midpoint, referenceCoordinate);
    let firstMismatch = null;
    let maxDelta = 0;
    let mismatchedSamples = 0;
    let samples = 0;
    for (let variable = variableStart; variable <= variableEnd; variable += 2) {
      const insidePixel = vertical
        ? screenshot.pixel(insideCoordinate, variable)
        : screenshot.pixel(variable, insideCoordinate);
      const referencePixel = vertical
        ? screenshot.pixel(referenceCoordinate, variable)
        : screenshot.pixel(variable, referenceCoordinate);
      const delta = maxChannelDelta(insidePixel, referencePixel);
      maxDelta = Math.max(maxDelta, delta);
      samples += 1;
      if (delta > 2) {
        mismatchedSamples += 1;
        firstMismatch ??= { insidePixel, referencePixel, variable };
      }
    }
    diagnostics.push({
      deviceCoordinate,
      edge,
      firstMismatch,
      maxChannelDelta: maxDelta,
      midpointPixel,
      midpointReferencePixel,
      mismatchedSamples,
      samples,
    });
  }
  return diagnostics;
}

function samePresentedGeometry(before, after) {
  return Number(before.accepted?.token || 0) === Number(after.accepted?.token || 0)
    && JSON.stringify(before.surface?.rect || null) === JSON.stringify(after.surface?.rect || null)
    && JSON.stringify(before.surface?.clipInsets || null)
      === JSON.stringify(after.surface?.clipInsets || null);
}

function assertNoSolidDocumentEdgeSeam(label, diagnostics) {
  for (const diagnostic of diagnostics) {
    assert.ok(
      diagnostic.maxChannelDelta <= 2,
      `${label}: ${diagnostic.edge} edge leaked compositor background: ${
        JSON.stringify(diagnostic)
      }`,
    );
    assert.ok(
      Math.abs(diagnostic.deviceCoordinate - Math.round(diagnostic.deviceCoordinate)) <= 1e-6,
      `${label}: ${diagnostic.edge} edge ${diagnostic.deviceCoordinate} split a device pixel`,
    );
  }
}

function assertThumbnailDiagnostic(state, initialDeviceState) {
  const thumbnails = state.thumbnailStats;
  assert.ok(thumbnails, "thumbnail statistics were not published");
  assert.deepEqual(
    {
      requested: thumbnails.requested,
      started: thumbnails.started,
      completed: thumbnails.completed,
      rendered: thumbnails.rendered,
      ready: thumbnails.ready,
      pending: thumbnails.pending,
      active: thumbnails.active,
      resultReady: thumbnails.resultReady,
    },
    {
      requested: 4,
      started: 4,
      completed: 4,
      rendered: 4,
      ready: 4,
      pending: false,
      active: false,
      resultReady: false,
    },
    "four SVG thumbnails did not settle cleanly",
  );
  assert.ok(
    thumbnails.firstRequestFrame > thumbnails.carouselFrame,
    "thumbnail rendering did not start asynchronously after the carousel frame",
  );
  assert.equal(thumbnails.publicationFrames?.length, 4, "expected four publication epochs");
  for (let index = 1; index < thumbnails.publicationFrames.length; ++index) {
    assert.ok(
      thumbnails.publicationFrames[index] > thumbnails.publicationFrames[index - 1],
      `thumbnail publication epoch ${index} did not advance`,
    );
  }
  assert.deepEqual(
    {
      headlessDeviceCreations: state.headlessDeviceCreations,
      workerDeviceCreations: state.runtime?.workerDeviceCreations,
    },
    initialDeviceState,
    "thumbnail generation unexpectedly created another WebGPU device",
  );

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
  assert.equal(fingerprints.size, 4, "the four SVG thumbnail fingerprints were not distinct");
  assert.ok(diagnostics[2].backgroundPixels > 1000, "text thumbnail lacked its background");
  assert.ok(diagnostics[2].glyphPixels > 20, "text thumbnail lacked rendered glyphs");
}

async function installErrorCapture(driver) {
  await driver.execute(`
    clearInterval(window.__donnerSafariRegressionTimer);
    if (window.__donnerSafariRegressionAnimationFrameRequest) {
      cancelAnimationFrame(window.__donnerSafariRegressionAnimationFrameRequest);
    }
    window.__donnerSafariRegressionErrors = [];
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
  `);
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
    const visibleSurfaces = Array.from(
      document.querySelectorAll('canvas[data-direct-surface-visible="true"]'),
    );
    const surface = visibleSurfaces[0] || null;
    const clipInsets = (element) => {
      const match = element?.style.clipPath?.match(/^inset\\(([^)]+)\\)$/);
      if (!match) return null;
      const values = match[1].trim().split(/\\s+/).map((value) => Number.parseFloat(value));
      if (values.length < 1 || values.length > 4 || values.some((value) => !Number.isFinite(value))) {
        return null;
      }
      const [top, second = top, third = top, fourth = second] = values;
      return values.length === 2
        ? { top, right: second, bottom: top, left: second }
        : values.length === 3
        ? { top, right: second, bottom: third, left: second }
        : { top, right: second, bottom: third, left: fourth };
    };
    return {
      accepted: window.__donnerAcceptedPresentation || null,
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
      loadingHidden: Boolean(loading?.hidden),
      mainLoopRenderedFrames: window.__donnerMainLoopRenderedFrames || 0,
      documentHasFocus: document.hasFocus(),
      timerTicks: window.__donnerSafariRegressionTimerTicks || 0,
      visibilityState: document.visibilityState,
      pageTimeOrigin: performance.timeOrigin,
      runtimeInitializedAtMs: window.__donnerRuntimeInitializedAtMs || 0,
      runtime: window.__donnerWorkerRuntimeStats || null,
      surface: surface ? {
        bitmapFrame: Number(surface.dataset.bitmapBridgeFrame || 0),
        clipInsets: clipInsets(surface),
        directFrame: Number(surface.dataset.directSurfaceFrame || 0),
        height: surface.height,
        rect: rect(surface),
        slot: surface.id,
        width: surface.width,
      } : null,
      surfaceMode: window.__donnerWorkerSurfaceMode || null,
      thumbnailStats: window.__donnerSampleThumbnailStats || null,
      visibleSurfaceCount: visibleSurfaces.length,
      wakeFailure: window.__donnerWorkerTaskWakeFailureStats || null,
      wgpuReadback: window.__donnerWgpuReadbackStats || null,
      wgpuReadbackCaptureCompletions: window.__donnerWgpuReadbackCaptureCompletions || 0,
      wgpuReadbackCaptureFailures: window.__donnerWgpuReadbackCaptureFailures || 0,
      wgpuReadbackCaptureStarts: window.__donnerWgpuReadbackCaptureStarts || 0,
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
    return state.loadingHidden && state.runtime?.ready && state.canvasRect ? state : null;
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

async function waitForAcceptedEpoch(driver, label, minimumToken) {
  return poll(label, async () => {
    const state = await readState(driver);
    assertNoFatal(label, [state]);
    const token = Number(state.accepted?.token || 0);
    if (token > minimumToken && displayedFrame(state) === token) {
      return state;
    }
    return null;
  });
}

async function capture(driver, name) {
  const filePath = path.join(kArtifactDir, `${name}.png`);
  await driver.screenshot(filePath);
  return filePath;
}

async function runRegression(driver, editorUrl, result) {
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
  await installErrorCapture(driver);
  result.visibleAutomation = await requireVisibleSafariAnimationFrame(driver);

  result.initial = await waitForEditor(driver, "initial Safari editor startup");
  assert.ok(["geode", "packaged"].includes(result.initial.backend), "package was not Geode");
  assert.equal(result.initial.runtime.initializationCount, 1, "worker initialized more than once");
  assert.ok(result.initial.runtime.workerDeviceCreations >= 1, "worker created no WebGPU device");
  assert.equal(
    result.initial.surfaceMode,
    "bitmap-bridge",
    "Safari did not select its bitmap bridge",
  );
  const initialHeapBytes = Number(result.initial.heapBytes || 0);
  assert.ok(initialHeapBytes >= 64 * 1024 * 1024, "Geode started below the 64 MiB heap fence");
  result.initialScreenshot = await capture(driver, "01-initial-carousel");

  const initialDeviceState = {
    headlessDeviceCreations: result.initial.headlessDeviceCreations,
    workerDeviceCreations: result.initial.runtime.workerDeviceCreations,
  };
  result.thumbnailsSettled = await poll(
    "asynchronous SVG thumbnails",
    async () => {
      const state = await readState(driver);
      assertNoFatal("asynchronous SVG thumbnails", [state]);
      return state.thumbnailStats?.ready === 4 ? state : null;
    },
    Math.max(kTimeoutMs, 20_000),
    25,
  );
  result.thumbnailsScreenshot = await capture(driver, "02-thumbnails-settled");
  assertFlatHeap("asynchronous SVG thumbnails", result.thumbnailsSettled, initialHeapBytes);

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

  const canvas = result.thumbnailDiagnostic.canvasRect;
  const beforeSampleToken = Number(result.thumbnailDiagnostic.accepted?.token || 0);
  const beforeSampleResults = Number(result.thumbnailDiagnostic.worker?.completedResults || 0);
  const sample = kLivePanOnly
    ? { id: "text-style", label: "Text and Style", xFraction: 0.6875 }
    : { id: "basic-shapes", label: "Basic Shapes", xFraction: 0.5 };
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
      accepted: window.__donnerAcceptedPresentation || null,
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
    const token = Number(state.accepted?.token || 0);
    if (
      state.activeSample?.sampleId === sample.id
      && token > beforeSampleToken
      && displayedFrame(state) === token
      && Number(state.worker?.completedResults || 0) > beforeSampleResults
    ) {
      return state;
    }
    return null;
  });
  assertAcceptedEpoch(sample.label, result.selectedSample, beforeSampleToken);
  assertFlatHeap(sample.label, result.selectedSample, initialHeapBytes);
  result.selectedSampleScreenshot = await capture(driver, `03-${sample.id}`);

  if (kLivePanOnly) {
    const panPoint = {
      x: result.selectedSample.surface.rect.x + result.selectedSample.surface.rect.width * 0.5,
      y: result.selectedSample.surface.rect.y + result.selectedSample.surface.rect.height * 0.5,
    };
    await driver.actions([pointerMove(panPoint.x, panPoint.y, 80)]);
    await driver.releaseActions();
    result.livePanFrames = [];
    const badFrames = [];
    const goodFrames = [];
    const edgePixels = new Set();
    for (let step = 1; step <= 48; ++step) {
      const magnitude = 9 + (step % 4) * 4;
      const direction = step % 2 === 0 ? -1 : 1;
      await driver.scroll(
        panPoint.x,
        panPoint.y,
        direction * magnitude,
        step % 3 === 0 ? direction * 5 : 0,
      );
      const beforeCapture = await readState(driver);
      const screenshot = await capture(driver, `04-live-pan-${String(step).padStart(2, "0")}`);
      const state = await readState(driver);
      if (!samePresentedGeometry(beforeCapture, state)) {
        result.livePanFrames.push({
          beforeAcceptedToken: Number(beforeCapture.accepted?.token || 0),
          beforeSurfaceRect: beforeCapture.surface?.rect || null,
          screenshot,
          skippedGeometryRace: true,
          afterAcceptedToken: Number(state.accepted?.token || 0),
          afterSurfaceRect: state.surface?.rect || null,
        });
        continue;
      }
      const diagnostics = scanSolidDocumentEdges(screenshot, state);
      const leftEdge = diagnostics.find((edge) => edge.edge === "left");
      edgePixels.add(JSON.stringify(leftEdge?.midpointPixel || []));
      const bad = diagnostics.some((edge) => edge.maxChannelDelta > 2);
      const frame = {
        acceptedToken: Number(state.accepted?.token || 0),
        bad,
        diagnostics,
        screenshot,
        surfaceRect: state.surface?.rect || null,
      };
      result.livePanFrames.push(frame);
      if (bad) {
        badFrames.push(frame);
      } else {
        goodFrames.push(frame);
      }
      if (badFrames.length >= 2 && goodFrames.length >= 1 && edgePixels.size >= 2) {
        break;
      }
    }
    assert.ok(goodFrames.length >= 1, "Safari live pan captured no clean edge frame");
    assert.ok(
      edgePixels.size >= 2,
      `Safari live pan did not expose changing edge pixels: ${[...edgePixels].join(", ")}`,
    );
    assert.deepEqual(
      badFrames,
      [],
      `Safari exposed ${badFrames.length} wheel-pan frames with document-edge background leaks`,
    );
    return;
  }

  result.basicShapes = result.selectedSample;
  result.basicShapesScreenshot = result.selectedSampleScreenshot;

  const seamPanPoint = {
    x: result.basicShapes.surface.rect.x + result.basicShapes.surface.rect.width * 0.5,
    y: result.basicShapes.surface.rect.y + result.basicShapes.surface.rect.height * 0.5,
  };
  await driver.actions([pointerMove(seamPanPoint.x, seamPanPoint.y, 80)]);
  await driver.releaseActions();
  const beforeSeamPan = await readState(driver);
  await driver.execute(
    `
    const [x, y] = arguments;
    const canvas = document.getElementById('canvas');
    return canvas.dispatchEvent(new WheelEvent('wheel', {
      bubbles: true,
      cancelable: true,
      clientX: x,
      clientY: y,
      deltaMode: WheelEvent.DOM_DELTA_PIXEL,
      deltaX: -2.5,
      deltaY: -1.25,
    }));
  `,
    [seamPanPoint.x, seamPanPoint.y],
  );
  const fractionalPan = await poll("fractional Safari pan layout", async () => {
    const state = await readState(driver);
    assertNoFatal("fractional Safari pan layout", [state]);
    const beforeRect = beforeSeamPan.surface?.rect;
    const rect = state.surface?.rect;
    if (
      rect && beforeRect
      && (Math.abs(rect.x - beforeRect.x) > 1e-6 || Math.abs(rect.y - beforeRect.y) > 1e-6)
    ) {
      return state;
    }
    return null;
  });
  const fractionalPanScreenshot = await capture(driver, "04-fractional-pan-seam");
  result.fractionalPanSeamProbe = {
    afterRect: fractionalPan.surface.rect,
    beforeRect: beforeSeamPan.surface.rect,
    diagnostics: analyzeSolidDocumentEdges(fractionalPanScreenshot, fractionalPan),
    screenshot: fractionalPanScreenshot,
  };
  assertNoSolidDocumentEdgeSeam(
    "fractional Safari pan",
    result.fractionalPanSeamProbe.diagnostics,
  );
  if (kSeamOnly) {
    return;
  }

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

  const documentRect = result.basicShapes.surface?.rect;
  assert.ok(documentRect, "Basic Shapes did not publish a visible document surface");
  const dragStart = {
    x: documentRect.x + documentRect.width * (122 / 640),
    y: documentRect.y + documentRect.height * (92 / 400),
  };
  await click(driver, dragStart.x, dragStart.y);
  await driver.actions([pointerMove(dragStart.x, dragStart.y), { type: "pointerDown", button: 0 }]);
  result.dragEpochs = [];
  let previousToken = Number((await readState(driver)).accepted?.token || 0);
  try {
    for (let step = 1; step <= 16; ++step) {
      await driver.actions([pointerMove(dragStart.x + step * 6, dragStart.y + step * 3, 8)]);
      const state = await waitForAcceptedEpoch(
        driver,
        `accepted Safari drag epoch ${step}`,
        previousToken,
      );
      assertAcceptedEpoch(`Safari drag epoch ${step}`, state, previousToken);
      assertFlatHeap(`Safari drag epoch ${step}`, state, initialHeapBytes);
      previousToken = Number(state.accepted.token);
      result.dragEpochs.push({
        acceptedToken: previousToken,
        displayedFrame: displayedFrame(state),
        heapBytes: state.heapBytes,
        workerResults: Number(state.worker?.completedResults || 0),
      });
      if (step % 4 === 0) {
        await capture(driver, `04-drag-${String(step).padStart(2, "0")}`);
      }
    }
  } finally {
    await driver.actions([{ type: "pointerUp", button: 0 }]).catch(() => {});
    await driver.releaseActions().catch(() => {});
  }
  const dragTokens = result.dragEpochs.map((sample) => sample.acceptedToken);
  assert.deepEqual(
    dragTokens,
    [...dragTokens].sort((left, right) => left - right),
    "accepted drag epochs regressed",
  );
  assert.equal(new Set(dragTokens).size, dragTokens.length, "accepted drag epochs repeated");
  assertNoFatal("Safari drag epochs", [await readState(driver)]);

  result.wakeBurst = [];
  result.wakeInput = [];
  let wakePoint = { x: dragStart.x + 96, y: dragStart.y + 48 };
  for (let iteration = 0; iteration < 4; ++iteration) {
    const direction = iteration % 2 === 0 ? 1 : -1;
    const inputBefore = await driver.execute(`
      return {
        eventCount: (window.__donnerSafariClickProbeEvents || []).length,
        mainLoopRenderedFrames: window.__donnerMainLoopRenderedFrames || 0,
      };
    `);
    await click(driver, wakePoint.x, wakePoint.y);
    await new Promise((resolve) => setTimeout(resolve, 20));
    const beforeToken = Number((await readState(driver)).accepted?.token || 0);
    await driver.actions([
      pointerMove(wakePoint.x, wakePoint.y, 40),
      { type: "pause", duration: 30 },
      { type: "pointerDown", button: 0 },
      { type: "pause", duration: 30 },
    ]);
    try {
      const moves = [];
      for (let step = 1; step <= 12; ++step) {
        moves.push(pointerMove(
          wakePoint.x + direction * step * 3,
          wakePoint.y + direction * step * 2,
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
      const [eventOffset, beforeToken] = arguments;
      return {
        beforeToken,
        events: (window.__donnerSafariClickProbeEvents || []).slice(eventOffset),
        activeSample: window.__donnerActiveSampleStats || null,
        accepted: window.__donnerAcceptedPresentation || null,
        editorFrameRequested: Boolean(window.__donnerEditorFrameRequested),
        mainLoopRenderedFrames: window.__donnerMainLoopRenderedFrames || 0,
        worker: window.__donnerWorkerStats || null,
      };
    `,
      [inputBefore.eventCount, beforeToken],
    );
    result.wakeInput.push(wakeInput);
    assertTrustedClickAtPoint(
      `renderer pthread wake burst ${iteration + 1}`,
      wakeInput.events,
      wakePoint,
    );
    const after = await waitForAcceptedEpoch(
      driver,
      `renderer pthread wake burst ${iteration + 1}`,
      beforeToken,
    );
    assertAcceptedEpoch(`renderer pthread wake burst ${iteration + 1}`, after, beforeToken);
    assertFlatHeap(`renderer pthread wake burst ${iteration + 1}`, after, initialHeapBytes);
    result.wakeBurst.push({
      acceptedToken: Number(after.accepted.token),
      displayedFrame: displayedFrame(after),
      heapBytes: after.heapBytes,
      workerResults: Number(after.worker?.completedResults || 0),
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
  // shape while proving the worker, accepted surface, Wasm heap, and page lifetime remain stable.
  result.resizeChurn = [];
  let resizeToken = Number(result.afterWakeBurst.accepted?.token || 0);
  const initialPageTimeOrigin = Number(result.initial.pageTimeOrigin || 0);
  assert.ok(initialPageTimeOrigin > 0, "Safari did not expose a stable page time origin");
  for (let iteration = 0; iteration < 24; ++iteration) {
    const width = 1400 + (iteration % 12) * 13;
    const height = 850 + Math.floor(iteration / 12) * 37 + (iteration % 3) * 7;
    await driver.request("POST", `/session/${driver.sessionId}/window/rect`, {
      width,
      height,
      x: 20,
      y: 20,
    });
    const state = await waitForAcceptedEpoch(
      driver,
      `Safari primary-target resize churn ${iteration + 1}`,
      resizeToken,
    );
    assertAcceptedEpoch(`Safari primary-target resize churn ${iteration + 1}`, state, resizeToken);
    assertFlatHeap(`Safari primary-target resize churn ${iteration + 1}`, state, initialHeapBytes);
    assert.equal(
      Number(state.pageTimeOrigin || 0),
      initialPageTimeOrigin,
      `Safari reloaded the page under resize memory pressure at iteration ${iteration + 1}`,
    );
    resizeToken = Number(state.accepted.token);
    result.resizeChurn.push({
      acceptedToken: resizeToken,
      displayedFrame: displayedFrame(state),
      heapBytes: state.heapBytes,
      height,
      width,
      workerResults: Number(state.worker?.completedResults || 0),
    });
  }
  await driver.request("POST", `/session/${driver.sessionId}/window/rect`, {
    width: 1600,
    height: 1000,
    x: 20,
    y: 20,
  });
  result.afterResizeChurn = await waitForAcceptedEpoch(
    driver,
    "Safari resize-churn restore",
    resizeToken,
  );
  assertAcceptedEpoch("Safari resize-churn restore", result.afterResizeChurn, resizeToken);
  assertFlatHeap("Safari resize-churn restore", result.afterResizeChurn, initialHeapBytes);
  assert.equal(
    Number(result.afterResizeChurn.pageTimeOrigin || 0),
    initialPageTimeOrigin,
    "Safari reloaded the page while restoring the viewport after resize churn",
  );
  result.afterResizeChurnScreenshot = await capture(driver, "06-after-resize-churn");

  await driver.request("POST", `/session/${driver.sessionId}/refresh`, {});
  await installErrorCapture(driver);
  result.visibleAutomationAfterReload = await requireVisibleSafariAnimationFrame(driver);
  result.afterReload = await waitForEditor(driver, "Safari reload after renderer teardown");
  assert.equal(
    result.afterReload.runtime.initializationCount,
    1,
    "reload initialized worker twice",
  );
  assert.ok(result.afterReload.runtime.workerDeviceCreations >= 1, "reload created no device");
  assertNoFatal("Safari reload after renderer teardown", [result.afterReload]);
  assertFlatHeap("Safari reload after renderer teardown", result.afterReload, initialHeapBytes);
  result.afterReloadSettled = await poll(
    "Safari reload thumbnails",
    async () => {
      const state = await readState(driver);
      assertNoFatal("Safari reload thumbnails", [state]);
      assertFlatHeap("Safari reload thumbnails", state, initialHeapBytes);
      return state.thumbnailStats?.ready === 4 ? state : null;
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
    ...result.dragEpochs.map((sample) => sample.heapBytes),
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
  editorUrl.searchParams.set("wgpuReadbackStats", "1");
  const result = {
    artifactDir: kArtifactDir,
    driverUrl: kDriverUrl,
    editorUrl: editorUrl.href,
    scope: kLivePanOnly ? "live-pan-seam" : kSeamOnly ? "fractional-pan-seam" : "full",
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
    const passLabel = kLivePanOnly
      ? "real Apple Safari live-pan edge pixels"
      : kSeamOnly
      ? "real Apple Safari fractional-pan edge pixels"
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
