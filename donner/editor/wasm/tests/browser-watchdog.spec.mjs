import assert from "node:assert/strict";
import { spawn, spawnSync } from "node:child_process";
import fs from "node:fs";
import { createRequire, syncBuiltinESMExports } from "node:module";
import os from "node:os";
import path from "node:path";
import { test } from "node:test";
import { fileURLToPath } from "node:url";
import vm from "node:vm";
import { startApplicationHeartbeat } from "./browser-application-heartbeat.mjs";
import { resourceQueuesAreQuiescent } from "./browser-resource-quiescence.mjs";
import {
  browserHostCapability,
  descendants,
  ownedGroup,
  processRows,
  supervise,
} from "./browser-watchdog.mjs";

const { kStallGuardMs } = createRequire(import.meta.url)("./test-timing.cjs");
// The test deadline in a case that asserts a different outcome (a stopped heartbeat, a teardown
// cap, a clean exit) is only a backstop for a hang. It must never win the race against the
// behavior under test, whatever the host load: a real Playwright run or a child start can take
// several seconds on a busy CI runner. It also keeps a hang a failure of its own case, with a
// reason, instead of the 900 s production default running into the Bazel timeout. Cases that rely
// on the deadline firing use 150 ms.
const kBackstopDeadlineMs = kStallGuardMs;
// `supervise` with the backstop deadline unless the case passes its own.
const superviseCase = (executable, args, options) =>
  supervise(executable, args, { deadlineMs: kBackstopDeadlineMs, ...options });
// Polls the heartbeat until `ready` holds or the stall guard runs out, and returns the last one
// read. The application pulse runs every 1000 ms; a fixed wait just past one pulse races a loaded
// host. Heartbeat writes are atomic renames, so every read is a whole heartbeat.
async function heartbeatWhen(file, ready) {
  const deadline = Date.now() + kStallGuardMs;
  for (;;) {
    const heartbeat = JSON.parse(fs.readFileSync(file, "utf8"));
    if (ready(heartbeat) || Date.now() >= deadline) return heartbeat;
    await new Promise((resolve) => setTimeout(resolve, 20));
  }
}
const directory = () =>
  fs.mkdtempSync(path.join(process.env.TEST_TMPDIR ?? os.tmpdir(), "watchdog-"));
test("ownership excludes unrelated processes and follows descendants", () => {
  const rows = [{ pid: 1, ppid: 0 }, { pid: 2, ppid: 1 }, { pid: 3, ppid: 2 }, { pid: 4, ppid: 0 }];
  assert.deepEqual(descendants(rows, 2), [rows[1], rows[2]]);
});
test("Linux process accounting needs no ps executable", () => {
  const procRoot = directory();
  const pid = 123456;
  fs.mkdirSync(path.join(procRoot, String(pid)));
  const fields = ["S", "10", "42", ...Array(16).fill("0"), "901", "0", "0"];
  fs.writeFileSync(
    path.join(procRoot, String(pid), "stat"),
    `${pid} (worker) helper) ${fields.join(" ")}`,
  );
  fs.writeFileSync(path.join(procRoot, String(pid), "status"), "Name: worker\nVmRSS: 7 kB\n");
  const rows = processRows({ platform: "linux", procRoot });
  assert.equal(rows.length, 1);
  assert.deepEqual(rows.find((row) => row.pid === pid), {
    pid,
    ppid: 10,
    pgid: 42,
    start: "901",
    rssBytes: 7168,
    command: "worker) helper",
  });
});

test("Linux child exit between stat and status remains an accounted zombie", () => {
  const procRoot = directory();
  const pid = 123456;
  const directoryPath = path.join(procRoot, String(pid));
  fs.mkdirSync(directoryPath);
  const statFile = path.join(directoryPath, "stat");
  const statusFile = path.join(directoryPath, "status");
  const fields = ["S", "10", "42", ...Array(16).fill("0"), "901", "0", "0"];
  const running = `${pid} (worker) ${fields.join(" ")}`;
  const zombie = running.replace(") S ", ") Z ");
  const read = fs.readFileSync;
  let statReads = 0;
  fs.readFileSync = function(file, ...args) {
    if (file === statFile) return ++statReads === 1 ? running : zombie;
    if (file === statusFile) return "State: Z (zombie)\n";
    return read.call(this, file, ...args);
  };
  try {
    assert.deepEqual(processRows({ platform: "linux", procRoot }), [{
      pid,
      ppid: 10,
      pgid: 42,
      start: "901",
      rssBytes: 0,
      command: "worker",
    }]);
  } finally {
    fs.readFileSync = read;
  }
});

function linuxExitFixture(flags, threads = "1", rss) {
  const procRoot = directory();
  const pid = 123456;
  const root = path.join(procRoot, String(pid));
  fs.mkdirSync(root);
  const fields = ["R", "10", "42", ...Array(16).fill("0"), "901", "0", "0"];
  fields[6] = flags;
  fields[17] = threads;
  fs.writeFileSync(path.join(root, "stat"), `${pid} (worker) ${fields.join(" ")}`);
  fs.writeFileSync(
    path.join(root, "status"),
    `Threads: ${threads}\n${rss === undefined ? "" : `VmRSS: ${rss} kB\n`}`,
  );
  return { platform: "linux", procRoot };
}

test("Linux single-thread exit can release its mm before zombie state", () => {
  assert.deepEqual(processRows(linuxExitFixture("4")), [{
    pid: 123456,
    ppid: 10,
    pgid: 42,
    start: "901",
    rssBytes: 0,
    command: "worker",
  }]);
});

test("Linux exiting tasks retain a present memory measurement", () => {
  for (const threads of ["0", "1", "2"]) {
    assert.equal(processRows(linuxExitFixture("4", threads, "7"))[0].rssBytes, 7168);
  }
});

