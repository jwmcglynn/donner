import assert from "node:assert/strict";
import { createRequire } from "node:module";
import test from "node:test";

const require = createRequire(import.meta.url);
const {
  browserDiagnosticStage,
  fixedGpuAcquisitionEvent,
  formatGpuCounters,
  pageErrorCategory,
  readGpuDiagnosticsBounded,
} = require("./browser-diagnostic-policy.js");

test("catalog font loading records only fixed GPU acquisition markers", () => {
  const marker = "[Geode/browser/acquire] stage=selection outcome=adapter_rejected";
  const secret = " token=secret https://example.invalid/private/path";
  assert.equal(fixedGpuAcquisitionEvent(marker), "stage=selection outcome=adapter_rejected");
  assert.equal(fixedGpuAcquisitionEvent(marker + secret), null);
  assert.equal(
    fixedGpuAcquisitionEvent("[Geode/browser/acquire] stage=selection outcome=secret"),
    null,
  );
});

test("browser diagnostics cannot disclose console content", async () => {
  const sensitive = "https://private.example.test/path /Users/operator/private/file";
  assert.equal(browserDiagnosticStage(sensitive), null);
  assert.equal(browserDiagnosticStage(`[wasm] parsed OK ${sensitive}`), "svg_parsed");
  assert.equal(
    pageErrorCategory(new Error(`Asyncify failed at ${sensitive}`)),
    "Wasm Asyncify error",
  );
  assert.equal(
    pageErrorCategory(new Error(`device was lost at ${sensitive}`)),
    "WebGPU device lost",
  );
  const validationError = new Error(sensitive);
  validationError.name = "GPUValidationError";
  assert.equal(pageErrorCategory(validationError), "WebGPU validation error");
  const memoryError = new Error(sensitive);
  memoryError.name = "GPUOutOfMemoryError";
  assert.equal(pageErrorCategory(memoryError), "WebGPU out of memory");
  assert.equal(pageErrorCategory(new Error(sensitive)), "Unclassified browser page error");
  assert.ok(!formatGpuCounters({ mapRequests: sensitive, extra: sensitive }).includes(sensitive));
  const stalledPage = { evaluate: () => new Promise(() => {}) };
  assert.equal(await readGpuDiagnosticsBounded(stalledPage), null);
});
