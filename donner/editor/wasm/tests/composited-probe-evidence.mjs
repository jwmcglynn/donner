import { writeFile } from "node:fs/promises";

/** Keep the loss observation explicitly comparable with probe `performance.now()` samples. */
export function normalizePresentationDeviceLoss(loss) {
  if (loss === null || loss === undefined) return null;
  const firstObservedAbsoluteMs = Number(loss.firstObservedAbsoluteMs);
  const pageTimeOriginMs = Number(loss.pageTimeOriginMs);
  return {
    ...loss,
    firstObservedAtMs: Number.isFinite(firstObservedAbsoluteMs) && Number.isFinite(pageTimeOriginMs)
      ? firstObservedAbsoluteMs - pageTimeOriginMs
      : null,
  };
}

/**
 * What the editor's own completed presentation frames say about a drag ordering candidate.
 *
 * Each probe sample keeps the editor's last completed presentation frame (`presentedFrame`: frame
 * id, represented pointer and serials). A candidate is a readback that moved against the drag. If
 * the editor's completed frame went backwards over the same pair of samples, by frame id or by the
 * pointer position it represents, the editor presented the older position. If the editor's frame
 * held or advanced along the drag, only the readback went back: the read-back path returned an
 * older image than the editor had already completed.
 *
 * @param {ReadonlyArray<{presentedFrame?: object | null}>} samples The probe's samples.
 * @param {{predecessorIndex: number, sampleIndex: number, pointerDx: number,
 *   pointerDy: number}} regression The candidate, with the drag direction over its interval.
 * @returns {{predecessor: object | null, sample: object | null, frameIdDelta: number | null,
 *   representedPointerAlongDrag: number | null,
 *   verdict: "editor-frame-went-backwards" | "readback-only" | "no-editor-frame"}}
 */
export function presentedFrameOrdering(samples, regression) {
  const predecessor = samples[regression.predecessorIndex]?.presentedFrame ?? null;
  const sample = samples[regression.sampleIndex]?.presentedFrame ?? null;
  if (predecessor === null || sample === null) {
    return {
      predecessor,
      sample,
      frameIdDelta: null,
      representedPointerAlongDrag: null,
      verdict: "no-editor-frame",
    };
  }
  const frameIdDelta = sample.frameId - predecessor.frameId;
  const dragLength = Math.hypot(regression.pointerDx, regression.pointerDy);
  // How far the represented pointer moved along the drag, CSS px; negative is against it.
  const representedPointerAlongDrag = dragLength > 0
    ? ((sample.pointerX - predecessor.pointerX) * regression.pointerDx
      + (sample.pointerY - predecessor.pointerY) * regression.pointerDy) / dragLength
    : 0;
  const wentBackwards = frameIdDelta < 0 || representedPointerAlongDrag < 0;
  return {
    predecessor,
    sample,
    frameIdDelta,
    representedPointerAlongDrag,
    verdict: wentBackwards ? "editor-frame-went-backwards" : "readback-only",
  };
}

/**
 * Stop sampling and preserve the collected window before any assertion.
 * @param {import("@playwright/test").Page} page
 * @param {import("@playwright/test").TestInfo} testInfo
 * @param {unknown} gesture
 * @returns {Promise<import("./composited-probe.ts").CompositedProbeResult>}
 */
export async function stopCompositedProbe(page, testInfo, gesture) {
  const result = await page.evaluate(() => {
    const probe = window.__donnerCompositedProbe;
    if (probe === undefined) {
      throw new Error("composited probe: install before stop");
    }
    probe.stop();
    return {
      samples: probe.samples.slice(),
      drawFailures: probe.drawFailures,
      frames: probe.frames,
      readbackRetries: probe.readbackRetries,
      readbackRescues: probe.readbackRescues,
      rawReadbackBytes: probe.rawReadbackBytes ?? 0,
      rawReadbackOverflow: probe.rawReadbackOverflow ?? false,
      presentation: {
        interaction: window.__donnerInteractionStats ?? null,
        overlay: window.__donnerOverlayStats ?? null,
        queue: window.__donnerPresentationQueueStats ?? null,
        repair: window.__donnerPresentationRepairStats ?? null,
        worker: window.__donnerWorkerStats ?? null,
        host: window.__donnerHostFrameTiming ?? null,
        viewport: window.__donnerViewportStats ?? null,
        deviceLoss: window.__donnerPresentationDeviceLoss ?? null,
      },
    };
  });
  if (result.presentation) {
    result.presentation.deviceLoss = normalizePresentationDeviceLoss(
      result.presentation.deviceLoss,
    );
  }
  const evidencePath = testInfo.outputPath("composited-probe.json");
  await writeFile(evidencePath, JSON.stringify({ gesture, result }), "utf8");
  await testInfo.attach("composited-probe", {
    path: evidencePath,
    contentType: "application/json",
  });
  return result;
}