test("Linux exit accounting uses flags from the identity-confirmed second stat", () => {
  const options = linuxExitFixture("0");
  const file = path.join(options.procRoot, "123456", "stat");
  const first = fs.readFileSync(file, "utf8");
  const fields = first.slice(first.lastIndexOf(")") + 1).trim().split(/\s+/);
  fields[6] = "4";
  const second = `123456 (worker) ${fields.join(" ")}`;
  const read = fs.readFileSync;
  let statReads = 0;
  fs.readFileSync = function(input, ...args) {
    if (input === file) return ++statReads === 1 ? first : second;
    return read.call(this, input, ...args);
  };
  try {
    assert.equal(processRows(options)[0].rssBytes, 0);
  } finally {
    fs.readFileSync = read;
  }
});

test("Linux exiting tasks cannot turn a malformed RSS field into zero", () => {
  assert.throws(() => processRows(linuxExitFixture("4", "1", "bad")), /unreadable process RSS/);
});

test("Linux missing memory for live or multi-thread tasks fails closed", () => {
  for (const [flags, threads] of [["0", "1"], ["4", "2"], ["4", "0"]]) {
    assert.throws(() => processRows(linuxExitFixture(flags, threads)), /unreadable process RSS/);
  }
});

test("Linux process flags and thread counts require valid unsigned integers", () => {
  for (const flags of ["-1", "4294967296", "4.0", "bad"]) {
    assert.throws(
      () => processRows(linuxExitFixture(flags, "1", "7")),
      /unreadable process identity/,
    );
  }
  assert.throws(
    () => processRows(linuxExitFixture("4", "bad", "7")),
    /unreadable process identity/,
  );
});

test("Linux measurement preserves elapsed and snapshot-size bounds", () => {
  const procRoot = directory();
  const pid = 123456;
  fs.mkdirSync(path.join(procRoot, String(pid)));
  const fields = ["S", "10", "42", ...Array(16).fill("0"), "901", "0", "0"];
  fs.writeFileSync(path.join(procRoot, String(pid), "stat"), `${pid} (worker) ${fields.join(" ")}`);
  fs.writeFileSync(path.join(procRoot, String(pid), "status"), "VmRSS: 7 kB\n");
  let clocks = 0;
  assert.throws(
    () => processRows({ platform: "linux", procRoot, now: () => ++clocks === 1 ? 0 : 1001 }),
    /measurement deadline/,
  );
  assert.throws(
    () => processRows({ platform: "linux", procRoot, maxSnapshotBytes: 1 }),
    /snapshot byte limit/,
  );
});

test("malformed Linux process identity fails closed", () => {
  const procRoot = directory();
  fs.mkdirSync(path.join(procRoot, "123456"));
  fs.writeFileSync(path.join(procRoot, "123456", "stat"), "unreadable identity");
  fs.writeFileSync(path.join(procRoot, "123456", "status"), "VmRSS: 7 kB\n");
  assert.throws(() => processRows({ platform: "linux", procRoot }), /unreadable process identity/);
});

test("normal exit preserves status and bounded evidence", async () => {
  const result = await superviseCase(process.execPath, [
    "-e",
    "setTimeout(() => process.exit(17), 100)",
  ], {
    outputDir: directory(),
    intervalMs: 20,
  });
  assert.equal(result.reason, null);
  assert.equal(result.exit.code, 17);
  assert.ok(result.samples.length <= 360);
});
test("external watchdog stops a child that does not service its event loop", async () => {
  const result = await superviseCase(process.execPath, ["-e", "while (true) {}"], {
    outputDir: directory(),
    intervalMs: 20,
    deadlineMs: 150,
  });
  assert.equal(result.reason, "test deadline exceeded");
  assert.equal(result.exit.signal, "SIGKILL");
});
test("memory limit stops the owned tree before allocating a stress workload", async () => {
  const result = await superviseCase(process.execPath, ["-e", "setInterval(() => {}, 1000)"], {
    outputDir: directory(),
    intervalMs: 20,
    memoryLimit: 1,
  });
  assert.equal(result.reason, "memory limit exceeded");
  assert.ok(result.peakRssBytes > 1);
});

test("a stopped heartbeat is detected independently of a live test process", async () => {
  const outputDir = directory();
  const heartbeatFile = path.join(outputDir, "heartbeat");
  const result = await superviseCase(process.execPath, [
    "-e",
    "require('node:fs').writeFileSync(process.env.DONNER_WATCHDOG_HEARTBEAT, 'ready'); setInterval(() => {}, 1000)",
  ], { outputDir, heartbeatFile, intervalMs: 100 });
  assert.equal(result.reason, "test heartbeat stopped");
  assert.equal(result.exit.signal, "SIGKILL");
});

test("fresh file writes cannot refresh a stale application response", async () => {
  const outputDir = directory();
  const heartbeatFile = path.join(outputDir, "heartbeat");
  const script = `const fs=require('node:fs'); const atMs=Date.now()-6000;
    setInterval(()=>fs.writeFileSync(process.env.DONNER_WATCHDOG_HEARTBEAT,
      JSON.stringify({phase:'active',sessionId:'stale',atMs})),20);`;
  const result = await superviseCase(process.execPath, ["-e", script], {
    outputDir,
    heartbeatFile,
    intervalMs: 20,
  });
  assert.equal(result.reason, "test heartbeat stopped");
  assert.deepEqual(result.cleanup, { groupGone: true, survivors: [] });
});

test("malformed evidence after a live application response fails closed", async () => {
  const outputDir = directory();
  const heartbeatFile = path.join(outputDir, "heartbeat");
  const corrupt = path.join(outputDir, "corrupt");
  fs.writeFileSync(
    heartbeatFile,
    JSON.stringify({ phase: "active", sessionId: "live", atMs: Date.now() }),
  );
  const script = `const fs=require('node:fs');
    setInterval(()=>{ if(fs.existsSync(${JSON.stringify(corrupt)}))
      fs.writeFileSync(process.env.DONNER_WATCHDOG_HEARTBEAT,'{'); },20);`;
  const result = await superviseCase(process.execPath, ["-e", script], {
    outputDir,
    heartbeatFile,
    intervalMs: 20,
    writeReceipt: () => {
      if (fs.existsSync(heartbeatFile)) {
        try {
          if (JSON.parse(fs.readFileSync(heartbeatFile, "utf8")).phase === "active") {
            fs.writeFileSync(corrupt, "observed");
          }
        } catch {}
      }
    },
  });
  assert.ok(result.samples.some((sample) => sample.application?.phase === "active"));
  assert.match(result.reason, /invalid application heartbeat/);
  assert.deepEqual(result.cleanup, { groupGone: true, survivors: [] });
});

