import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import fs from "node:fs";
import { createRequire } from "node:module";
import path from "node:path";
import { test } from "node:test";
import { PNG } from "pngjs";

const require = createRequire(import.meta.url);
assert.ok(process.env.DONNER_GPU_TRANSFER_COMPARISON, "Bazel must provide the comparison module");
assert.ok(process.env.DONNER_BROWSER_GOLDEN_COMPARE, "Bazel must provide the native comparator");
assert.ok(process.env.TEST_TMPDIR, "Bazel must provide sandbox scratch space");
assert.ok(process.env.TEST_UNDECLARED_OUTPUTS_DIR, "Bazel must retain comparison evidence");
const { compareGpuTransferSamples } = require(
  path.resolve(process.env.DONNER_GPU_TRANSFER_COMPARISON),
);
const scratch = fs.mkdtempSync(path.join(process.env.TEST_TMPDIR, "gpu-transfer-comparison-"));
const evidence = fs.mkdtempSync(
  path.join(process.env.TEST_UNDECLARED_OUTPUTS_DIR, "gpu-transfer-comparison-"),
);
const sentinel = "caller test protocol sentinel\n";

function setup(name, hostile) {
  const outputDirectory = path.join(evidence, name);
  fs.mkdirSync(outputDirectory);
  const protocolFiles = {
    XML_OUTPUT_FILE: path.join(outputDirectory, "caller-test.xml"),
    TEST_PREMATURE_EXIT_FILE: path.join(outputDirectory, "caller-premature-exit"),
    TEST_SHARD_STATUS_FILE: path.join(outputDirectory, "caller-shard-status"),
    TEST_WARNINGS_OUTPUT_FILE: path.join(outputDirectory, "caller-warnings"),
    GTEST_SHARD_STATUS_FILE: path.join(outputDirectory, "caller-gtest-shard-status"),
  };
  const callerReport = path.join(outputDirectory, "caller-gtest-report.json");
  const callerFiles = [...Object.values(protocolFiles), callerReport];
  for (const file of callerFiles) fs.writeFileSync(file, sentinel);
  const environment = { ...process.env };
  for (const key of Object.keys(environment)) {
    if (key.startsWith("GTEST_") || key.startsWith("TESTBRIDGE_")) delete environment[key];
  }
  Object.assign(environment, protocolFiles, {
    TEST_UNDECLARED_OUTPUTS_DIR: outputDirectory,
    UPDATE_GOLDEN_IMAGES_DIR: scratch,
    DONNER_BROWSER_COMPARE_MODE: "invalid-inherited-mode",
  });
  if (hostile) {
    Object.assign(environment, {
      GTEST_FILTER: "NoSuchSuite.NoSuchCase",
      GTEST_REPEAT: "0",
      GTEST_TOTAL_SHARDS: "4",
      GTEST_SHARD_INDEX: "3",
      GTEST_LIST_TESTS: "1",
      GTEST_OUTPUT: `json:${callerReport}`,
      TESTBRIDGE_TEST_ONLY: "NoSuchSuite.NoSuchCase",
    });
  }
  return { outputDirectory, environment, callerFiles };
}

function authoredSamples(outputDirectory) {
  const samples = [];
  const hashes = new Set();
  for (let sequence = 0; sequence < 12; ++sequence) {
    const data = Buffer.alloc(32 * 24 * 4);
    for (let y = 0; y < 24; ++y) {
      for (let x = 0; x < 32; ++x) {
        const offset = (y * 32 + x) * 4;
        data[offset] = (sequence * 17 + x * 7) % 256;
        data[offset + 1] = (sequence * 29 + y * 11) % 256;
        data[offset + 2] = (sequence * 31 + x + y) % 256;
        data[offset + 3] = 255;
      }
    }
    const png = PNG.sync.write({ width: 32, height: 24, data });
    const decoded = PNG.sync.read(png);
    assert.deepEqual([decoded.width, decoded.height], [32, 24]);
    assert.deepEqual(
      decoded.data,
      data,
      `sample ${sequence} must contain its authored RGBA pixels`,
    );
    assert.ok(decoded.data.some((value, index) => index % 4 !== 3 && value > 0));
    hashes.add(createHash("sha256").update(png).digest("hex"));
    const actualPath = path.join(outputDirectory, `${sequence}-actual.png`);
    const expectedPath = path.join(outputDirectory, `${sequence}-expected.png`);
    fs.writeFileSync(actualPath, png);
    fs.writeFileSync(expectedPath, png);
    fs.writeFileSync(
      path.join(outputDirectory, `${sequence}-compare.json`),
      JSON.stringify({ stale: true }),
    );
    samples.push({
      sequence,
      width: 32,
      height: 24,
      actualPath,
      expectedPath,
      pixelChecked: false,
    });
  }
  assert.equal(hashes.size, 12, "all twelve authored images must be distinct");
  assert.equal(
    new Set(samples.flatMap(({ actualPath, expectedPath }) => [actualPath, expectedPath])).size,
    24,
  );
  for (let sequence = 12; sequence < 15; ++sequence) {
    samples.push({
      sequence,
      width: 2560,
      height: 1800,
      stage: "timing-only",
      pixelChecked: false,
    });
  }
  return samples;
}

