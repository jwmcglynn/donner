import { execFile } from "node:child_process";
import { mkdir, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";

/** Prefix of every log line the collector prints; CI selects follow-up probes on it. */
export const kStallMarker = "browser-stall-diagnostics";
/**
 * Test environment switch that enables the recorder. Only the ephemeral hosted macOS CI job sets
 * it: load, memory and call graphs describe the host, so a persistent host never publishes them.
 */
export const kEnableVariable = "DONNER_BROWSER_STALL_DIAGNOSTICS";
/** Most processes sampled per timeout: the browser, the GPU process and every renderer. */
export const kMaxSampledProcesses = 6;
/** Deadline for the whole collection, whatever state its child processes are left in. */
export const kCollectionBudgetMs = 15_000;
/** Upper bound on each retained call graph. */
export const kMaxCallGraphBytes = 512 * 1024;
/** Oldest system log start relative to collection, so the CI log slice stays bounded. */
export const kMaxWindowSeconds = 600;

const kSampleSeconds = 1;
// The process table and the parallel samples together stay well inside Playwright's 30 s
// after-hooks budget, which the page and context teardown share.
const kSampleTimeoutMs = 9_000;
const kQuickCommandTimeoutMs = 3_000;
const kWindowPaddingSeconds = 10;
// Until the process table gives the browser's age, assume it launched this long before the test.
const kAssumedLaunchSeconds = 60;
const kWindowFile = "system-log-window.txt";
const kSampledKinds = ["browser", "gpu-process", "renderer"];

/**
 * Parse `ps -A -ww -o pid=,ppid=,stat=,etime=,time=,rss=,args=` output.
 * @param {string} text
 * @returns {Array<{pid: number, ppid: number, stat: string, elapsed: string,
 *   cpuTime: string, rssKiB: number, args: string}>}
 */
export function parseProcessTable(text) {
  const rows = [];
  for (const line of text.split("\n")) {
    const match = /^\s*(\d+)\s+(\d+)\s+(\S+)\s+([\d:-]+)\s+([\d:.-]+)\s+(\d+)\s+(.*)$/.exec(line);
    if (match) {
      rows.push({
        pid: Number(match[1]),
        ppid: Number(match[2]),
        stat: match[3],
        elapsed: match[4],
        cpuTime: match[5],
        rssKiB: Number(match[6]),
        args: match[7],
      });
    }
  }
  return rows;
}

/**
 * Seconds in a `ps` elapsed time of the form `[[dd-]hh:]mm:ss`, or null when malformed.
 * @param {string} elapsed
 * @returns {number | null}
 */
export function parseElapsedSeconds(elapsed) {
  const match = /^(?:(?:(\d+)-)?(\d+):)?(\d+):(\d+)$/.exec(elapsed);
  if (!match) {
    return null;
  }
  const [days, hours, minutes, seconds] = match.slice(1).map((part) => Number(part ?? 0));
  return ((days * 24 + hours) * 60 + minutes) * 60 + seconds;
}

/**
 * A fixed process role taken from the command line. Only this label leaves the
 * collector; command lines carry profile paths and are never written out.
 * @param {string} args
 * @param {boolean} directChild whether the process is a direct child of the test worker
 * @returns {string}
 */
export function classifyBrowserProcess(args, directChild) {
  const type = /(?:^|\s)--type=([A-Za-z0-9_.-]+)/.exec(args)?.[1];
  if (type === "utility") {
    const subType = /(?:^|\s)--utility-sub-type=([A-Za-z0-9_.]+)/.exec(args)?.[1];
    return subType ? `utility:${subType}` : "utility";
  }
  if (type) {
    return type;
  }
  return directChild ? "browser" : "other";
}

/**
 * Every descendant of `rootPid`, labelled, without command lines.
 * @param {ReturnType<typeof parseProcessTable>} rows
 * @param {number} rootPid
 * @param {Set<number>} excludedPids processes the collector itself started
 */
export function browserProcessesUnder(rows, rootPid, excludedPids = new Set()) {
  const descendants = new Set([rootPid]);
  let grew = true;
  while (grew) {
    grew = false;
    for (const row of rows) {
      if (!descendants.has(row.pid) && descendants.has(row.ppid)) {
        descendants.add(row.pid);
        grew = true;
      }
    }
  }
  return rows
    .filter((row) => row.pid !== rootPid && descendants.has(row.pid) && !excludedPids.has(row.pid))
    .map(({ pid, ppid, stat, elapsed, cpuTime, rssKiB, args }) => ({
      kind: classifyBrowserProcess(args, ppid === rootPid),
      pid,
      ppid,
      stat,
      elapsed,
      cpuTime,
      rssKiB,
    }));
}

/**
 * The processes to sample: the browser, its GPU process and every renderer, since the stalled
 * page's renderer is not necessarily the first one listed; at most `limit`.
 * @template {{kind: string}} T
 * @param {T[]} processes
 * @param {number} limit
 * @returns {T[]}
 */
export function selectSampledProcesses(processes, limit = kMaxSampledProcesses) {
  return processes
    .map((entry, order) => ({ entry, order, rank: kSampledKinds.indexOf(entry.kind) }))
    .filter(({ rank }) => rank >= 0)
    .sort((a, b) => a.rank - b.rank || a.order - b.order)
    .slice(0, limit)
    .map(({ entry }) => entry);
}

/**
 * The thread call graph and top-of-stack summary of a macOS `sample` report.
 * The header and binary image list are dropped because they name host paths and
 * hardware; what remains is symbol names and the libraries that own them.
 * @param {string} report
 * @param {number} maxBytes
 * @returns {{text: string, truncated: boolean} | null}
 */
export function extractCallGraph(report, maxBytes = kMaxCallGraphBytes) {
  const start = report.indexOf("Call graph:");
  if (start < 0) {
    return null;
  }
  const end = report.indexOf("\nBinary Images:", start);
  const section = report.slice(start, end < 0 ? report.length : end).trimEnd() + "\n";
  const bytes = Buffer.from(section, "utf8");
  if (bytes.length <= maxBytes) {
    return { text: section, truncated: false };
  }
  let cut = maxBytes;
  // Cut at a character boundary: back off over UTF-8 continuation bytes.
  while (cut > 0 && (bytes[cut] & 0xc0) === 0x80) {
    --cut;
  }
  return { text: bytes.subarray(0, cut).toString("utf8"), truncated: true };
}

/**
 * The UTC window a CI system log slice should cover: from before the browser
 * launched (or the test began, whichever is older) to the end of collection,
 * bounded to `kMaxWindowSeconds`.
 * @param {number} collectedAtMs when collection started
 * @param {number} finishedAtMs when collection finished
 * @param {number} testDurationMs
 * @param {number | null} browserElapsedSeconds
 * @returns {string} `<start> <end>` as second-resolution ISO times
 */
export function systemLogWindow(
  collectedAtMs,
  finishedAtMs,
  testDurationMs,
  browserElapsedSeconds,
) {
  const lookbackSeconds = Math.min(
    kMaxWindowSeconds,
    Math.max(testDurationMs / 1000, browserElapsedSeconds ?? 0) + kWindowPaddingSeconds,
  );
  const iso = (ms) => new Date(Math.floor(ms / 1000) * 1000).toISOString().replace(".000Z", "Z");
  return `${iso(collectedAtMs - lookbackSeconds * 1000)} ${iso(finishedAtMs + 1000)}`;
}

/**
 * A fixed description of a failed command. Node's own message repeats the full
 * command line, which names output paths, so it is never kept.
 * @param {(Error & {killed?: boolean, signal?: string | null, code?: unknown}) | null} error
 * @returns {string}
 */
export function commandFailure(error) {
  if (!error) {
    return "";
  }
  if (error.killed) {
    return "timeout";
  }
  if (error.signal) {
    return `signal:${String(error.signal).replaceAll(/[^A-Z0-9]/g, "")}`;
  }
  if (typeof error.code === "number") {
    return `exit:${error.code}`;
  }
  if (typeof error.code === "string") {
    return `error:${error.code.replaceAll(/[^A-Z0-9_]/g, "")}`;
  }
  return "error";
}

/**
 * Run a command with a deadline, never rejecting.
 * @param {string} file
 * @param {string[]} args
 * @param {number} timeoutMs
 * @returns {{pid: number | undefined, done: Promise<{ok: boolean, stdout: string, error: string}>}}
 */
export function runBounded(file, args, timeoutMs) {
  let resolveDone;
  const done = new Promise((resolve) => {
    resolveDone = resolve;
  });
  const child = execFile(
    file,
    args,
    { timeout: timeoutMs, killSignal: "SIGKILL", maxBuffer: 16 * 1024 * 1024, encoding: "utf8" },
    (error, stdout) => {
      resolveDone({ ok: !error, stdout: stdout ?? "", error: commandFailure(error) });
    },
  );
  return { pid: child.pid, done };
}

/**
 * Write an output file unless the collection was abandoned. Once the budget expires the hook
 * returns and teardown proceeds, so a collection still running in the background must not
 * replace or add files: the outputs then hold exactly what was written before the deadline.
 */
async function writeOutput(signal, file, text) {
  if (signal?.aborted) {
    return false;
  }
  await writeFile(file, text, "utf8");
  return true;
}

async function sampleProcess(target, outputDirectory, scratchDirectory, run, signal) {
  const name = `sample-${target.kind.replaceAll(/[^A-Za-z0-9_.-]/g, "_")}-${target.pid}.txt`;
  // The full report names host paths and hardware, so it stays out of the outputs.
  const rawPath = path.join(scratchDirectory, `${name}.raw`);
  const result = await run(
    "/usr/bin/sample",
    [String(target.pid), String(kSampleSeconds), "-mayDie", "-file", rawPath],
    kSampleTimeoutMs,
  ).done;
  const report = await readFile(rawPath, "utf8").catch(() => "");
  await rm(rawPath, { force: true });
  const callGraph = extractCallGraph(report);
  if (!callGraph) {
    return {
      pid: target.pid,
      kind: target.kind,
      file: null,
      error: result.error || "no call graph",
    };
  }
  const written = await writeOutput(signal, path.join(outputDirectory, name), callGraph.text);
  return {
    pid: target.pid,
    kind: target.kind,
    file: written ? name : null,
    truncated: callGraph.truncated,
  };
}

/**
 * Record the state of the browser processes a failed test leaves behind, while
 * they are still alive: each process's role, scheduler state, age, CPU time and
 * memory, a short call graph of the browser, GPU and renderer processes, and
 * host load.
 *
 * The window a CI system log slice should cover is written first, before any command
 * runs, with an end that covers the whole collection budget, so a collection that
 * overruns its budget on a loaded host still yields the log slice. It is narrowed to
 * the browser's real launch time after the process table and to the exact end last.
 *
 * Bounded by construction: at most `kMaxSampledProcesses` call graphs of at most
 * `kMaxCallGraphBytes` each, and every command has a deadline. Nothing is written
 * after `signal` aborts.
 *
 * @param {{rootPid: number, outputDirectory: string, scratchDirectory: string,
 *   test: {title: string, status: string, durationMs: number},
 *   run?: typeof runBounded, now?: () => number, signal?: AbortSignal,
 *   budgetMs?: number}} options
 */
export async function collectBrowserStallDiagnostics({
  rootPid,
  outputDirectory,
  scratchDirectory,
  test,
  run = runBounded,
  now = Date.now,
  signal,
  budgetMs = kCollectionBudgetMs,
}) {
  const collectedAtMs = now();
  const windowPath = path.join(outputDirectory, kWindowFile);
  await mkdir(outputDirectory, { recursive: true });
  await writeOutput(
    signal,
    windowPath,
    `${
      systemLogWindow(
        collectedAtMs,
        collectedAtMs + budgetMs,
        test.durationMs,
        test.durationMs / 1000 + kAssumedLaunchSeconds,
      )
    }\n`,
  );
  await mkdir(scratchDirectory, { recursive: true });
  const ps = run(
    "/bin/ps",
    ["-A", "-ww", "-o", "pid=,ppid=,stat=,etime=,time=,rss=,args="],
    kQuickCommandTimeoutMs,
  );
  const table = await ps.done;
  // Read before the collector starts any other child, which would join the worker's tree.
  const processes = browserProcessesUnder(
    parseProcessTable(table.stdout),
    rootPid,
    new Set(ps.pid === undefined ? [] : [ps.pid]),
  );
  const browserElapsed = processes.find((entry) => entry.kind === "browser")?.elapsed;
  const browserElapsedSeconds = browserElapsed === undefined
    ? null
    : parseElapsedSeconds(browserElapsed);
  await writeOutput(
    signal,
    windowPath,
    `${
      systemLogWindow(
        collectedAtMs,
        collectedAtMs + budgetMs,
        test.durationMs,
        browserElapsedSeconds,
      )
    }\n`,
  );
  if (signal?.aborted) {
    throw abandonedError();
  }
  const load = run(
    "/usr/sbin/sysctl",
    ["-n", "vm.loadavg", "kern.memorystatus_level"],
    kQuickCommandTimeoutMs,
  ).done;
  const samples = await Promise.all(
    selectSampledProcesses(processes).map((target) =>
      sampleProcess(target, outputDirectory, scratchDirectory, run, signal)
    ),
  );
  const loadResult = await load;
  const [loadAverage = null, memoryStatusLevel = null] = loadResult.ok
    ? loadResult.stdout.trim().split("\n").map((line) => line.trim())
    : [];
  const window = systemLogWindow(collectedAtMs, now(), test.durationMs, browserElapsedSeconds);
  const summary = {
    collectedAt: new Date(collectedAtMs).toISOString(),
    test,
    processTable: table.ok ? "ok" : table.error,
    loadAverage,
    memoryStatusLevel,
    processes,
    samples,
    systemLogWindow: window,
  };
  await writeOutput(
    signal,
    path.join(outputDirectory, "summary.json"),
    JSON.stringify(summary, null, 2),
  );
  await writeOutput(signal, windowPath, `${window}\n`);
  if (signal?.aborted) {
    throw abandonedError();
  }
  return summary;
}

function abandonedError() {
  return Object.assign(new Error("collection budget"), { code: "collection-timeout" });
}

/**
 * The line printed before collection starts. CI selects the browser comparison reruns on
 * it, so it is printed even when collection later fails or overruns its budget.
 * @param {{status: string, durationMs: number}} test
 * @returns {string}
 */
export function formatStallMarker(test) {
  return `${kStallMarker} deadline status=${test.status} durationMs=${test.durationMs}`;
}

/**
 * The fixed-format lines printed to the test log, so the job log shows the
 * process states without downloading the artifact.
 * @param {Awaited<ReturnType<typeof collectBrowserStallDiagnostics>>} summary
 * @returns {string[]}
 */
export function formatStallSummary(summary) {
  const sampled = summary.samples.filter((sample) => sample.file).length;
  return [
    `${kStallMarker} loadavg=${JSON.stringify(summary.loadAverage)} `
    + `memorystatus=${summary.memoryStatusLevel}`,
    ...summary.processes.map((entry) =>
      `${kStallMarker} process kind=${entry.kind} pid=${entry.pid} ppid=${entry.ppid} `
      + `stat=${entry.stat} elapsed=${entry.elapsed} cpu=${entry.cpuTime} rssKiB=${entry.rssKiB}`
    ),
    `${kStallMarker} callGraphs=${sampled}/${summary.samples.length} window=${summary.systemLogWindow}`,
  ];
}

/** How close to its own timeout a failed test's duration must be to count as a deadline. */
export const kDeadlineToleranceMs = 500;

/**
 * Whether a test ended at its own deadline. A pending Playwright call cut off by the test
 * timeout leaves the status `timedOut`. A pending `expect.poll`, `toPass` or web-first
 * assertion instead gives up 250 ms before that deadline and fails with an ordinary
 * assertion error whose call log reads "Test timeout of <timeout>ms exceeded", leaving the
 * status `failed`; that is the common shape of a stalled browser in these suites.
 *
 * A failed test counts when it carries that message for its own timeout, or when its
 * duration is within `kDeadlineToleranceMs` of its own timeout. The duration test does not
 * depend on Playwright's wording, which a Playwright upgrade could change: Playwright sets
 * `testInfo.duration` to the elapsed test time before it runs the `afterEach` hooks.
 * @param {{status?: string, timeout: number, duration?: number,
 *   errors?: Array<{message?: string}>}} testInfo
 * @returns {boolean}
 */
export function endedAtTestDeadline({ status, timeout, duration, errors = [] }) {
  if (status === "timedOut") {
    return true;
  }
  if (status !== "failed") {
    return false;
  }
  const deadlineMessage = `Test timeout of ${timeout}ms exceeded`;
  const reachedDeadline = timeout > 0 && Number.isFinite(duration)
    && duration >= timeout - kDeadlineToleranceMs;
  return reachedDeadline
    || errors.some((error) => String(error?.message ?? "").includes(deadlineMessage));
}

/**
 * Whether a finished test should be recorded: only one that ended at its deadline, only on
 * macOS, and only when the hosted CI job enabled the recorder.
 * @param {{platform: string, env: Record<string, string | undefined>,
 *   testInfo: {status?: string, timeout: number, duration?: number,
 *     errors?: Array<{message?: string}>}}} options
 * @returns {boolean}
 */
export function shouldRecordBrowserStall({ platform, env, testInfo }) {
  return env[kEnableVariable] === "1" && platform === "darwin" && endedAtTestDeadline(testInfo);
}

/**
 * The fixed text printed when collection fails. Error messages from the file system name
 * output paths, so only a symbolic code is kept.
 * @param {unknown} error
 * @returns {string}
 */
export function describeCollectionFailure(error) {
  const code = /** @type {{code?: unknown}} */ (error)?.code;
  const fixed = typeof code === "string" ? code.replaceAll(/[^A-Za-z0-9_-]/g, "") : "";
  return `${kStallMarker} failed: ${fixed || "error"}`;
}

/**
 * Run `collect` but give up after `budgetMs`, even if a child process never lets it finish.
 * The signal passed to `collect` aborts when the budget expires, so a collection left running
 * stops writing.
 * @template T
 * @param {(signal: AbortSignal) => Promise<T>} collect
 * @param {number} budgetMs
 * @returns {Promise<T>}
 */
export async function withCollectionBudget(collect, budgetMs = kCollectionBudgetMs) {
  const controller = new AbortController();
  let timer;
  const expired = new Promise((_, reject) => {
    timer = setTimeout(() => {
      controller.abort();
      reject(abandonedError());
    }, budgetMs);
  });
  try {
    return await Promise.race([collect(controller.signal), expired]);
  } finally {
    clearTimeout(timer);
  }
}

/**
 * The `afterEach` body: record the browser processes of a test that ended at its deadline
 * when enabled, print the summary lines, and never throw.
 * @param {{title: string, status?: string, timeout: number, duration: number,
 *   errors?: Array<{message?: string}>, outputPath: (name: string) => string}} testInfo
 * @param {{platform?: string, env?: Record<string, string | undefined>, rootPid?: number,
 *   log?: (line: string) => void, collect?: typeof collectBrowserStallDiagnostics,
 *   budgetMs?: number}} options
 * @returns {Promise<string[]>} the printed lines
 */
export async function recordBrowserStall(testInfo, {
  platform = process.platform,
  env = process.env,
  rootPid = process.pid,
  log = (line) => console.log(line),
  collect = collectBrowserStallDiagnostics,
  budgetMs = kCollectionBudgetMs,
} = {}) {
  if (!shouldRecordBrowserStall({ platform, env, testInfo })) {
    return [];
  }
  const test = {
    title: testInfo.title,
    status: testInfo.status ?? "unknown",
    durationMs: testInfo.duration,
  };
  const lines = [formatStallMarker(test)];
  log(lines[0]);
  let rest;
  try {
    const summary = await withCollectionBudget(
      (signal) =>
        collect({
          rootPid,
          outputDirectory: testInfo.outputPath("browser-stall"),
          scratchDirectory: path.join(env.TEST_TMPDIR ?? tmpdir(), "browser-stall-raw"),
          test,
          signal,
          budgetMs,
        }),
      budgetMs,
    );
    rest = formatStallSummary(summary);
  } catch (error) {
    rest = [describeCollectionFailure(error)];
  }
  for (const line of rest) {
    log(line);
    lines.push(line);
  }
  return lines;
}