for (
  const [name, timestamp] of [["missing", "undefined"], ["future", "Date.now()+60000"], [
    "invalid",
    "'not-a-time'",
  ]]
) {
  test(`an active heartbeat with a ${name} response timestamp fails closed`, async () => {
    const outputDir = directory();
    const heartbeatFile = path.join(outputDir, "heartbeat");
    const script = `const fs=require('node:fs');
      setInterval(()=>fs.writeFileSync(process.env.DONNER_WATCHDOG_HEARTBEAT,
        JSON.stringify({phase:'active',sessionId:'invalid',atMs:${timestamp}})),20);`;
    const result = await superviseCase(process.execPath, ["-e", script], {
      outputDir,
      heartbeatFile,
      intervalMs: 20,
    });
    assert.match(result.reason, /invalid application heartbeat/);
    assert.deepEqual(result.cleanup, { groupGone: true, survivors: [] });
  });
}

test("fast launcher exit cannot orphan its detached child", async () => {
  const outputDir = directory();
  const pidFile = path.join(outputDir, "grandchild.pid");
  const script =
    `const cp=require('node:child_process'); const c=cp.spawn(process.execPath,['-e','setInterval(()=>{},1000)'],{detached:true,stdio:'ignore'}); require('node:fs').writeFileSync(${
      JSON.stringify(pidFile)
    }, String(c.pid)); c.unref();`;
  const result = await superviseCase(process.execPath, ["-e", script], { outputDir });
  assert.equal(result.exit.code, 0);
  assert.deepEqual(result.cleanup, { groupGone: true, survivors: [] });
  assert.throws(() => process.kill(Number(fs.readFileSync(pidFile, "utf8")), 0), { code: "ESRCH" });
});
test("reporting failure still terminates the owned process group", async () => {
  const result = await superviseCase(process.execPath, ["-e", "while(true){}"], {
    outputDir: directory(),
    intervalMs: 20,
    writeReceipt: () => {
      throw new Error("simulated artifact quota");
    },
  });
  assert.match(result.reason, /reporting failed/);
  assert.deepEqual(result.cleanup, { groupGone: true, survivors: [] });
});
test("process measurement failure does not prevent termination", async () => {
  const result = await superviseCase(process.execPath, ["-e", "while(true){}"], {
    outputDir: directory(),
    intervalMs: 20,
    heartbeatLimitMs: 200,
    sampleProcesses: () => {
      throw new Error("simulated ps failure");
    },
  });
  assert.match(result.reason, /measurement failed: simulated ps failure/);
  assert.deepEqual(result.cleanup, { groupGone: true, survivors: [] });
});
test("one failed process sample does not stop a healthy run", async () => {
  // A loaded host can make one `ps` call exceed its timeout.
  let calls = 0;
  const result = await superviseCase(process.execPath, [
    "-e",
    "setTimeout(() => process.exit(17), 300)",
  ], {
    outputDir: directory(),
    intervalMs: 20,
    sampleProcesses: () => {
      if (++calls === 1) throw new Error("simulated ps timeout");
      return processRows();
    },
  });
  assert.equal(result.reason, null);
  assert.equal(result.exit.code, 17);
  assert.equal(result.measurementFailures[0]?.error, "simulated ps timeout");
  assert.ok(result.samples.length > 0, "sampling must resume after the failed sample");
});

test("supervisor termination kills its owned group before returning", async () => {
  const outputDir = directory();
  const timer = setTimeout(() => process.kill(process.pid, "SIGTERM"), 200);
  try {
    const result = await superviseCase(process.execPath, ["-e", "setInterval(() => {}, 1000)"], {
      outputDir,
      intervalMs: 20,
    });
    assert.equal(result.reason, "supervisor received SIGTERM");
    assert.deepEqual(result.cleanup, { groupGone: true, survivors: [] });
  } finally {
    clearTimeout(timer);
  }
});

test("group accounting includes reparented members and excludes unrelated groups", () => {
  const rows = [
    { pid: 20, ppid: 10, pgid: 20, rssBytes: 100 },
    { pid: 21, ppid: 1, pgid: 20, rssBytes: 200 },
    { pid: 22, ppid: 10, pgid: 22, rssBytes: 400 },
  ];
  assert.deepEqual(ownedGroup(rows, 20), rows.slice(0, 2));
});
test("a reparented group member is subject to the RSS limit on its first sample", async () => {
  const result = await superviseCase(process.execPath, ["-e", "setInterval(() => {},1000)"], {
    outputDir: directory(),
    intervalMs: 20,
    memoryLimit: 1024 * 1024 * 1024,
    sampleProcesses: () => {
      const rows = processRows();
      const anchor = rows.find((row) => row.ppid === process.pid && row.pid === row.pgid);
      assert.ok(anchor);
      return [...rows, { pid: -1, ppid: 1, pgid: anchor.pgid, start: "orphan", rssBytes: 2 ** 32 }];
    },
  });
  assert.equal(result.reason, "memory limit exceeded");
  assert.ok(result.peakRssBytes >= 2 ** 32);
  assert.deepEqual(result.cleanup, { groupGone: true, survivors: [] });
});