function encodeRetainedReadbacks({ indices, maximumBytes }) {
  const probe = window.__donnerCompositedProbe;
  if (!probe || probe.running || !probe.captureReadbacks) {
    throw new Error("Stop the enabled readback capture before encoding evidence");
  }
  if (probe.rawReadbackOverflow) throw new Error("Raw readback memory budget exceeded");
  const width = probe.rawReadbackWidth;
  const height = probe.rawReadbackHeight;
  const byteLength = width * height * 4;
  if (
    !Number.isSafeInteger(width) || !Number.isSafeInteger(height)
    || !Number.isSafeInteger(byteLength) || width <= 0 || height <= 0
  ) {
    throw new Error("Invalid retained readback dimensions");
  }
  const canvas = document.createElement("canvas");
  canvas.width = width;
  canvas.height = height;
  const context = canvas.getContext("2d");
  if (!context) throw new Error("Could not create the evidence encoding context");
  let encodedBytes = 0;
  return indices.map((index) => {
    const pixels = probe.rawReadbacks[index];
    if (index >= probe.samples.length || !(pixels instanceof Uint8ClampedArray)) {
      throw new Error(`Missing retained readback for sample ${index}`);
    }
    if (pixels.byteLength !== byteLength) {
      throw new Error(`Readback dimensions do not match sample ${index}`);
    }
    context.putImageData(new ImageData(pixels, width, height), 0, 0);
    const dataUrl = canvas.toDataURL("image/png");
    const prefix = "data:image/png;base64,";
    if (!dataUrl.startsWith(prefix)) throw new Error(`PNG encoding failed for sample ${index}`);
    const png = dataUrl.slice(prefix.length);
    const bytes = atob(png).length;
    if (bytes > maximumBytes - encodedBytes) throw new Error("Encoded readback budget exceeded");
    encodedBytes += bytes;
    return { index, png };
  });
}

function prepareReadbackFiles(records, indices, maximumBytes) {
  const byIndex = new Map();
  let bytes = 0;
  for (const record of records) {
    if (!indices.includes(record.index) || byIndex.has(record.index)) {
      throw new Error("Unexpected or duplicate readback sample index");
    }
    if (typeof record.png !== "string") throw new Error("Invalid encoded readback");
    const size = Buffer.byteLength(record.png, "base64");
    if (size > maximumBytes - bytes) throw new Error("Encoded readback budget exceeded");
    const body = Buffer.from(record.png, "base64");
    if (!body.subarray(0, 8).equals(Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]))) {
      throw new Error("Readback is not a PNG image");
    }
    bytes += body.length;
    byIndex.set(record.index, body);
  }
  for (const index of indices) {
    if (!byIndex.has(index)) throw new Error(`Missing retained readback for sample ${index}`);
  }
  return { byIndex, bytes };
}

/** Encode retained sample bytes after sampling stops, then archive the complete bounded set. */
export async function attachCompositedReadbacks(page, testInfo, requestedIndices) {
  const indices = [...new Set(requestedIndices)].sort((left, right) => left - right);
  if (
    indices.length === 0 || indices.length > 16
    || indices.some((index) => !Number.isSafeInteger(index) || index < 0)
  ) {
    throw new Error("Request between 1 and 16 valid readback sample indices");
  }
  const maximumBytes = 8 * 1024 * 1024;
  const start = performance.now();
  const records = await page.evaluate(encodeRetainedReadbacks, { indices, maximumBytes });
  const { byIndex, bytes } = prepareReadbackFiles(records, indices, maximumBytes);
  const encodingMs = performance.now() - start;
  for (const [index, body] of byIndex) {
    const name = `composited-frame-${index}`;
    const filename = testInfo.outputPath(`${name}.png`);
    await writeFile(filename, body, { mode: 0o600 });
    await testInfo.attach(name, { path: filename, contentType: "image/png" });
  }
  return { indices, bytes, encodingMs };
}
