import assert from "node:assert/strict";
import { mkdtemp, readdir, readFile, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";
import test from "node:test";
import {
  browserProcessesUnder,
  classifyBrowserProcess,
  collectBrowserStallDiagnostics,
  commandFailure,
  describeCollectionFailure,
  extractCallGraph,
  formatStallSummary,
  kEnableVariable,
  kMaxSampledProcesses,
  kMaxWindowSeconds,
  parseElapsedSeconds,
  parseProcessTable,
  recordBrowserStall,
  runBounded,
  selectSampledProcesses,
  shouldRecordBrowserStall,
  systemLogWindow,
} from "./browser-stall-diagnostics.mjs";

const kApp = "/private/home/runfiles/chrome-mac-arm64/Google Chrome for Testing.app/Contents";
const kHelper = `${kApp}/Frameworks/Google Chrome for Testing Framework.framework/Helpers`;
const kWorkerPid = 500;
// Shaped like `ps -A -ww -o pid=,ppid=,stat=,etime=,time=,rss=,args=` on macOS.
const kProcessTable = [
  `    1     0 Ss   02-03:04:05   1:02.03  12000 /sbin/launchd`,
  `  500   400 S          01:10   0:02.50  90000 node playwright worker`,
  `  610   500 Ss         01:05   0:00.90 150000 ${kApp}/MacOS/Google Chrome for Testing `
  + `--headless --user-data-dir=/private/home/profile --remote-debugging-pipe`,
  `  611   610 U          01:04   0:00.01  20000 ${kHelper}/Google Chrome for Testing Helper (GPU).app/`
  + `Contents/MacOS/Google Chrome for Testing Helper (GPU) --type=gpu-process --use-gl=angle`,
  `  612   610 S          01:04   0:00.20  40000 ${kHelper}/Google Chrome for Testing Helper.app/`
  + `Contents/MacOS/Google Chrome for Testing Helper --type=utility `
  + `--utility-sub-type=network.mojom.NetworkService`,
  `  613   610 S          01:03   0:00.30  60000 ${kHelper}/Google Chrome for Testing Helper `
  + `(Renderer).app/Contents/MacOS/Google Chrome for Testing Helper (Renderer) --type=renderer`,
  `  614   610 S          01:05   0:00.00   3000 ${kApp}/Frameworks/chrome_crashpad_handler --database=/x`,
  `  615   610 S          00:40   0:00.20  70000 ${kHelper}/Google Chrome for Testing Helper `
  + `(Renderer).app/Contents/MacOS/Google Chrome for Testing Helper (Renderer) --type=renderer`,
  `  700   500 R          00:00   0:00.00    900 /bin/ps -A -ww -o pid=,ppid=,stat=,etime=,time=,rss=,args=`,
  `  800     1 S          09:00   0:05.00 200000 ${kApp}/MacOS/Google Chrome for Testing --type=renderer`,
].join("\n");

const kSampleReport = [
  "Analysis of sampling Google Chrome for Testing Helper (GPU) (pid 611) every 1 millisecond",
  `Path:            ${kHelper}/Google Chrome for Testing Helper (GPU).app/Contents/MacOS/x`,
  "Hardware Model:  Mac00,0",
  "----",
  "",
  "Call graph:",
  "    812 Thread_1   DispatchQueue_1: com.apple.main-thread  (serial)",
  "      812 start  (in dyld) + 7184  [0x19489dd54]",
  "        812 mach_msg2_trap  (in libsystem_kernel.dylib) + 8  [0x194bfa3b4]",
  "",
  "Total number in stack (recursive counted multiple, when >=5):",
  "",
  "Sort by top of stack, same collapsed (when >= 5):",
  "        mach_msg2_trap  (in libsystem_kernel.dylib)        812",
  "",
  "Binary Images:",
  `       0x100000000 -        0x100001fff  +helper (0) <UUID> ${kHelper}/helper`,
].join("\n");

async function scratch(name) {
  return mkdtemp(path.join(process.env.TEST_TMPDIR ?? tmpdir(), `${name}-`));
}

test("process table rows keep app paths with spaces intact", () => {
  const rows = parseProcessTable(kProcessTable);
  assert.equal(rows.length, 10);
  assert.deepEqual(
    { ...rows[3], args: rows[3].args.slice(-26) },
    {
      pid: 611,
      ppid: 610,
      stat: "U",
      elapsed: "01:04",
      cpuTime: "0:00.01",
      rssKiB: 20000,
      args: "gpu-process --use-gl=angle",
    },
  );
  assert.deepEqual(parseProcessTable("garbage\n\n  x y z"), []);
});

test("elapsed times parse with optional hours and days", () => {
  assert.deepEqual(
    ["00:07", "01:05", "1:02:03", "02-03:04:05", "bad", "1:2:3:4"].map(parseElapsedSeconds),
    [7, 65, 3723, 183845, null, null],
  );
});

test("process roles come from fixed command-line switches only", () => {
  assert.deepEqual(
    [
      classifyBrowserProcess("x --type=gpu-process --foo", false),
      classifyBrowserProcess("x --type=renderer", false),
      classifyBrowserProcess(
        "x --type=utility --utility-sub-type=network.mojom.NetworkService",
        false,
      ),
      classifyBrowserProcess("x --type=utility --utility-sub-type=/private/path", false),
      classifyBrowserProcess("x/chrome_crashpad_handler --database=/x", false),
      classifyBrowserProcess("x --headless", true),
      classifyBrowserProcess("x --headless", false),
    ],
    [
      "gpu-process",
      "renderer",
      "utility:network.mojom.NetworkService",
      "utility",
      "other",
      "browser",
      "other",
    ],
  );
});

test("only the worker's descendants are reported, without command lines", () => {
  const processes = browserProcessesUnder(
    parseProcessTable(kProcessTable),
    kWorkerPid,
    new Set([700]),
  );
  assert.deepEqual(
    processes.map(({ kind, pid, stat }) => [kind, pid, stat]),
    [
      ["browser", 610, "Ss"],
      ["gpu-process", 611, "U"],
      ["utility:network.mojom.NetworkService", 612, "S"],
      ["renderer", 613, "S"],
      ["other", 614, "S"],
      ["renderer", 615, "S"],
    ],
  );
  assert.doesNotMatch(JSON.stringify(processes), /private|--/);
});

test("the browser, GPU and every renderer are sampled, within the limit", () => {
  const kinds = ["utility", "renderer", "other", "gpu-process", "browser", "renderer", "a", "b"];
  const selected = selectSampledProcesses(kinds.map((kind, pid) => ({ kind, pid })));
  assert.deepEqual(
    selected.map(({ kind, pid }) => `${kind}:${pid}`),
    ["browser:4", "gpu-process:3", "renderer:1", "renderer:5"],
  );
  const many = [
    "renderer",
    "renderer",
    "gpu-process",
    "renderer",
    "browser",
    "renderer",
    "renderer",
  ]
    .map((kind, pid) => ({ kind, pid }));
  assert.deepEqual(
    selectSampledProcesses(many).map(({ kind, pid }) => `${kind}:${pid}`),
    ["browser:4", "gpu-process:2", "renderer:0", "renderer:1", "renderer:3", "renderer:5"],
  );
  assert.equal(kMaxSampledProcesses, 6);
});

test("call graphs drop the report header and binary image paths", () => {
  const graph = extractCallGraph(kSampleReport);
  assert.equal(graph?.truncated, false);
  assert.match(graph?.text ?? "", /^Call graph:\n/);
  assert.match(graph?.text ?? "", /Sort by top of stack/);
  assert.doesNotMatch(graph?.text ?? "", /private|Hardware|Path:|Binary Images/);
  assert.equal(extractCallGraph("sample: unable to examine process"), null);

  const bounded = extractCallGraph(`Call graph:\n${"x".repeat(100)}\nBinary Images:\n`, 32);
  assert.deepEqual(
    [bounded?.truncated, Buffer.byteLength(bounded?.text ?? "")],
    [true, 32],
  );
  // "Call graph:\n" is 12 bytes; a 32-byte cut lands inside the seventh three-byte character.
  const wide = extractCallGraph(`Call graph:\n${"\u2500".repeat(30)}\n`, 32);
  assert.deepEqual([wide?.truncated, wide?.text], [true, `Call graph:\n${"\u2500".repeat(6)}`]);
});

test("failed commands are described without their command line", async () => {
  const directory = await scratch("stall-commands");
  const results = await Promise.all([
    runBounded(process.execPath, ["-e", "process.exit(3)", directory], 10_000).done,
    runBounded(process.execPath, ["-e", "setTimeout(() => {}, 60000)", directory], 200).done,
    runBounded(path.join(directory, "missing-tool"), [directory], 10_000).done,
    runBounded(process.execPath, ["-e", "process.stdout.write('ok')"], 10_000).done,
  ]);
  assert.deepEqual(results, [
    { ok: false, stdout: "", error: "exit:3" },
    { ok: false, stdout: "", error: "timeout" },
    { ok: false, stdout: "", error: "error:ENOENT" },
    { ok: true, stdout: "ok", error: "" },
  ]);
});

test("the system log window covers browser launch and stays bounded", () => {
  const collected = Date.parse("2026-10-04T03:25:30.400Z");
  const finished = collected + 4_000;
  assert.equal(
    systemLogWindow(collected, finished, 30_000, 45),
    "2026-10-04T03:24:35Z 2026-10-04T03:25:35Z",
  );
  assert.equal(
    systemLogWindow(collected, finished, 30_000, null),
    "2026-10-04T03:24:50Z 2026-10-04T03:25:35Z",
  );
  const [start] = systemLogWindow(collected, finished, 10 * 3600_000, 99_999).split(" ");
  assert.equal(Math.round((collected - Date.parse(start)) / 1000), kMaxWindowSeconds);
});

test("collection writes a bounded, path-free record and removes raw reports", async () => {
  const outputDirectory = await scratch("stall-out");
  const scratchDirectory = await scratch("stall-raw");
  const calls = [];
  const run = (file, args) => {
    calls.push(path.basename(file));
    if (file === "/bin/ps") {
      return { pid: 700, done: Promise.resolve({ ok: true, stdout: kProcessTable, error: "" }) };
    }
    if (file === "/usr/sbin/sysctl") {
      return {
        pid: 701,
        done: Promise.resolve({ ok: true, stdout: "{ 2.50 2.10 1.90 }\n42\n", error: "" }),
      };
    }
    const pid = Number(args[0]);
    const rawPath = args[args.indexOf("-file") + 1];
    // Node's message for a failed command repeats its full command line, output path included.
    const refused = Object.assign(
      new Error(`Command failed: /usr/bin/sample ${args.join(" ")}\nsample: cannot examine`),
      { code: 1 },
    );
    const done = pid === 613
      ? Promise.resolve({ ok: false, stdout: "", error: commandFailure(refused) })
      : writeFile(rawPath, kSampleReport, "utf8").then(() => ({ ok: true, stdout: "", error: "" }));
    return { pid: 900 + pid, done };
  };
  let clock = Date.parse("2026-10-04T03:25:30.000Z");
  const summary = await collectBrowserStallDiagnostics({
    rootPid: kWorkerPid,
    outputDirectory,
    scratchDirectory,
    test: { title: "first browser command", status: "timedOut", durationMs: 30_000 },
    run,
    now: () => (clock += 2_000),
  });

  assert.deepEqual(calls.slice(0, 2), ["ps", "sysctl"]);
  assert.deepEqual(
    summary.samples.map(({ kind, file, error }) => [kind, file, error ?? null]),
    [
      ["browser", "sample-browser-610.txt", null],
      ["gpu-process", "sample-gpu-process-611.txt", null],
      ["renderer", null, "exit:1"],
      ["renderer", "sample-renderer-615.txt", null],
    ],
  );
  assert.deepEqual([summary.loadAverage, summary.memoryStatusLevel], ["{ 2.50 2.10 1.90 }", "42"]);
  assert.deepEqual(await readdir(scratchDirectory), []);

  const written = (await readdir(outputDirectory)).sort();
  assert.deepEqual(written, [
    "sample-browser-610.txt",
    "sample-gpu-process-611.txt",
    "sample-renderer-615.txt",
    "summary.json",
    "system-log-window.txt",
  ]);
  for (const name of written) {
    const text = await readFile(path.join(outputDirectory, name), "utf8");
    assert.doesNotMatch(
      text,
      /private|Hardware|--type|Command failed/,
      `${name} must not carry host paths, hardware or command lines`,
    );
    assert.equal(text.includes(scratchDirectory), false, `${name} must not name the scratch path`);
  }
  assert.match(
    await readFile(path.join(outputDirectory, "system-log-window.txt"), "utf8"),
    /^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ \d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ\n$/,
  );

  const lines = formatStallSummary(summary);
  assert.match(
    lines[0],
    /^browser-stall-diagnostics loadavg="\{ 2\.50 2\.10 1\.90 \}" memorystatus=42$/,
  );
  assert.equal(
    lines[2],
    "browser-stall-diagnostics process kind=gpu-process pid=611 ppid=610 stat=U elapsed=01:04 "
      + "cpu=0:00.01 rssKiB=20000",
  );
  assert.match(lines.at(-1) ?? "", /^browser-stall-diagnostics callGraphs=3\/4 window=/);
});

// The error Playwright 1.62 reports when a pending expect.poll reaches the test deadline,
// as the hosted macOS lane printed it for a stalled WebGPU adapter request. The test status
// is "failed", not "timedOut", because the poll gives up 250 ms before the deadline.
const kPollCutOffByTestDeadline = [
  "Error: expected all five catalog SVGs to publish real Donner-rendered thumbnails",
  "",
  "expect(received).toBe(expected) // Object.is equality",
  "",
  "Expected: 5",
  "Received: 0",
  "",
  "Call Log:",
  "- Test timeout of 30000ms exceeded",
].join("\n");

test("only a macOS test that ended at its deadline in the hosted job is recorded", () => {
  const enabled = { [kEnableVariable]: "1" };
  const pollCutOff = {
    status: "failed",
    timeout: 30_000,
    errors: [{ message: kPollCutOffByTestDeadline }],
  };
  const timedOut = { status: "timedOut", timeout: 30_000, errors: [] };
  const assertion = { status: "failed", timeout: 30_000, errors: [{ message: "Expected: 5" }] };
  const otherDeadline = {
    status: "failed",
    timeout: 60_000,
    errors: [{ message: kPollCutOffByTestDeadline }],
  };
  const cases = [
    ["darwin", timedOut, enabled],
    ["darwin", pollCutOff, enabled],
    ["darwin", timedOut, {}],
    ["darwin", pollCutOff, { [kEnableVariable]: "0" }],
    ["darwin", assertion, enabled],
    ["darwin", otherDeadline, enabled],
    ["darwin", { status: "passed", timeout: 30_000, errors: [] }, enabled],
    ["darwin", { status: "interrupted", timeout: 30_000, errors: [] }, enabled],
    ["linux", pollCutOff, enabled],
  ];
  assert.deepEqual(
    cases.map(([platform, testInfo, env]) => shouldRecordBrowserStall({ platform, testInfo, env })),
    [true, true, false, false, false, false, false, false, false],
  );
});

function deadlineTestInfo(outputRoot) {
  return {
    title: "first browser command",
    status: "failed",
    timeout: 30_000,
    errors: [{ message: kPollCutOffByTestDeadline }],
    duration: 30_000,
    outputPath: (name) => path.join(outputRoot, name),
  };
}

test("without the hosted switch the recorder collects and prints nothing", async () => {
  const outputRoot = await scratch("stall-inert");
  const printed = [];
  let collected = 0;
  const lines = await recordBrowserStall(deadlineTestInfo(outputRoot), {
    platform: "darwin",
    env: { TEST_TMPDIR: outputRoot },
    log: (line) => printed.push(line),
    collect: async () => {
      ++collected;
      throw new Error("must not run");
    },
  });
  assert.deepEqual([lines, printed, collected, await readdir(outputRoot)], [[], [], 0, []]);
});

test("a failed collection prints only a fixed code", async () => {
  const outputRoot = await scratch("stall-error");
  const printed = [];
  const failure = Object.assign(
    new Error(`EACCES: permission denied, open '${outputRoot}/browser-stall/summary.json'`),
    { code: "EACCES" },
  );
  await recordBrowserStall(deadlineTestInfo(outputRoot), {
    platform: "darwin",
    env: { [kEnableVariable]: "1", TEST_TMPDIR: outputRoot },
    log: (line) => printed.push(line),
    collect: async () => {
      throw failure;
    },
  });
  assert.deepEqual(printed, [
    "browser-stall-diagnostics deadline status=failed durationMs=30000",
    "browser-stall-diagnostics failed: EACCES",
  ]);
  assert.deepEqual(
    [describeCollectionFailure(new Error(outputRoot)), describeCollectionFailure(null)],
    ["browser-stall-diagnostics failed: error", "browser-stall-diagnostics failed: error"],
  );
});

test("collection ends at its budget even when a command never finishes", async () => {
  const outputRoot = await scratch("stall-budget");
  const printed = [];
  const never = () => ({ pid: undefined, done: new Promise(() => {}) });
  const started = Date.now();
  await recordBrowserStall(deadlineTestInfo(outputRoot), {
    platform: "darwin",
    env: { [kEnableVariable]: "1", TEST_TMPDIR: outputRoot },
    log: (line) => printed.push(line),
    collect: (options) => collectBrowserStallDiagnostics({ ...options, run: never }),
    budgetMs: 200,
  });
  const elapsedMs = Date.now() - started;
  // The marker selects the CI comparison reruns and the window drives the system log slice,
  // so both must survive a collection that overruns its budget.
  assert.deepEqual(printed, [
    "browser-stall-diagnostics deadline status=failed durationMs=30000",
    "browser-stall-diagnostics failed: collection-timeout",
  ]);
  assert.ok(elapsedMs < 5_000, `collection took ${elapsedMs} ms`);
  const outputDirectory = path.join(outputRoot, "browser-stall");
  assert.deepEqual(await readdir(outputDirectory), ["system-log-window.txt"]);
  const window = await readFile(path.join(outputDirectory, "system-log-window.txt"), "utf8");
  assert.match(window, /^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ \d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ\n$/);
  const [start, end] = window.trim().split(" ").map(Date.parse);
  // Covers the test, an assumed launch before it, and the whole collection budget.
  assert.ok(start <= started - 30_000 - 60_000, window);
  assert.ok(end >= started + 200 - 1_000, window);
});

test("a collection that outlives its budget writes nothing more", async () => {
  const outputRoot = await scratch("stall-late");
  const printed = [];
  let releaseTable;
  const tableReleased = new Promise((resolve) => {
    releaseTable = resolve;
  });
  const sampled = [];
  const late = (file) => {
    if (file === "/bin/ps") {
      return {
        pid: 700,
        done: tableReleased.then(() => ({ ok: true, stdout: kProcessTable, error: "" })),
      };
    }
    sampled.push(file);
    return { pid: 701, done: Promise.resolve({ ok: true, stdout: "", error: "" }) };
  };
  let finished;
  await recordBrowserStall(deadlineTestInfo(outputRoot), {
    platform: "darwin",
    env: { [kEnableVariable]: "1", TEST_TMPDIR: outputRoot },
    log: (line) => printed.push(line),
    collect: (options) => {
      finished = collectBrowserStallDiagnostics({ ...options, run: late }).catch((error) =>
        error.code
      );
      return finished;
    },
    budgetMs: 100,
  });
  const outputDirectory = path.join(outputRoot, "browser-stall");
  const windowAtDeadline = await readFile(path.join(outputDirectory, "system-log-window.txt"));
  releaseTable();
  assert.equal(await finished, "collection-timeout");
  assert.deepEqual(
    [printed.at(-1), sampled, await readdir(outputDirectory)],
    ["browser-stall-diagnostics failed: collection-timeout", [], ["system-log-window.txt"]],
  );
  assert.deepEqual(
    await readFile(path.join(outputDirectory, "system-log-window.txt")),
    windowAtDeadline,
  );
});

test("each Chromium regression spec installs the recorder through its hook", async () => {
  const hook = await readFile(new URL("./browser-stall-diagnostics.ts", import.meta.url), "utf8");
  assert.match(
    hook,
    /test\.afterEach\(async \(\) => \{\n\s+await recordBrowserStall\(test\.info\(\)\);\n\s+\}\);/,
  );
  for (
    const spec of [
      "browser-presentation-regression.spec.ts",
      "smoke.spec.ts",
      "standalone-geode-browser-renderer.spec.ts",
    ]
  ) {
    const source = await readFile(new URL(`./${spec}`, import.meta.url), "utf8");
    assert.match(
      source,
      /^import \{ installBrowserStallDiagnostics \} from "\.\/browser-stall-diagnostics";$/m,
      `${spec} must import the recorder hook`,
    );
    assert.match(source, /^installBrowserStallDiagnostics\(test\);$/m, `${spec} must install it`);
  }
});