test("a pending application RPC cannot overwrite teardown after stop", async () => {
  const heartbeat = path.join(directory(), "heartbeat");
  let calls = 0;
  let release;
  let entered;
  const pending = new Promise((resolve) => {
    entered = resolve;
  });
  const worker = {
    evaluate() {
      ++calls;
      if (calls === 1) return Promise.resolve(true);
      if (calls === 4) {
        entered();
        return new Promise((resolve) => {
          release = resolve;
        });
      }
      return Promise.resolve({ textures: 1 });
    },
  };
  const stop = await startApplicationHeartbeat({ workers: () => [worker] }, heartbeat);
  await pending;
  stop();
  const before = JSON.parse(fs.readFileSync(heartbeat, "utf8"));
  assert.equal(before.phase, "teardown");
  release({ textures: 2 });
  await new Promise((resolve) => setTimeout(resolve, 30));
  assert.deepEqual(JSON.parse(fs.readFileSync(heartbeat, "utf8")), before);
});
test("application heartbeat preserves presentation diagnostics without refreshing a stalled app", async () => {
  const heartbeat = path.join(directory(), "heartbeat");
  let calls = 0;
  const worker = {
    evaluate: () => Promise.resolve(++calls === 1 ? true : { pendingSubmissions: 3 }),
  };
  const presentation = {
    queue: { admission: 1, framesInFlight: 3 },
    interaction: { dragging: true },
  };
  const page = { workers: () => [worker], evaluate: () => Promise.resolve(presentation) };
  const stop = await startApplicationHeartbeat(page, heartbeat);
  try {
    const before = JSON.parse(fs.readFileSync(heartbeat, "utf8"));
    assert.deepEqual(before.presentation, presentation);
    worker.evaluate = () => new Promise(() => {});
    // A fixed wait, not a poll: the claim is that nothing changes, so there is no state to wait
    // for. 1700 ms spans at least one 1000 ms pulse.
    await new Promise((resolve) => setTimeout(resolve, 1700));
    assert.deepEqual(JSON.parse(fs.readFileSync(heartbeat, "utf8")), before);
  } finally {
    stop();
  }
});

test("unavailable page diagnostics cannot stop a live application heartbeat", async () => {
  const heartbeat = path.join(directory(), "heartbeat");
  let calls = 0;
  const worker = { evaluate: () => Promise.resolve(++calls === 1 ? true : { textures: 1 }) };
  const page = {
    workers: () => [worker],
    evaluate: () => Promise.reject(new Error("page unavailable")),
  };
  const stop = await startApplicationHeartbeat(page, heartbeat);
  try {
    const before = JSON.parse(fs.readFileSync(heartbeat, "utf8"));
    assert.deepEqual(before.presentation, { unavailable: true });
    const after = await heartbeatWhen(heartbeat, (current) => current.atMs > before.atMs);
    assert.ok(after.atMs > before.atMs, "a later pulse refreshes the heartbeat");
  } finally {
    stop();
  }
});

test("fresh late samples cannot erase a latched teardown deadline", async () => {
  const outputDir = directory();
  const heartbeatFile = path.join(outputDir, "heartbeat");
  const acknowledged = path.join(outputDir, "teardown-observed");
  fs.writeFileSync(
    heartbeatFile,
    JSON.stringify({ phase: "teardown", sessionId: "same", atMs: Date.now() }),
  );
  const writer = createRequire(import.meta.url).resolve("./browser-heartbeat.cjs");
  const script = `const fs=require('node:fs'); const {writeHeartbeat}=require(${
    JSON.stringify(writer)
  });
    setInterval(()=>{if(fs.existsSync(${JSON.stringify(acknowledged)}))
      writeHeartbeat(process.env.DONNER_WATCHDOG_HEARTBEAT,
        {phase:'active',sessionId:'same',atMs:Date.now()});},20);`;
  const result = await superviseCase(process.execPath, ["-e", script], {
    outputDir,
    heartbeatFile,
    intervalMs: 20,
    teardownLimitMs: 200,
    writeReceipt: () => fs.writeFileSync(acknowledged, "observed"),
  });
  assert.equal(result.reason, "browser teardown deadline exceeded");
  assert.deepEqual(result.cleanup, { groupGone: true, survivors: [] });
});
test("a later session's launch ends the previous session's teardown deadline", async () => {
  const outputDir = directory();
  const heartbeatFile = path.join(outputDir, "heartbeat");
  const completed = path.join(outputDir, "second-session-completed");
  const { writeHeartbeat, refreshIdleHeartbeat } = createRequire(import.meta.url)(
    "./browser-heartbeat.cjs",
  );
  const script = `const fs=require('node:fs');
    const wait=setInterval(()=>{
      if(fs.existsSync(${JSON.stringify(completed)})) clearInterval(wait);
    },20);`;
  let samples = 0;
  let secondLaunchAtMs;
  const result = await superviseCase(process.execPath, ["-e", script], {
    outputDir,
    heartbeatFile,
    intervalMs: 20,
    teardownLimitMs: 200,
    sampleProcesses: () => {
      const rows = processRows();
      // Keep child startup and process-listing latency outside the teardown transition.
      if (samples++ === 0) {
        writeHeartbeat(heartbeatFile, { phase: "teardown", sessionId: "first", atMs: Date.now() });
      } else if (secondLaunchAtMs === undefined) {
        secondLaunchAtMs = Date.now();
        writeHeartbeat(heartbeatFile, { phase: "launch", sessionId: "second", atMs: secondLaunchAtMs });
      } else {
        refreshIdleHeartbeat(heartbeatFile);
        if (Date.now() - secondLaunchAtMs >= 600) fs.writeFileSync(completed, "completed");
      }
      return rows;
    },
  });
  assert.equal(result.samples[0]?.application?.phase, "teardown");
  assert.equal(result.samples[0]?.application?.sessionId, "first");
  assert.equal(result.samples[1]?.application?.phase, "launch");
  assert.equal(result.samples[1]?.application?.sessionId, "second");
  assert.ok(result.samples.some((row) =>
    row.application?.phase === "launch" && row.application?.sessionId === "second"
    && row.atMs - result.samples[0].atMs > 200
  ), "the second session must outlive the previous teardown cap");
  assert.equal(result.reason, null);
  assert.equal(result.exit.code, 0);
  assert.deepEqual(result.cleanup, { groupGone: true, survivors: [] });
});
test("termination is issued once even when the stop and exit paths both run", async () => {
  const original = process.kill;
  let terminations = 0;
  process.kill = function(pid, signal) {
    if (pid < 0 && signal === "SIGKILL") ++terminations;
    return original.call(this, pid, signal);
  };
  try {
    const result = await superviseCase(process.execPath, ["-e", "while(true){}"], {
      outputDir: directory(),
      intervalMs: 20,
      deadlineMs: 150,
    });
    assert.equal(result.reason, "test deadline exceeded");
    assert.equal(terminations, 1);
    assert.deepEqual(result.cleanup, { groupGone: true, survivors: [] });
  } finally {
    process.kill = original;
  }
});

