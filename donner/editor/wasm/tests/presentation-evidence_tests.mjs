import assert from "node:assert/strict";
import { existsSync, mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import { createRequire } from "node:module";
import { tmpdir } from "node:os";
import path from "node:path";
import { test } from "node:test";
import { fileURLToPath } from "node:url";
import { PNG } from "pngjs";

import {
  assertCallerProtocolFilesUnchanged,
  kCompletedReport,
  writeCallerProtocolFiles,
  writeIncompleteComparators,
} from "./incomplete-comparators.mjs";

// The page-free cases of browser-presentation-regression.spec.ts: they drive its capture, evidence
// and diagnosis helpers with fake pages, synthetic PNGs and fixed records, so they need no browser
// and run with the other Node tests instead of in a browser lane.
const require = createRequire(import.meta.url);
assert.ok(process.env.DONNER_PRESENTATION_EVIDENCE, "Bazel must provide the bundled helpers");
assert.ok(process.env.DONNER_BROWSER_GOLDEN_COMPARE, "Bazel must provide the native comparator");
const {
  basicShapesGateState,
  hasSuccessfulDiagnosticRequest,
  wgpuDiagnosticOutcome,
  canvasCaptureImplicatesApplication,
  captureEditorPage,
  captureFailureCanvasEvidence,
  captureWithCanvasAccount,
  compareOverlayBitmap,
  completedFrameMatchesCamera,
  boundFailureDiagnostic,
  diagnoseCanvasAfterState,
  diagnosePostFailureGpuReadback,
  hasPresentedBasicShapesHostFrame,
  kFailureEvidenceBudgetMs,
  kUnavailableCanvasAccount,
  readEditorPixelBoundsFromPng,
  readEditorResizePixelBounds,
  scoredFrameIsStable,
} = require(path.resolve(process.env.DONNER_PRESENTATION_EVIDENCE));
// The bundle loads this same module, so the timeout it recognizes is the one thrown here.
const { errors } = require("@playwright/test");

const testDirectory = path.dirname(fileURLToPath(import.meta.url));
const scratchRoot = process.env.TEST_TMPDIR ?? tmpdir();

/**
 * A `size` x `size` opaque PNG of `background`, with an optional `square` x `square` block of
 * `fill` at the origin.
 */
function syntheticCapturePng(size, background, square = 0, fill = background) {
  const image = new PNG({ width: size, height: size });
  for (let y = 0; y < size; ++y) {
    for (let x = 0; x < size; ++x) {
      const color = x < square && y < square ? fill : background;
      image.data.set([...color, 255], (y * size + x) * 4);
    }
  }
  return PNG.sync.write(image);
}

/** Read until `accept` holds, at most `attempts` times; the fakes answer immediately. */
async function readUntil(read, accept, attempts) {
  let value;
  for (let attempt = 0; attempt < attempts; ++attempt) {
    value = await read();
    if (accept(value)) return value;
  }
  assert.fail(`no accepted value in ${attempts} reads; last ${JSON.stringify(value)}`);
}

// Fixtures are built offline: the fake pages test only the helpers' retry policy, and a live
// capture can fail on its own, for example when the GPU process is lost mid-capture.
test("editor page capture leaves retries to the outer poll and keeps drags one-shot", async () => {
  const visible = syntheticCapturePng(64, [44, 47, 56], 8, [45, 110, 230]);
  const black = syntheticCapturePng(64, [12, 15, 20]);

  // The baseline poll owns the retry schedule: black, timeout, then visible.
  let shots = 0;
  const baselinePage = {
    screenshot: async (options) => {
      assert.equal(options, undefined);
      ++shots;
      if (shots === 1) {
        return black;
      }
      if (shots === 2) {
        throw new errors.TimeoutError(
          "Timeout 1000ms exceeded\nCall log:\n - taking page screenshot\n - waiting for fonts to load...",
        );
      }
      return visible;
    },
  };
  const observations = [];
  await readUntil(async () => {
    const capture = await captureEditorPage(baselinePage);
    const blue = capture.usable
      ? readEditorPixelBoundsFromPng(capture.png, "basic-blue", { width: 64, height: 64 }, {
        minX: 0,
        minY: 0,
        maxX: 64,
        maxY: 64,
      })
      : null;
    observations.push([shots, capture.usable, blue !== null]);
    return blue?.pixels ?? 0;
  }, (pixels) => pixels > 0, 10);
  assert.equal(shots, 3);
  assert.deepEqual(observations, [
    [1, true, false],
    [2, false, false],
    [3, true, true],
  ]);

  let dragShots = 0;
  const oneShotDragPage = {
    viewportSize: () => ({ width: 64, height: 64 }),
    screenshot: async (options) => {
      assert.equal(options, undefined);
      ++dragShots;
      return black;
    },
  };
  const dragCapture = await readEditorResizePixelBounds(
    oneShotDragPage,
    { x: 0, y: 0, width: 64, height: 64 },
  );
  assert.deepEqual([dragCapture.usableCapture, dragCapture.blue, dragCapture.teal, dragShots], [
    true,
    null,
    null,
    1,
  ]);
  assert.deepEqual(dragCapture.png, black);

  const timedOut = await captureEditorPage({
    screenshot: async () => {
      throw new errors.TimeoutError(
        "Timeout 1000ms exceeded\nCall log:\n - taking page screenshot\n - waiting for fonts to load...\n - fonts loaded",
      );
    },
  });
  assert.deepEqual([timedOut.usable, timedOut.attempts, timedOut.timedOutCaptures], [false, 1, 1]);
  assert.equal(timedOut.png.length, 0);
  assert.deepEqual(timedOut.timeoutStage, {
    takingPageScreenshot: true,
    waitingForFonts: true,
    fontsLoaded: true,
  });

  await assert.rejects(captureEditorPage({
    screenshot: async () => {
      throw new Error("non-timeout screenshot failure");
    },
  }), /non-timeout screenshot failure/);
});

test("scored Firefox frame evidence attributes only stable app frames", async () => {
  const frame = {
    renderedFrames: 60,
    hostFrames: 60,
    acquiredFrames: 59,
    presentedFrames: 59,
    lastAcquired: false,
    lastPresented: false,
  };
  const probe = {
    beforeRead: frame,
    afterRead: frame,
    opaquePixels: 0,
    pngBase64: null,
  };
  assert.equal(scoredFrameIsStable(frame, probe), true);
  assert.equal(
    scoredFrameIsStable(frame, { ...probe, afterRead: { ...frame, renderedFrames: 61 } }),
    false,
  );
  assert.equal(
    scoredFrameIsStable(frame, { ...probe, beforeRead: { ...frame, presentedFrames: 60 } }),
    false,
  );
  assert.equal(scoredFrameIsStable({ ...frame, hostFrames: null }, probe), false);
  assert.equal(scoredFrameIsStable({ ...frame, acquiredFrames: null }, probe), false);
  assert.equal(scoredFrameIsStable(null, probe), false);
  assert.equal(scoredFrameIsStable(frame, null), false);
  assert.equal(await boundFailureDiagnostic(new Promise(() => {}), 10), null);
});

test("canvas diagnosis does not queue after an unavailable state IPC", async () => {
  let pageReads = 0;
  const page = {
    evaluate: async () => {
      ++pageReads;
      return null;
    },
  };
  assert.deepEqual(await diagnoseCanvasAfterState(page, true, false), {
    unavailable: "canvas diagnosis skipped after state IPC unavailable",
  });
  assert.equal(pageReads, 0);
});

test("post-failure WGPU readback stays idle after a successful baseline", async () => {
  let pageReads = 0;
  const page = {
    evaluate: async () => {
      ++pageReads;
      return null;
    },
  };
  assert.equal(await diagnosePostFailureGpuReadback(page, false), null);
  assert.equal(pageReads, 0);
});

test("Basic Shapes capture gate accepts a completed current frame after UI coalescing", () => {
  const state = {
    sampleId: "basic-shapes",
    completedResults: 1,
    presentedAtMs: 11835,
    renderedFrames: 61,
    hostFrames: 61,
    hostPresented: false,
    frameId: 40,
    captureId: 4,
    completedFrameId: 40,
    completedCaptureId: 4,
    completedInputRepresented: true,
  };
  assert.equal(hasPresentedBasicShapesHostFrame(state, 0), true);
  assert.equal(
    hasPresentedBasicShapesHostFrame({ ...state, completedInputRepresented: false }, 0),
    false,
  );
  assert.equal(hasPresentedBasicShapesHostFrame({ ...state, completedCaptureId: 3 }, 0), false);
  assert.equal(hasPresentedBasicShapesHostFrame({ ...state, completedFrameId: 39 }, 0), false);
  assert.equal(hasPresentedBasicShapesHostFrame({ ...state, hostFrames: 60 }, 0), false);
  assert.equal(hasPresentedBasicShapesHostFrame({ ...state, completedResults: 0 }, 0), false);
});

function canvasEvidenceForTest(overrides = {}) {
  const { lateSubmits = 0, newestLateFrame = null, uiVertices = 3940, probe, ...surface } =
    overrides;
  return {
    surface: {
      renderedFrames: 10,
      hostFrames: 10,
      acquiredFrames: 10,
      presentedFrames: 10,
      lastAcquired: true,
      lastPresented: true,
      ...surface,
    },
    uiVertices,
    completedResults: 3,
    presentedAtMs: 100,
    probe: "probe" in overrides
      ? probe ?? null
      : { canvasWorkers: 1, frames: 10, inTaskSubmits: 10, lateSubmits, newestLateFrame },
  };
}

test("canvas frame account clears an editor that drew every frame", () => {
  const clean = canvasEvidenceForTest();
  assert.equal(canvasCaptureImplicatesApplication(clean, clean), null);
  // A frame written late before the reads is not the frame on screen once a newer one presents.
  const olderLateFrame = canvasEvidenceForTest({ lateSubmits: 1, newestLateFrame: 9 });
  assert.equal(canvasCaptureImplicatesApplication(olderLateFrame, olderLateFrame), null);
});

test("canvas frame account implicates the editor for each way it could present nothing", () => {
  const probe = (canvasWorkers, inTaskSubmits) => ({
    canvasWorkers,
    frames: 10,
    inTaskSubmits,
    lateSubmits: 0,
    newestLateFrame: null,
  });
  const cases = [
    [
      canvasEvidenceForTest({ acquiredFrames: null }),
      canvasEvidenceForTest(),
      "the application's frame account is incomplete",
    ],
    [
      canvasEvidenceForTest(),
      canvasEvidenceForTest({ probe: null }),
      "the application's frame account is incomplete",
    ],
    [
      canvasEvidenceForTest({ probe: probe(2, 10) }),
      canvasEvidenceForTest({ probe: probe(2, 10) }),
      "the application's frame account is incomplete",
    ],
    [
      canvasEvidenceForTest({ probe: probe(1, 0) }),
      canvasEvidenceForTest({ probe: probe(1, 0) }),
      "the application's frame account is incomplete",
    ],
    [
      canvasEvidenceForTest(),
      canvasEvidenceForTest({ acquiredFrames: 11 }),
      "a frame acquired the canvas without completing its draw",
    ],
    [
      canvasEvidenceForTest({ lastPresented: false }),
      canvasEvidenceForTest(),
      "the latest host frame did not complete its presentation",
    ],
    [
      canvasEvidenceForTest(),
      canvasEvidenceForTest({ uiVertices: 0 }),
      "the latest host frame recorded no editor UI",
    ],
    [
      canvasEvidenceForTest({ uiVertices: null }),
      canvasEvidenceForTest(),
      "the latest host frame recorded no editor UI",
    ],
    [
      canvasEvidenceForTest({ lateSubmits: 1, newestLateFrame: 10 }),
      canvasEvidenceForTest({ lateSubmits: 1, newestLateFrame: 10 }),
      "the newest canvas frame was written after its acquiring task ended",
    ],
    [
      canvasEvidenceForTest(),
      canvasEvidenceForTest({ lateSubmits: 1 }),
      "a frame wrote to the canvas after its acquiring task ended",
    ],
  ];
  assert.deepEqual(
    cases.map(([before, after]) => canvasCaptureImplicatesApplication(before, after)),
    cases.map(([, , reason]) => reason),
  );
});

// Drives the account path with injected reads, capture and attach; no page is involved.
async function recordCanvasAccountForTest(read, attach = async () => {}) {
  const shot = Buffer.from("capture");
  let captures = 0;
  const attached = [];
  const png = await captureWithCanvasAccount({
    read,
    attach: async (json) => {
      attached.push(json);
      await attach(json);
    },
    boundMs: 50,
  }, async () => {
    ++captures;
    return shot;
  });
  return { png, captures, attached };
}

const kUnavailableAccountRecord = {
  before: kUnavailableCanvasAccount,
  after: kUnavailableCanvasAccount,
  applicationImplication: "the application's frame account is unavailable",
};

test("a frame account read that throws records the account as unavailable", async () => {
  for (
    const read of [
      async () => {
        throw new Error("evaluation failed");
      },
      () => {
        throw new Error("synchronous failure");
      },
    ]
  ) {
    const result = await recordCanvasAccountForTest(read);
    assert.deepEqual([result.png.toString(), result.captures], ["capture", 1]);
    assert.deepEqual(result.attached.map((json) => JSON.parse(json)), [kUnavailableAccountRecord]);
  }
});

test("failure canvas evidence stays within its bound when the page never answers", async () => {
  // Every page and worker call records itself and never settles, so the capture can finish only
  // through its own per-step bounds.
  const calls = [];
  const silent = (name) => (...args) => {
    calls.push([name, ...args.filter((arg) => typeof arg !== "function")]);
    return new Promise(() => {});
  };
  const silentPage = {
    evaluate: silent("page.evaluate"),
    screenshot: silent("page.screenshot"),
    workers: () => [{ evaluate: silent("worker.evaluate") }],
  };
  // The parts of Playwright's test info the evidence writer uses.
  const outputDirectory = mkdtempSync(path.join(scratchRoot, "failure-canvas-evidence-"));
  const testInfo = {
    testId: "failure-canvas-evidence-bound",
    timeout: 30_000,
    outputPath: (name) => path.join(outputDirectory, name),
    attach: async () => {},
  };
  const stepMs = 50;
  const startedAt = Date.now();
  await captureFailureCanvasEvidence(silentPage, testInfo, stepMs);
  const elapsedMs = Date.now() - startedAt;
  // The state read (page and worker together) and the screenshot each wait one bounded step; the
  // readback is skipped because the page did not answer the state read. No call is retried.
  assert.deepEqual(calls, [
    ["worker.evaluate"],
    ["page.evaluate"],
    ["page.screenshot", { timeout: stepMs }],
  ]);
  // The whole capture is bounded: far inside the budget that three full steps would take.
  assert.ok(kFailureEvidenceBudgetMs <= 15_000);
  assert.ok(elapsedMs < kFailureEvidenceBudgetMs, `capture took ${elapsedMs} ms`);
  const recordPath = testInfo.outputPath("failure-canvas-evidence.json");
  const record = JSON.parse(readFileSync(recordPath, "utf8"));
  assert.equal(record.presentation, "timeout");
  assert.deepEqual(record.screenshot, { error: "timeout" });
  assert.deepEqual(record.readback, { skipped: "the page did not answer the state read" });
  // A worker that never answers is read in the state step and recorded as a timeout.
  assert.equal(record.workers.length, 1);
  assert.deepEqual(Object.keys(record.workers[0]).sort(), ["askedAt", "timeout"]);
  assert.equal(record.workers[0].timeout, true);
  assert.equal(typeof record.workers[0].askedAt, "number");
});

test("a frame account read that never settles records the account as unavailable", async () => {
  const result = await recordCanvasAccountForTest(() => new Promise(() => {}));
  assert.deepEqual([result.png.toString(), result.captures], ["capture", 1]);
  assert.deepEqual(result.attached.map((json) => JSON.parse(json)), [kUnavailableAccountRecord]);
});

test("a frame account attach that fails or never settles still returns the capture", async () => {
  const clean = canvasEvidenceForTest();
  const attaches = [
    async () => {
      throw new Error("attach failed");
    },
    () => new Promise(() => {}),
  ];
  for (const attach of attaches) {
    const result = await recordCanvasAccountForTest(async () => clean, attach);
    assert.deepEqual([result.png.toString(), result.captures, result.attached.length], [
      "capture",
      1,
      1,
    ]);
    assert.equal(JSON.parse(result.attached[0]).applicationImplication, null);
  }
});

// The Chromium golden that the spec's overlay cases compare against.
const kOverlayGolden = path.join(
  testDirectory,
  "browser-presentation-regression.spec.ts-snapshots",
  "basic-shapes-compositor-tile-overlay-darwin.png",
);

test("native overlay pixelmatch retains a one-pixel artwork regression", () => {
  const baseline = readFileSync(kOverlayGolden);
  const outputDir = path.join(
    mkdtempSync(path.join(process.env.TEST_UNDECLARED_OUTPUTS_DIR ?? scratchRoot, "overlay-")),
    "overlay-native-guard",
  );
  const hostileEnvironment = {
    ...process.env,
    GTEST_FILTER: "NoSuchSuite.NoSuchCase",
    GTEST_REPEAT: "0",
    GTEST_TOTAL_SHARDS: "4",
    GTEST_SHARD_INDEX: "3",
    GTEST_LIST_TESTS: "1",
    GTEST_OUTPUT: "json:stale.json",
    TESTBRIDGE_TEST_ONLY: "NoSuchSuite.NoSuchCase",
    UPDATE_GOLDEN_IMAGES_DIR: outputDir,
  };
  assert.equal(
    compareOverlayBitmap(baseline, kOverlayGolden, outputDir, hostileEnvironment).matched,
    true,
  );

  const oneChannel = PNG.sync.read(baseline);
  const roundedOffset = (274 * oneChannel.width + 279) * 4 + 1;
  oneChannel.data[roundedOffset] += 1;
  const accepted = compareOverlayBitmap(
    PNG.sync.write(oneChannel),
    kOverlayGolden,
    outputDir,
    hostileEnvironment,
  );
  assert.equal(accepted.matched, true, accepted.detail);

  oneChannel.data[roundedOffset] += 1;
  const rejected = compareOverlayBitmap(
    PNG.sync.write(oneChannel),
    kOverlayGolden,
    outputDir,
    hostileEnvironment,
  );
  assert.equal(rejected.matched, false, rejected.detail);
  assert.ok([rejected.failureActual, rejected.failureExpected, rejected.diff].every(existsSync));

  const changed = PNG.sync.read(baseline);
  const artworkOffset = (80 * changed.width + 100) * 4;
  changed.data.set([255, 0, 0, 255], artworkOffset);
  const comparison = compareOverlayBitmap(
    PNG.sync.write(changed),
    kOverlayGolden,
    outputDir,
    hostileEnvironment,
  );
  assert.equal(comparison.matched, false, comparison.detail);
  assert.ok(
    [comparison.failureActual, comparison.failureExpected, comparison.diff].every(existsSync),
  );
});

const kIncompleteComparators = writeIncompleteComparators(
  mkdtempSync(path.join(scratchRoot, "incomplete-comparator-")),
);

for (const { behavior, comparator, expectedError } of kIncompleteComparators) {
  test(`overlay comparison fails when its comparator ${behavior}`, () => {
    const outputDir = mkdtempSync(
      path.join(process.env.TEST_UNDECLARED_OUTPUTS_DIR ?? scratchRoot, "overlay-incomplete-"),
    );
    const caller = writeCallerProtocolFiles(outputDir);
    // A completed report from an earlier run must not stand in for this one.
    writeFileSync(path.join(outputDir, "gtest-result.json"), JSON.stringify(kCompletedReport));
    const environment = {
      ...process.env,
      ...caller.settings,
      GTEST_OUTPUT: `json:${caller.report}`,
      DONNER_BROWSER_GOLDEN_COMPARE: comparator,
    };
    const captured = readFileSync(kOverlayGolden);
    assert.throws(
      () => compareOverlayBitmap(captured, kOverlayGolden, outputDir, environment),
      expectedError,
    );
    assertCallerProtocolFilesUnchanged(caller.files);
  });
}

test("zoom camera completion accepts retained scenes but rejects failed presentation", () => {
  const state = {
    queue: { captureId: 14, inputRepresented: false, viewportZoom: 3.375 },
    viewport: { zoom: 3.375 },
    host: { lastSurfacePresented: true },
  };
  assert.equal(completedFrameMatchesCamera(state), true);
  assert.equal(
    completedFrameMatchesCamera({ ...state, host: { lastSurfacePresented: false } }),
    false,
  );
  assert.equal(completedFrameMatchesCamera({ ...state, viewport: { zoom: 5.0625 } }), false);
  assert.equal(
    completedFrameMatchesCamera({ ...state, queue: { ...state.queue, deviceLost: true } }),
    false,
  );
  assert.equal(
    completedFrameMatchesCamera({ ...state, queue: { ...state.queue, captureId: 0 } }),
    false,
  );
});

test("pre-capture gate reads only published frame state", () => {
  const scope = {
    __donnerActiveSampleStats: { sampleId: "basic-shapes" },
    __donnerWorkerStats: { completedResults: 2, presentedAtMs: 3 },
    __donnerMainLoopRenderedFrames: 7,
    __donnerHostFrameTiming: { frames: 7, lastSurfacePresented: true },
    __donnerOverlayStats: { frameId: 12, captureId: 4 },
    __donnerPresentationQueueStats: { frameId: 12, captureId: 4, inputRepresented: true },
    get document() { throw new Error("DOM observation before capture"); },
    getComputedStyle() { throw new Error("style observation before capture"); },
  };
  assert.deepEqual(basicShapesGateState(scope), {
    sampleId: "basic-shapes", completedResults: 2, presentedAtMs: 3, renderedFrames: 7,
    hostFrames: 7, hostPresented: true, frameId: 12, captureId: 4, completedFrameId: 12,
    completedCaptureId: 4, completedInputRepresented: true,
  });
});

test("readback completion distinguishes a failed request from old successful pixels", () => {
  const scope = {
    __donnerWgpuReadbackRequested: 2, __donnerWgpuReadbackCompleted: 2,
    __donnerWgpuReadbackStats: { request: 1 }, __donnerWgpuReadbackLastFailedRequest: 2,
    __donnerWgpuReadbackCaptureFailures: 1,
  };
  const failed = wgpuDiagnosticOutcome(scope);
  assert.equal(failed.completed, 2);
  assert.equal(failed.successfulRequest, 1);
  assert.equal(hasSuccessfulDiagnosticRequest(failed, 2), false);
  scope.__donnerWgpuReadbackRequested = scope.__donnerWgpuReadbackCompleted = 3;
  scope.__donnerWgpuReadbackStats = { request: 3 };
  const recovered = wgpuDiagnosticOutcome(scope);
  assert.equal(hasSuccessfulDiagnosticRequest(recovered, 3), true);
  assert.equal(hasSuccessfulDiagnosticRequest(recovered, 4), false);
});