function completedReports(samples, outputDirectory) {
  const completed = [];
  for (const sample of samples.slice(0, 12)) {
    const file = path.join(outputDirectory, `${sample.sequence}-compare.json`);
    const report = JSON.parse(fs.readFileSync(file, "utf8"));
    assert.equal(report.stale, undefined, "each native report must replace its stale predecessor");
    assert.deepEqual([report.tests, report.failures, report.disabled, report.errors], [1, 0, 0, 0]);
    assert.equal(report.testsuites.length, 1);
    assert.equal(report.testsuites[0].testsuite.length, 1);
    const record = report.testsuites[0].testsuite[0];
    assert.deepEqual([record.classname, record.name, record.status, record.result], [
      "StandaloneGeodeBrowserPngCompare",
      "CanvasMatchesGolden",
      "RUN",
      "COMPLETED",
    ]);
    completed.push({ sequence: sample.sequence, file, tests: report.tests });
    assert.deepEqual(fs.readFileSync(sample.actualPath), fs.readFileSync(sample.expectedPath));
  }
  fs.writeFileSync(
    path.join(outputDirectory, "completed-comparisons.json"),
    JSON.stringify(completed, null, 2),
  );
  assert.equal(completed.length, 12);
}

for (const hostile of [false, true]) {
  test(`saved samples preserve caller protocol with ${hostile ? "hostile" : "neutral"} GTest settings`, () => {
    const context = setup(hostile ? "hostile" : "neutral", hostile);
    const samples = authoredSamples(context.outputDirectory);
    compareGpuTransferSamples(samples, context.outputDirectory, context.environment);
    assert.deepEqual(samples.map(({ pixelChecked }) => pixelChecked), [
      ...Array(12).fill(true),
      ...Array(3).fill(false),
    ], "only the twelve pixel pairs may be marked checked");
    if (hostile) completedReports(samples, context.outputDirectory);
    for (const file of context.callerFiles) {
      assert.equal(
        fs.readFileSync(file, "utf8"),
        sentinel,
        `native comparisons must preserve ${path.basename(file)}`,
      );
    }
    if (!hostile) completedReports(samples, context.outputDirectory);
    for (let sequence = 12; sequence < 15; ++sequence) {
      assert.equal(
        fs.existsSync(path.join(context.outputDirectory, `${sequence}-compare.json`)),
        false,
      );
    }
    assert.deepEqual(
      fs.readdirSync(scratch),
      [],
      "inherited golden-update settings must remain suppressed",
    );
  });
}

test("a real native pixel mismatch throws before marking the sample checked", () => {
  const context = setup("mismatch", true);
  const samples = authoredSamples(context.outputDirectory);
  const sample = samples[0];
  const originalGolden = fs.readFileSync(sample.expectedPath);
  const actual = PNG.sync.read(originalGolden);
  for (let index = 0; index < actual.data.length; index += 4) {
    actual.data[index] = 255;
    actual.data[index + 1] = 255;
    actual.data[index + 2] = 255;
  }
  fs.writeFileSync(sample.actualPath, PNG.sync.write(actual));
  assert.notDeepEqual(fs.readFileSync(sample.actualPath), originalGolden);
  assert.throws(() =>
    compareGpuTransferSamples([sample], context.outputDirectory, context.environment)
  );
  assert.equal(sample.pixelChecked, false);
  assert.deepEqual(
    fs.readFileSync(sample.expectedPath),
    originalGolden,
    "mismatch must not update the golden",
  );
  const report = JSON.parse(
    fs.readFileSync(path.join(context.outputDirectory, "0-compare.json"), "utf8"),
  );
  assert.deepEqual([report.tests, report.failures], [1, 1]);
  const record = report.testsuites[0].testsuite[0];
  assert.deepEqual([record.name, record.status, record.result], [
    "CanvasMatchesGolden",
    "RUN",
    "COMPLETED",
  ]);
  assert.equal(record.failures.length, 1);
  for (const file of context.callerFiles) assert.equal(fs.readFileSync(file, "utf8"), sentinel);
  assert.deepEqual(fs.readdirSync(scratch), []);
});