// Runs `body` with every signal-zero probe of a process group failing with errno `code`.
async function withFailingGroupProbe(code, body) {
  const original = process.kill;
  process.kill = function(pid, signal) {
    if (pid < 0 && signal === 0) {
      const error = new Error(`${code} signal-zero probe`);
      error.code = code;
      throw error;
    }
    return original.call(this, pid, signal);
  };
  try {
    return await body();
  } finally {
    process.kill = original;
  }
}
function superviseWithDeniedGroupProbe(options, code = "EPERM", args = ["-e", "process.exit(0)"]) {
  return withFailingGroupProbe(code, () => superviseCase(process.execPath, args, {
    outputDir: directory(),
    intervalMs: 20,
    ...options,
  }));
}
test("a denied signal-zero probe verifies absence through process enumeration", async () => {
  const result = await superviseWithDeniedGroupProbe();
  assert.equal(result.reason, null);
  assert.deepEqual(result.cleanup, { groupGone: true, survivors: [] });
});
test("a denied signal-zero probe survives one failed process listing", async () => {
  let calls = 0;
  const result = await superviseWithDeniedGroupProbe({
    cleanupProcesses: () => {
      if (++calls === 1) throw new Error("simulated ps timeout");
      return processRows();
    },
  });
  assert.equal(result.reason, null);
  assert.deepEqual(result.cleanup, { groupGone: true, survivors: [] });
});
test("a denied signal-zero probe without any process listing fails closed", async () => {
  const result = await superviseWithDeniedGroupProbe({
    cleanupProcesses: () => {
      throw new Error("simulated ps failure");
    },
  });
  assert.match(
    result.reason,
    /cleanup verification unavailable: group enumeration failed: simulated/,
  );
  assert.deepEqual(result.cleanup, { groupGone: false, survivors: null });
});

function assertFinalReport(result, listeners) {
  assert.ok(result.cleanup, "cleanup ran to completion");
  const report = JSON.parse(fs.readFileSync(result.reportFile, "utf8"));
  assert.deepEqual(report.cleanup, result.cleanup, "the final receipt records the cleanup");
  assert.equal(report.reason, result.reason);
  assert.deepEqual(["SIGTERM", "SIGINT"].map((signal) => process.listenerCount(signal)), listeners);
}
test("an unexpected signal-zero probe failure fails closed and finishes cleanup", async () => {
  const listeners = ["SIGTERM", "SIGINT"].map((signal) => process.listenerCount(signal));
  const result = await superviseWithDeniedGroupProbe({}, "EINVAL");
  assert.match(
    result.reason,
    /cleanup verification unavailable: group probe failed: EINVAL signal-zero probe/,
  );
  assert.deepEqual(result.cleanup, { groupGone: false, survivors: [] });
  assert.equal(result.exit.code, 0, "the driver's own exit is still reported");
  assertFinalReport(result, listeners);
});
const kUnverifiedProbeFailure = "group probe failed: EINVAL signal-zero probe";
const kUnverifiedProbeReason = `test process cleanup verification unavailable: ${
  kUnverifiedProbeFailure}`;
