import assert from "node:assert/strict";
import fs from "node:fs";
import { createRequire } from "node:module";
import os from "node:os";
import path from "node:path";
import { test } from "node:test";
import vm from "node:vm";
import { startApplicationHeartbeat } from "./browser-application-heartbeat.mjs";
import { resourceQueuesAreQuiescent } from "./browser-resource-quiescence.mjs";
import { descendants, ownedGroup, processRows, supervise } from "./browser-watchdog.mjs";

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
  const result = await supervise(process.execPath, [
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
  const result = await supervise(process.execPath, ["-e", "while (true) {}"], {
    outputDir: directory(),
    intervalMs: 20,
    deadlineMs: 150,
  });
  assert.equal(result.reason, "test deadline exceeded");
  assert.equal(result.exit.signal, "SIGKILL");
});
test("memory limit stops the owned tree before allocating a stress workload", async () => {
  const result = await supervise(process.execPath, ["-e", "setInterval(() => {}, 1000)"], {
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
  const result = await supervise(process.execPath, [
    "-e",
    "require('node:fs').writeFileSync(process.env.DONNER_WATCHDOG_HEARTBEAT, 'ready'); setInterval(() => {}, 1000)",
  ], { outputDir, heartbeatFile, intervalMs: 100, deadlineMs: 7000 });
  assert.equal(result.reason, "test heartbeat stopped");
  assert.equal(result.exit.signal, "SIGKILL");
});

test("fresh file writes cannot refresh a stale application response", async () => {
  const outputDir = directory();
  const heartbeatFile = path.join(outputDir, "heartbeat");
  const script = `const fs=require('node:fs'); const atMs=Date.now()-6000;
    setInterval(()=>fs.writeFileSync(process.env.DONNER_WATCHDOG_HEARTBEAT,
      JSON.stringify({phase:'active',sessionId:'stale',atMs})),20);`;
  const result = await supervise(process.execPath, ["-e", script], {
    outputDir,
    heartbeatFile,
    intervalMs: 20,
    deadlineMs: 2000,
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
  const result = await supervise(process.execPath, ["-e", script], {
    outputDir,
    heartbeatFile,
    intervalMs: 20,
    deadlineMs: 2000,
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
    const result = await supervise(process.execPath, ["-e", script], {
      outputDir,
      heartbeatFile,
      intervalMs: 20,
      deadlineMs: 2000,
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
  const result = await supervise(process.execPath, ["-e", script], { outputDir });
  assert.equal(result.exit.code, 0);
  assert.deepEqual(result.cleanup, { groupGone: true, survivors: [] });
  assert.throws(() => process.kill(Number(fs.readFileSync(pidFile, "utf8")), 0), { code: "ESRCH" });
});
test("reporting failure still terminates the owned process group", async () => {
  const result = await supervise(process.execPath, ["-e", "while(true){}"], {
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
  const result = await supervise(process.execPath, ["-e", "while(true){}"], {
    outputDir: directory(),
    intervalMs: 20,
    sampleProcesses: () => {
      throw new Error("simulated ps failure");
    },
  });
  assert.match(result.reason, /measurement failed/);
  assert.deepEqual(result.cleanup, { groupGone: true, survivors: [] });
});

test("supervisor termination kills its owned group before returning", async () => {
  const outputDir = directory();
  const timer = setTimeout(() => process.kill(process.pid, "SIGTERM"), 200);
  try {
    const result = await supervise(process.execPath, ["-e", "setInterval(() => {}, 1000)"], {
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
  const result = await supervise(process.execPath, ["-e", "setInterval(() => {},1000)"], {
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
    await new Promise((resolve) => setTimeout(resolve, 1200));
    assert.ok(JSON.parse(fs.readFileSync(heartbeat, "utf8")).atMs > before.atMs);
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
  const result = await supervise(process.execPath, ["-e", script], {
    outputDir,
    heartbeatFile,
    intervalMs: 20,
    teardownLimitMs: 200,
    deadlineMs: 2000,
    writeReceipt: () => fs.writeFileSync(acknowledged, "observed"),
  });
  assert.equal(result.reason, "browser teardown deadline exceeded");
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
    const result = await supervise(process.execPath, ["-e", "while(true){}"], {
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

test("a denied signal-zero probe verifies absence through process enumeration", async () => {
  const original = process.kill;
  process.kill = function(pid, signal) {
    if (pid < 0 && signal === 0) {
      const error = new Error("denied signal-zero probe");
      error.code = "EPERM";
      throw error;
    }
    return original.call(this, pid, signal);
  };
  try {
    const result = await supervise(process.execPath, ["-e", "process.exit(0)"], {
      outputDir: directory(),
      intervalMs: 20,
    });
    assert.equal(result.reason, null);
    assert.deepEqual(result.cleanup, { groupGone: true, survivors: [] });
  } finally {
    process.kill = original;
  }
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
    await new Promise((resolve) => setTimeout(resolve, 1200));
    const unavailable = JSON.parse(fs.readFileSync(heartbeat, "utf8")).workers[1];
    assert.equal(unavailable.unavailable, true);
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
  const result = await supervise(process.execPath, [
    require.resolve("@playwright/test/cli"),
    "test",
    `--config=${config}`,
  ], {
    outputDir,
    intervalMs: 20,
    deadlineMs: 8000,
  });
  assert.equal(result.reason, null);
  assert.equal(result.exit.code, 0);
  assert.deepEqual(result.cleanup, { groupGone: true, survivors: [] });
});
test("final cleanup enumeration failure cannot report success", async () => {
  const result = await supervise(process.execPath, ["-e", "process.exit(0)"], {
    outputDir: directory(),
    intervalMs: 5000,
    cleanupProcesses: () => {
      throw new Error("simulated final verification unavailable");
    },
  });
  assert.notEqual(result.reason, null);
  assert.equal(result.cleanup.survivors, null);
});
test("final receipt failure cannot report success", async () => {
  const result = await supervise(process.execPath, ["-e", "process.exit(0)"], {
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
