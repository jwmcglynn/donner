import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";
import vm from "node:vm";

const source = readFileSync(new URL("../../WholeAppWorkerBridge.cc", import.meta.url), "utf8");

function recorder() {
  const statsStart = source.indexOf("EM_JS(void, RecordPresentedDocumentFrameStats,");
  assert.notEqual(statsStart, -1);
  const statsSource = source.slice(statsStart, source.indexOf("\n\nvoid ", statsStart));
  const statsBody = statsSource.match(/int surfacePresented\), \{([\s\S]*?)\n {6}\}\);/);
  assert.ok(statsBody, "presentation stats helper body");
  const start = source.indexOf("void PublishPresentedDocumentFrame(");
  assert.notEqual(start, -1);
  const functionSource = source.slice(start, source.indexOf("\nvoid ", start + 5));
  const body = functionSource.match(/MAIN_THREAD_ASYNC_EM_ASM\(\s*\{([\s\S]*?)\n {6}\},/);
  assert.ok(body, "production presentation callback body");
  const window = {};
  const context = {
    window,
    performance: { timeOrigin: 1000, now: () => 1000000 },
  };
  const predicates = /EM_JS\(int, (WorkerFrame\w+),\s*\(([\s\S]*?)\), \{([\s\S]*?)\n {6}\}\);/g;
  let predicateCount = 0;
  for (const [, name, signature, predicateBody] of source.matchAll(predicates)) {
    const parameters = signature.split(",").map((parameter) => parameter.trim().match(/[A-Za-z_]\w*$/)[0]);
    context[name] = vm.runInNewContext(`(function(${parameters.join(",")}) {${predicateBody}\n})`, context);
    ++predicateCount;
  }
  assert.ok(predicateCount >= 4, "production completion predicates");
  context.RecordPresentedDocumentFrameStats = vm.runInNewContext(
    `(function(documentGeneration, frameVersion, presentedAtEpochMs, surfacePresented) {${statsBody[1]}\n})`,
    context,
  );
  const publish = vm.runInNewContext(
    `(function($0, $1, $2, $3 = true, $4 = 1, $5 = 1, $6 = 1, $7 = true) {${body[1]}\n})`,
    context,
  );
  return { window, publish, stats: () => window.__donnerPresentedDocumentStats };
}

test("only distinct presented document identities count, without worker-result telemetry", () => {
  const r = recorder();
  r.publish(0, 0, 1000);
  assert.equal(r.stats(), undefined);
  r.publish(1, 0, 1010);
  assert.equal(r.stats()?.count, 1);
  r.publish(1, 0, 1020);
  assert.equal(r.stats().count, 1, "UI-only repetition must not inflate document cadence");
  r.publish(1, 2, 1040);
  r.publish(2, 2, 1060);
  assert.equal(r.stats().count, 3, "a replacement document is a distinct identity");
  assert.equal(r.stats().samples.length, 3);
});

test("cadence uses GPU-completion observation time, not delayed diagnostic delivery", () => {
  const r = recorder();
  r.publish(1, 1, 1100);
  r.publish(1, 2, 1117);
  assert.equal(r.stats()?.lastPresentedAtMs, 117);
  assert.equal(r.stats().samples[1][3] - r.stats().samples[0][3], 17);
  assert.equal(r.stats().lastPublicationDelayMs, 999883);
});

test("presentation samples retain a bounded ring of the newest distinct frames", () => {
  const r = recorder();
  for (let frame = 1; frame <= 5000; ++frame) r.publish(1, frame, 1000 + frame * 16);
  assert.equal(r.stats()?.count, 5000);
  assert.equal(r.stats().samples.length, 4096);
  const sequences = Array.from(r.stats().samples, (sample) => sample[0]).sort((a, b) => a - b);
  assert.equal(sequences[0], 905);
  assert.equal(sequences.at(-1), 5000);
});

test("invalid diagnostic identities or timestamps do not manufacture cadence", () => {
  const r = recorder();
  for (
    const args of [
      [-1, 1, 1100],
      [1, -1, 1100],
      [1, 0.5, 1100],
      [Number.MAX_SAFE_INTEGER + 1, 1, 1100],
      [1, 1, NaN],
      [1, 1, Infinity],
    ]
  ) {
    r.publish(...args);
  }
  assert.equal(r.stats(), undefined);
});

test("offscreen completion does not count as surface presentation", () => {
  const r = recorder();
  r.publish(1, 1, 1100, false);
  assert.equal(r.stats(), undefined);
  r.publish(1, 1, 1110, true);
  r.publish(1, 2, 1120, false);
  assert.equal(r.stats().count, 1);
  assert.equal(r.stats().lastPresentedAtMs, 110);
});