test("an unexpected signal-zero probe failure keeps a failing driver's exit", async () => {
  const listeners = ["SIGTERM", "SIGINT"].map((signal) => process.listenerCount(signal));
  const result = await superviseWithDeniedGroupProbe({}, "EINVAL", ["-e", "process.exit(3)"]);
  assert.equal(result.exit.code, 3, "the driver's own failure is still reported");
  assert.equal(result.reason, kUnverifiedProbeReason);
  assert.equal(result.reportingError, kUnverifiedProbeFailure);
  assert.deepEqual(result.cleanup, { groupGone: false, survivors: [] });
  assertFinalReport(result, listeners);
});
// Supervises a driver whose anchor emits an error before the driver starts, so the awaited driver
// result rejects, with every signal-zero group probe failing with EINVAL. Resolves to the rejection,
// the anchor's pid and the report path.
async function superviseWithAnchorError() {
  const childProcess = createRequire(import.meta.url)("node:child_process");
  const originalSpawn = childProcess.spawn;
  let anchor;
  childProcess.spawn = function(command, args, ...rest) {
    const child = originalSpawn.call(this, command, args, ...rest);
    if (args?.[0]?.endsWith("browser-watchdog-anchor.mjs")) {
      anchor = child;
      // The driver never starts: this anchor fails before launch. The error is emitted before any
      // I/O callback runs, so it always precedes the anchor's ready message.
      child.send = () => true;
      process.nextTick(() => child.emit("error", new Error("simulated anchor failure")));
    }
    return child;
  };
  // The watchdog imports `spawn` as an ES module binding; publish the replacement to it.
  syncBuiltinESMExports();
  const outputDir = directory();
  try {
    const error = await withFailingGroupProbe("EINVAL", () => superviseCase(
      process.execPath,
      ["-e", "process.exit(0)"],
      { outputDir, intervalMs: 20 },
    )).then(() => null, (rejection) => rejection);
    const reportFile = path.join(outputDir, "browser-watchdog.json");
    return { error, anchorPid: anchor?.pid, reportFile };
  } finally {
    childProcess.spawn = originalSpawn;
    syncBuiltinESMExports();
  }
}
test("an unexpected signal-zero probe failure cannot replace a rejected driver result", async () => {
  const listeners = ["SIGTERM", "SIGINT"].map((signal) => process.listenerCount(signal));
  const { error, anchorPid, reportFile } = await superviseWithAnchorError();
  assert.equal(error?.message, "simulated anchor failure", "the driver's own failure is the result");
  const report = JSON.parse(fs.readFileSync(reportFile, "utf8"));
  assert.equal(report.reason, kUnverifiedProbeReason);
  assert.equal(report.reportingError, kUnverifiedProbeFailure);
  assert.deepEqual(report.cleanup, { groupGone: false, survivors: [] });
  assert.deepEqual(["SIGTERM", "SIGINT"].map((signal) => process.listenerCount(signal)), listeners);
  assert.throws(() => process.kill(-anchorPid, 0), { code: "ESRCH" }, "the group was still ended");
});
// The watchdog command's own exit status, for a driver that exits `driverCode` while every
// signal-zero group probe fails with EINVAL: an unverified cleanup must never exit green.
for (const [name, driverCode] of [["passing", 0], ["failing", 3]]) {
  test(`the watchdog command fails a ${name} driver whose cleanup is unverified`, () => {
    const root = directory();
    const outputDir = path.join(root, "outputs");
    const driver = path.join(root, "driver.sh");
    fs.writeFileSync(driver, `#!/bin/sh\nexit ${driverCode}\n`, { mode: 0o755 });
    const failingProbe = path.join(root, "failing-probe.cjs");
    fs.writeFileSync(failingProbe, `
      const original = process.kill;
      process.kill = function(pid, signal) {
        if (pid < 0 && signal === 0) {
          throw Object.assign(new Error("EINVAL signal-zero probe"), { code: "EINVAL" });
        }
        return original.call(this, pid, signal);
      };
    `);
    const watchdog = fileURLToPath(new URL("./browser-watchdog.mjs", import.meta.url));
    // SIGKILL on timeout: the supervisor's own SIGTERM handler would otherwise absorb it.
    const child = spawnSync(process.execPath, ["--require", failingProbe, watchdog], {
      encoding: "utf8",
      timeout: kStallGuardMs,
      killSignal: "SIGKILL",
      env: {
        ...process.env,
        DONNER_WATCHDOG_DRIVER: driver,
        DONNER_WATCHDOG_SPEC: driver,
        DONNER_WATCHDOG_CONFIG: driver,
        DONNER_WATCHDOG_HEARTBEAT: undefined,
        TEST_UNDECLARED_OUTPUTS_DIR: outputDir,
      },
    });
    assert.equal(child.error, undefined, "the supervisor exits without being killed");
    assert.equal(child.status, 1, child.stderr);
    const reportFile = path.join(outputDir, "browser-watchdog.json");
    assert.ok(fs.existsSync(reportFile), `the final receipt is written\n${child.stderr}`);
    const report = JSON.parse(fs.readFileSync(reportFile, "utf8"));
    assert.equal(report.reason, kUnverifiedProbeReason);
    assert.deepEqual(report.cleanup, { groupGone: false, survivors: [] });
  });
}
// Supervises `args` with every group kill denied, ending the group after the test if it is left.
async function superviseWithDeniedGroupKill(t, args, options) {
  const original = process.kill;
  let deniedGroup;
  // The group may outlive the denied kill if the anchor's disconnect fallback fails; end it after
  // the test.
  t.after(() => {
    if (deniedGroup === undefined) return;
    try {
      original.call(process, deniedGroup, "SIGKILL");
    } catch (error) {
      if (error.code !== "ESRCH") throw error;
    }
  });
  process.kill = function(pid, signal) {
    if (pid < 0 && signal === "SIGKILL") {
      deniedGroup = pid;
      const error = new Error("denied group termination");
      error.code = "EPERM";
      throw error;
    }
    return original.call(this, pid, signal);
  };
  try {
    return await superviseCase(process.execPath, args, {
      outputDir: directory(),
      intervalMs: 20,
      ...options,
    });
  } finally {
    process.kill = original;
  }
}
test("a failed group termination fails closed and still finishes cleanup", async (t) => {
  const listeners = ["SIGTERM", "SIGINT"].map((signal) => process.listenerCount(signal));
  const result = await superviseWithDeniedGroupKill(t, ["-e", "process.exit(0)"]);
  assert.equal(result.reason, "test process group termination failed: denied group termination");
  assert.equal(result.terminationError, "denied group termination");
  assert.equal(result.exit.code, 0, "the driver's own exit is still reported");
  assertFinalReport(result, listeners);
});
test(
  "a failed group termination after a stop does not wait for the driver",
  { timeout: kStallGuardMs },
  async (t) => {
    const listeners = ["SIGTERM", "SIGINT"].map((signal) => process.listenerCount(signal));
    const result = await superviseWithDeniedGroupKill(t, ["-e", "setInterval(() => {}, 1000)"], {
      deadlineMs: 150,
    });
    assert.equal(result.reason, "test deadline exceeded", "the first cause is kept");
    assert.equal(result.terminationError, "denied group termination");
    assert.deepEqual(result.exit, { code: null, signal: null });
    assertFinalReport(result, listeners);
  },
);

