// Diagnostic policy is shared by browser consumers and ordinary Node tests.

const gpuCounterNames = [
  "deviceRequests",
  "queueSubmits",
  "queueSubmitFailures",
  "queueWaits",
  "queueDone",
  "queueRejected",
  "mapRequests",
  "mapResolved",
  "mapRejected",
  "mapPending",
  "observedAtMs",
  "lastQueueSubmitAtMs",
  "lastQueueDoneAtMs",
  "lastMapRequestAtMs",
  "lastMapResolvedAtMs",
  "lastMapRejectedAtMs",
  "gpuValidationErrors",
  "gpuOutOfMemoryErrors",
  "gpuInternalErrors",
  "gpuOtherErrors",
  "diagnosticObserverErrors",
  "deviceLostDestroyed",
  "deviceLostOther",
];

function formatGpuCounters(value) {
  const counters = value !== null && typeof value === "object"
    ? value
    : {};
  return gpuCounterNames.map((name) => {
    const count = counters[name];
    return `${name}=${
      typeof count === "number" && Number.isSafeInteger(count) && count >= 0
        ? count
        : "unknown"
    }`;
  }).join(",");
}

async function readGpuDiagnosticsBounded(
  page,
) {
  let timer;
  try {
    return await Promise.race([
      page.evaluate(() => {
        const { current, atDeadline } = window.__donnerGpuDiagnostics;
        return { current, atDeadline };
      }).catch(() => null),
      new Promise((resolve) => {
        timer = setTimeout(() => resolve(null), 500);
      }),
    ]);
  } catch {
    return null;
  } finally {
    if (timer !== undefined) clearTimeout(timer);
  }
}

const browserDiagnosticStages = new Map([
  ["[wasm] render_svg entry", "render_entry"],
  ["[wasm] parsed OK", "svg_parsed"],
  ["[wasm] RendererGeode constructed", "renderer_constructed"],
  ["[wasm] draw() returned", "draw_returned"],
  ["[wasm] takeSnapshot() returned", "snapshot_returned"],
  ["[Geode] snapshot map reached the capture deadline", "snapshot_map_deadline"],
]);

function browserDiagnosticStage(line) {
  for (const [prefix, stage] of browserDiagnosticStages) {
    if (line.startsWith(prefix)) {
      return stage;
    }
  }
  return null;
}

function pageErrorCategory(error) {
  const message = error.message;
  if (/device.{0,24}lost|lost.{0,24}device/i.test(message)) {
    return "WebGPU device lost";
  }
  if (error.name === "GPUValidationError" || /GPUValidationError|validation error/i.test(message)) {
    return "WebGPU validation error";
  }
  if (error.name === "GPUOutOfMemoryError" || /GPUOutOfMemoryError|out of memory/i.test(message)) {
    return "WebGPU out of memory";
  }
  if (/Asyncify/i.test(message)) {
    return "Wasm Asyncify error";
  }
  if (error.name === "RuntimeError") {
    return "Wasm runtime error";
  }
  if (error.name === "TypeError") {
    return "JavaScript type error";
  }
  if (error.name === "RangeError") {
    return "JavaScript range error";
  }
  if (error.name === "ReferenceError") {
    return "JavaScript reference error";
  }
  return "Unclassified browser page error";
}

const kGpuAcquisitionPattern =
  /^\[Geode\/browser\/acquire\] stage=(selection|runtime|root) outcome=(api_unavailable|adapter_null|adapter_rejected|device_rejected|device_install_failed|deadline_pending|request_failed|context_failed|construction_failed)$/;
function fixedGpuAcquisitionEvent(text) {
  const match = kGpuAcquisitionPattern.exec(text);
  return match ? `stage=${match[1]} outcome=${match[2]}` : null;
}

module.exports = {
  browserDiagnosticStage,
  fixedGpuAcquisitionEvent,
  formatGpuCounters,
  gpuCounterNames,
  pageErrorCategory,
  readGpuDiagnosticsBounded,
};
