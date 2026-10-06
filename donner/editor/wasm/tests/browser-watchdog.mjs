import { spawn } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { processRows } from "./browser-process-snapshot.cjs";
export { processRows };

// A loaded host can make one process listing slow or fail, so the final cleanup checks try this
// many listings before they treat the group as unverifiable.
const kCleanupListingAttempts = 3;

export function descendants(rows, rootPid) {
  const selected = new Set([rootPid]);
  let changed = true;
  while (changed) {
    changed = false;
    for (const row of rows) {
      if (selected.has(row.ppid) && !selected.has(row.pid)) {
        selected.add(row.pid);
        changed = true;
      }
    }
  }
  return rows.filter((row) => selected.has(row.pid));
}

export function ownedGroup(rows, groupId) {
  return rows.filter((row) => row.pgid === groupId);
}

export async function supervise(executable, args, options = {}) {
  const outputDir = options.outputDir ?? process.env.TEST_UNDECLARED_OUTPUTS_DIR;
  if (!outputDir) throw new Error("watchdog requires a scoped artifact directory");
  fs.mkdirSync(outputDir, { recursive: true });
  const configuredHeartbeat = options.heartbeatFile ?? process.env.DONNER_WATCHDOG_HEARTBEAT;
  const heartbeatFile = configuredHeartbeat
    ? path.resolve(process.env.TEST_TMPDIR ?? outputDir, configuredHeartbeat)
    : null;
  const reportFile = path.join(outputDir, "browser-watchdog.json");
  const memoryLimit = options.memoryLimit ?? 4 * 1024 * 1024 * 1024;
  const deadlineMs = options.deadlineMs ?? 900000;
  const heartbeatLimitMs = options.heartbeatLimitMs ?? 5000;
  const teardownLimitMs = options.teardownLimitMs ?? 30000;
  const sampleProcesses = options.sampleProcesses ?? processRows;
  const intervalMs = options.intervalMs ?? 1000;
  const anchor = spawn(process.execPath, [
    fileURLToPath(new URL("./browser-watchdog-anchor.mjs", import.meta.url)),
  ], {
    detached: true,
    stdio: ["ignore", "inherit", "inherit", "ipc"],
    env: { ...process.env, ...(heartbeatFile ? { DONNER_WATCHDOG_HEARTBEAT: heartbeatFile } : {}) },
  });
  const startedAt = Date.now();
  const owned = new Map();
  const samples = [];
  const measurementFailures = [];
  let lastMeasuredAtMs = 0;
  let rootIdentity;
  let reason = null;
  let reportingError = null;
  let peakRssBytes = 0;
  let monitor;
  let stopping = false;
  let terminationIssued = false;
  let cleanup;
  let heartbeatSessionId;
  let teardownDeadline;
  let applicationHeartbeatObserved = false;
  const receipt = () => ({
    reason,
    reportingError,
    rootPid: anchor.pid,
    rootIdentity,
    peakRssBytes,
    memoryLimit,
    samples,
    measurementFailures,
    cleanup,
  });
  const writeReceipt = options.writeReceipt ?? (() => {
    fs.writeFileSync(`${reportFile}.pending`, JSON.stringify(receipt(), null, 2));
    fs.renameSync(`${reportFile}.pending`, reportFile);
  });
  const reportBestEffort = () => {
    try {
      writeReceipt();
    } catch (error) {
      reportingError = error.message;
    }
  };
  // The anchor stays alive until this group is terminated, so its group id cannot be recycled.
  const terminateGroup = () => {
    if (terminationIssued) return;
    terminationIssued = true;
    try {
      process.kill(-anchor.pid, "SIGKILL");
    } catch (error) {
      if (error.code !== "ESRCH") throw error;
    }
  };
  const stop = (failure) => {
    if (!reason) reason = failure;
    if (stopping) return;
    stopping = true;
    clearInterval(monitor);
    try {
      terminateGroup();
    } finally {
      reportBestEffort();
    }
  };
  const onSigterm = () => stop("supervisor received SIGTERM");
  const onSigint = () => stop("supervisor received SIGINT");
  process.once("SIGTERM", onSigterm);
  process.once("SIGINT", onSigint);
  const anchorExited = new Promise((resolve) => anchor.once("exit", resolve));
  const driverExited = new Promise((resolve, reject) => {
    anchor.once("error", reject);
    anchor.on("message", (message) => {
      if (message.kind === "anchor-ready" && !stopping) {
        anchor.send({
          executable,
          args,
          preload: fileURLToPath(new URL("./browser-watchdog-child.cjs", import.meta.url)),
        }, (error) => {
          if (error && !stopping) stop(`watchdog launch failed: ${error.message}`);
        });
      }
      if (message.kind === "driver-exit") {
        resolve({ code: message.code, signal: message.signal, error: message.error });
      }
    });
    anchor.once("exit", (code, signal) => resolve({ code, signal }));
  });
  const sample = () => {
    try {
      let rows;
      try {
        rows = sampleProcesses();
      } catch (error) {
        // A loaded host can make one process listing slow or fail. Keep the run going until the
        // tree has gone unmeasured for longer than the heartbeat limit.
        const atMs = Date.now() - startedAt;
        measurementFailures.push({ atMs, error: error.message.slice(0, 200) });
        if (measurementFailures.length > 16) measurementFailures.shift();
        if (atMs - lastMeasuredAtMs > heartbeatLimitMs) throw error;
        return;
      }
      lastMeasuredAtMs = Date.now() - startedAt;
      const root = rows.find((row) => row.pid === anchor.pid);
      if (rootIdentity && root?.start !== rootIdentity) {
        throw new Error("watchdog anchor identity changed");
      }
      if (root) rootIdentity ??= root.start;
      const current = ownedGroup(rows, anchor.pid);
      for (const row of current) owned.set(row.pid, row.start);
      const rssBytes = rows.filter((row) => owned.get(row.pid) === row.start)
        .reduce((total, row) => total + row.rssBytes, 0);
      peakRssBytes = Math.max(peakRssBytes, rssBytes);
      const atMs = Date.now() - startedAt;
      const row = {
        atMs,
        rssBytes,
        processes: current.length,
        processRss: current.slice(0, 64).map(({ pid, ppid, pgid, rssBytes, command }) => ({
          pid,
          ppid,
          pgid,
          rssBytes,
          command,
        })),
      };
      if (heartbeatFile && fs.existsSync(heartbeatFile)) {
        row.heartbeatAgeMs = Date.now() - fs.statSync(heartbeatFile).mtimeMs;
        const heartbeat = fs.readFileSync(heartbeatFile, "utf8");
        if (heartbeat.length < 16384) {
          try {
            row.application = JSON.parse(heartbeat);
          } catch {}
        }
      }
      if (row.application?.phase === "active") {
        const responseAtMs = row.application.atMs;
        const nowMs = Date.now();
        if (!Number.isSafeInteger(responseAtMs) || responseAtMs <= 0 || responseAtMs > nowMs) {
          throw new Error("invalid application heartbeat timestamp");
        }
        row.heartbeatAgeMs = Math.max(row.heartbeatAgeMs, nowMs - responseAtMs);
        applicationHeartbeatObserved = true;
      } else if (row.application?.phase === "launch" || row.application?.phase === "teardown") {
        applicationHeartbeatObserved = false;
      } else if (applicationHeartbeatObserved) {
        throw new Error("invalid application heartbeat evidence");
      }
      if (row.application?.phase === "active" && row.application.sessionId !== heartbeatSessionId) {
        heartbeatSessionId = row.application.sessionId;
        teardownDeadline = undefined;
      }
      if (row.application?.phase === "teardown") {
        heartbeatSessionId ??= row.application.sessionId;
        teardownDeadline ??= Date.now() + teardownLimitMs;
      }
      samples.push(row);
      if (samples.length > 360) samples.shift();
      if (rssBytes > memoryLimit) stop("memory limit exceeded");
      else if (atMs > deadlineMs) stop("test deadline exceeded");
      else if (teardownDeadline && Date.now() > teardownDeadline) {
        stop("browser teardown deadline exceeded");
      } else if (
        heartbeatFile && (row.application?.phase === "active" || atMs > 5000)
        && (row.heartbeatAgeMs ?? atMs) > heartbeatLimitMs
      ) {
        stop("test heartbeat stopped");
      } else {
        try {
          writeReceipt();
        } catch (error) {
          stop(`watchdog reporting failed: ${error.message}`);
        }
      }
    } catch (error) {
      stop(`watchdog measurement failed: ${error.message}`);
    }
  };
  monitor = setInterval(sample, intervalMs);
  let exit;
  try {
    exit = await driverExited;
  } finally {
    clearInterval(monitor);
    terminateGroup();
    await anchorExited;
    // Group cleanup is independent of process enumeration and artifact writing.
    const listProcesses = options.cleanupProcesses ?? processRows;
    const pause = () => new Promise((resolve) => setTimeout(resolve, 10));
    let groupGone = false;
    let failedGroupListings = 0;
    for (let attempt = 0; attempt < 100; ++attempt) {
      try {
        process.kill(-anchor.pid, 0);
      } catch (error) {
        if (error.code === "ESRCH") {
          groupGone = true;
          break;
        }
        if (error.code !== "EPERM") throw error;
        // A denied probe needs a process listing to show that the group is gone.
        try {
          if (ownedGroup(listProcesses(), anchor.pid).length === 0) {
            groupGone = true;
            break;
          }
        } catch (listingError) {
          if (++failedGroupListings === kCleanupListingAttempts) {
            reportingError ??= `group enumeration failed: ${listingError.message}`;
            reason ??= "test process cleanup verification unavailable";
            break;
          }
        }
      }
      await pause();
    }
    let survivors = null;
    for (let attempt = 1; survivors === null; ++attempt) {
      try {
        survivors = listProcesses().filter((row) => owned.get(row.pid) === row.start)
          .map((row) => row.pid);
      } catch (error) {
        if (attempt === kCleanupListingAttempts) {
          reportingError ??= `survivor enumeration failed: ${error.message}`;
          reason ??= "test process cleanup verification unavailable";
          break;
        }
        await pause();
      }
    }
    cleanup = { groupGone, survivors };
    if (!groupGone || survivors === null || survivors.length) {
      reason ??= "test process cleanup incomplete";
    }
    process.removeListener("SIGTERM", onSigterm);
    process.removeListener("SIGINT", onSigint);
    reportBestEffort();
    if (reportingError) reason ??= "watchdog final reporting failed";
  }
  return { ...receipt(), exit, reportFile };
}

if (process.argv[1] && import.meta.url === pathToFileURL(fs.realpathSync(process.argv[1])).href) {
  const executable = process.env.DONNER_WATCHDOG_DRIVER;
  if (!executable) throw new Error("DONNER_WATCHDOG_DRIVER is required");
  const spec = process.env.DONNER_WATCHDOG_SPEC;
  const config = process.env.DONNER_WATCHDOG_CONFIG;
  if (!spec || !config) throw new Error("watchdog requires the declared browser test and config");
  const args = [
    "test",
    path.resolve(spec),
    `--config=${path.resolve(config)}`,
    ...process.argv.slice(2),
  ];
  if (process.platform === "darwin") {
    let guiNamespaceAvailable = false;
    try {
      execFileSync("/bin/launchctl", ["print", `gui/${process.getuid()}`], {
        stdio: "ignore",
        timeout: 1000,
      });
      guiNamespaceAvailable = true;
    } catch {}
    console.log(
      `browser host capability ${JSON.stringify({ uid: process.getuid(), guiNamespaceAvailable })}`,
    );
  }
  const result = await supervise(path.resolve(executable), args);
  process.exitCode = result.reason ? 1 : (result.exit.code ?? 1);
}