// Whether a process group still has a member.
function groupAlive(group) {
  try {
    process.kill(group, 0);
    return true;
  } catch (error) {
    if (error.code === "ESRCH") return false;
    throw error;
  }
}
test("a supervisor whose group kill failed still exits", async (t) => {
  // A separate process, so a handle left open on the surviving anchor would keep it alive.
  const watchdog = new URL("./browser-watchdog.mjs", import.meta.url).href;
  const script = `
    import { supervise } from ${JSON.stringify(watchdog)};
    const original = process.kill;
    process.kill = function(pid, signal) {
      if (pid < 0 && signal === "SIGKILL") {
        console.log(JSON.stringify({ deniedGroup: pid }));
        const error = new Error("denied group termination");
        error.code = "EPERM";
        throw error;
      }
      return original.call(this, pid, signal);
    };
    const result = await supervise(process.execPath, ["-e", "setInterval(() => {}, 1000)"], {
      outputDir: ${JSON.stringify(directory())},
      intervalMs: 20,
      deadlineMs: 150,
    });
    process.exitCode = result.reason ? 1 : 0;
  `;
  const denied = [];
  // The group may outlive the denied kill if the anchor's disconnect fallback fails; end it
  // after the test.
  t.after(() => {
    for (const group of denied) {
      if (groupAlive(group)) process.kill(group, "SIGKILL");
    }
  });
  // SIGKILL on timeout: the supervisor's own SIGTERM handler would otherwise absorb it.
  const child = spawnSync(process.execPath, ["--input-type=module", "-e", script], {
    encoding: "utf8",
    timeout: kStallGuardMs,
    killSignal: "SIGKILL",
  });
  for (const line of child.stdout.split("\n")) {
    if (line.startsWith('{"deniedGroup"')) denied.push(JSON.parse(line).deniedGroup);
  }
  assert.equal(child.error, undefined, "the supervisor exits without being killed");
  assert.equal(child.status, 1, child.stderr);
  assert.equal(denied.length, 1);
  // Disconnecting from the anchor makes it end its own group, the fallback for the failed kill.
  const deadline = Date.now() + kStallGuardMs;
  while (groupAlive(denied[0]) && Date.now() < deadline) {
    await new Promise((resolve) => setTimeout(resolve, 10));
  }
  assert.equal(groupAlive(denied[0]), false, "the anchor ended its group");
});

test("a rejected worker diagnostic remains unavailable for known GPU ownership", async () => {
  const heartbeat = path.join(directory(), "heartbeat");
  let calls = 0;
  const gpu = { textures: 1, textureBytes: 4, pendingSubmissions: 0 };
  const surface = { evaluate: () => Promise.resolve(++calls === 1 ? true : gpu) };
  let otherCalls = 0;
  const owner = {
    evaluate: () => {
      ++otherCalls;
      if (otherCalls === 1) return Promise.resolve(false);
      if (otherCalls === 2) return Promise.resolve(gpu);
      return Promise.reject(new Error("simulated owner RPC failure"));
    },
  };
  const stop = await startApplicationHeartbeat({ workers: () => [surface, owner] }, heartbeat);
  try {
    assert.deepEqual(JSON.parse(fs.readFileSync(heartbeat, "utf8")).workers[1].gpu, gpu);
    const after = await heartbeatWhen(heartbeat, (current) => current.workers[1].unavailable);
    assert.equal(after.workers[1].unavailable, true);
  } finally {
    stop();
  }
});

test("normal Playwright webserver teardown succeeds under supervision", async () => {
  const outputDir = directory();
  const testFile = path.join(outputDir, "teardown.spec.cjs");
  const require = createRequire(import.meta.url);
  fs.writeFileSync(
    testFile,
    `const {test}=require(${
      JSON.stringify(require.resolve("@playwright/test"))
    }); test('fixture without a browser',async()=>{});`,
  );
  const config = path.join(outputDir, "playwright.config.cjs");
  fs.writeFileSync(
    config,
    `module.exports={testDir:${
      JSON.stringify(outputDir)
    },testMatch:'teardown.spec.cjs',workers:1,reporter:'list',webServer:{command:${
      JSON.stringify(
        `${
          JSON.stringify(process.execPath)
        } -e "console.log('SERVER_READY');setInterval(()=>{},1000)"`,
      )
    },wait:{stdout:/SERVER_READY/}}};`,
  );
  const result = await superviseCase(process.execPath, [
    require.resolve("@playwright/test/cli"),
    "test",
    `--config=${config}`,
  ], {
    outputDir,
    intervalMs: 20,
  });
  assert.equal(result.reason, null, `reporting error: ${result.reportingError}`);
  assert.equal(result.exit.code, 0);
  assert.deepEqual(result.cleanup, { groupGone: true, survivors: [] });
});
test("final cleanup enumeration failure cannot report success", async () => {
  const result = await superviseCase(process.execPath, ["-e", "process.exit(0)"], {
    outputDir: directory(),
    intervalMs: 5000,
    cleanupProcesses: () => {
      throw new Error("simulated final verification unavailable");
    },
  });
  // The reason names the failed listing, so a run that fails this way says why.
  assert.equal(
    result.reason,
    "test process cleanup verification unavailable: "
      + "survivor enumeration failed: simulated final verification unavailable",
  );
  assert.equal(result.cleanup.survivors, null);
});
test("one failed final cleanup enumeration is retried", async () => {
  let calls = 0;
  const result = await superviseCase(process.execPath, ["-e", "process.exit(0)"], {
    outputDir: directory(),
    intervalMs: 5000,
    cleanupProcesses: () => {
      if (++calls === 1) throw new Error("simulated ps timeout");
      return processRows();
    },
  });
  assert.equal(result.reason, null);
  assert.deepEqual(result.cleanup, { groupGone: true, survivors: [] });
});
test("final receipt failure cannot report success", async () => {
  const result = await superviseCase(process.execPath, ["-e", "process.exit(0)"], {
    outputDir: directory(),
    intervalMs: 5000,
    writeReceipt: () => {
      throw new Error("simulated final receipt failure");
    },
  });
  assert.notEqual(result.reason, null);
  assert.match(result.reportingError, /final receipt failure/);
});

test("unknown and expired negative targets never forward a process-group signal", () => {
  const forwarded = [];
  let onExit;
  const child = {
    pid: 27,
    once: (event, callback) => {
      if (event === "exit") onExit = callback;
    },
  };
  const childProcess = {
    spawn: () => child,
    fork: () => child,
    execFileSync: () => "27 26 20 Mon Sep 28 00:00:00 2026\n",
  };
  const simulatedProcess = {
    env: { DONNER_WATCHDOG_GROUP: "20" },
    kill: (...args) => {
      forwarded.push(args);
      return true;
    },
  };
  vm.runInNewContext(
    fs.readFileSync(new URL("./browser-watchdog-child.cjs", import.meta.url), "utf8"),
    {
      require: (name) => {
        if (name === "node:child_process") return childProcess;
        if (name === "./browser-process-snapshot.cjs") {
          return {
            processRows: () => [{ pid: 27, ppid: 26, pgid: 20, start: "same-start" }],
            listWithRetries: (list) => list(),
          };
        }
        throw new Error(`unexpected fixture dependency: ${name}`);
      },
      process: simulatedProcess,
    },
  );
  assert.throws(() => simulatedProcess.kill(-91, "SIGKILL"), { code: "ESRCH" });
  childProcess.spawn("test", []);
  onExit();
  assert.throws(() => simulatedProcess.kill(-27, "SIGKILL"), { code: "ESRCH" });
  assert.deepEqual(forwarded, []);
});

test("resource quiescence cannot omit an owner whose first sample failed", () => {
  const surface = { gpu: { pendingSubmissions: 0 }, ageMs: 0 };
  const missingRenderer = { gpu: null, unavailable: true, ageMs: 0 };
  assert.equal(resourceQueuesAreQuiescent([surface, missingRenderer], 0), false);
  assert.equal(resourceQueuesAreQuiescent([surface, { gpu: null, ageMs: 0 }], 0), true);
});
test("resource quiescence requires explicit completed presentation telemetry", () => {
  const surface = { gpu: { pendingSubmissions: 0 }, ageMs: 0 };
  assert.equal(resourceQueuesAreQuiescent([surface], undefined), false);
  assert.equal(resourceQueuesAreQuiescent([surface], 1), false);
  assert.equal(resourceQueuesAreQuiescent([surface], 0), true);
});

test("the browser host capability forwards the bounded launchctl probe", () => {
  const calls = [];
  const actual = browserHostCapability({
    platform: "darwin",
    launchctl: "fixture-launchctl",
    uid: 501,
    execFile: (...args) => calls.push(args),
  });
  assert.deepEqual(actual, { uid: 501, guiNamespaceAvailable: true });
  assert.deepEqual(calls, [
    ["fixture-launchctl", ["print", "gui/501"], { stdio: "ignore", timeout: 1000 }],
  ]);
});

test("failed and expired host capability probes remain unavailable", () => {
  const failures = [
    Object.assign(new Error("probe exited unsuccessfully"), { status: 1 }),
    Object.assign(new Error("probe deadline expired"), { code: "ETIMEDOUT" }),
    Object.assign(new Error("probe executable is missing"), { code: "ENOENT" }),
  ];
  for (const failure of failures) {
    let calls = 0;
    const actual = browserHostCapability({
      platform: "darwin",
      launchctl: "fixture-launchctl",
      uid: 501,
      execFile: () => {
        ++calls;
        throw failure;
      },
    });
    assert.deepEqual(actual, { uid: 501, guiNamespaceAvailable: false });
    assert.equal(calls, 1, failure.message);
  }
});

test("other platforms do not probe a macOS GUI namespace", () => {
  assert.equal(
    browserHostCapability({
      platform: "linux",
      uid: 501,
      execFile: () => assert.fail("a non-macOS host must not invoke launchctl"),
    }),
    null,
  );
});

// Runs `script` under the driver preload in its own process group, with the process listing
// failing on the listed call numbers ("all" fails every call).
function runUnderDriverPreload(script, failingListings) {
  const testDir = fileURLToPath(new URL(".", import.meta.url));
  const stub = path.join(directory(), "failing-listing.cjs");
  fs.writeFileSync(
    stub,
    `const snapshot = require(${
      JSON.stringify(path.join(testDir, "browser-process-snapshot.cjs"))
    });
const list = snapshot.processRows;
const failing = ${JSON.stringify(failingListings)};
let calls = 0;
snapshot.processRows = (...args) => {
  ++calls;
  if (failing === "all" || failing.includes(calls)) throw new Error("simulated ps timeout");
  return list(...args);
};
process.env.DONNER_WATCHDOG_GROUP = String(process.pid);
`,
  );
  const preload = path.join(testDir, "browser-watchdog-child.cjs");
  return new Promise((resolve) => {
    const child = spawn(process.execPath, ["-r", stub, "-r", preload, "-e", script], {
      detached: true,
      stdio: ["ignore", "pipe", "pipe"],
    });
    let output = "";
    child.stdout.on("data", (chunk) => output += chunk);
    child.stderr.on("data", (chunk) => output += chunk);
    child.once("exit", () => {
      // A failed run can leave its own child behind in the group.
      try {
        process.kill(-child.pid, "SIGKILL");
      } catch (error) {
        if (error.code !== "ESRCH") throw error;
      }
    });
    // Output can still arrive after "exit"; "close" follows the last of it.
    child.once("close", (code) => resolve({ code, output }));
  });
}
const kSpawnAndStop = `
const { spawn } = require("node:child_process");
const child = spawn(process.execPath, ["-e", "setInterval(() => {}, 1000)"]);
child.once("exit", (code, signal) => { console.log("child " + signal); process.exit(0); });
setTimeout(() => process.kill(-child.pid, "SIGTERM"), 100);
// Backstop only: the child normally exits within a second.
setTimeout(() => process.exit(3), ${kStallGuardMs});
`;
test("the driver preload tracks a spawned child through one failed process listing", async () => {
  const result = await runUnderDriverPreload(kSpawnAndStop, [1]);
  assert.equal(result.code, 0, result.output);
  assert.match(result.output, /child SIGTERM/);
});
test("the driver preload signals a child group through one failed process listing", async () => {
  const result = await runUnderDriverPreload(kSpawnAndStop, [2]);
  assert.equal(result.code, 0, result.output);
  assert.match(result.output, /child SIGTERM/);
});
test("the driver preload still fails when no process listing succeeds", async () => {
  const result = await runUnderDriverPreload(kSpawnAndStop, "all");
  assert.notEqual(result.code, 0);
  assert.match(result.output, /simulated ps timeout/);
});
