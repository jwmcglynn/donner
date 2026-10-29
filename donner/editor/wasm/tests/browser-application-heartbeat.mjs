import crypto from "node:crypto";
import fs from "node:fs";
import {
  APPLICATION_HEARTBEAT_LIMIT_MS,
  applicationActiveMarker,
  writeHeartbeat,
} from "./browser-heartbeat.cjs";

// Ownership discovery is bounded by the existing application-liveness ceiling. An unanswered
// IPC is unknown, not a negative role response. Keep one request per worker, including requests
// still pending when another worker identifies itself, so diagnostics cannot overlap them.
function discoverApplicationWorker(page, requests) {
  return new Promise((resolve, reject) => {
    const probes = new Map();
    const expiresAt = Date.now() + APPLICATION_HEARTBEAT_LIMIT_MS;
    let finished = false;
    const counts = () => {
      const result = { true: 0, false: 0, failed: 0, unresolved: 0 };
      for (const probe of probes.values()) result[probe.status]++;
      return JSON.stringify(result);
    };
    const cleanup = () => {
      clearTimeout(deadline);
      page.off?.("worker", identify);
      page.off?.("close", closed);
      for (const [worker, probe] of probes) worker.off?.("close", probe.closed);
    };
    const fail = (reason) => {
      if (finished) return;
      finished = true;
      cleanup();
      reject(new Error(`${reason}: ${counts()}`));
    };
    const closed = () => fail("page closed during application worker discovery");
    const identify = (worker) => {
      if (finished || probes.has(worker)) return;
      if (Date.now() >= expiresAt) {
        fail("application worker discovery deadline");
        return;
      }
      const probe = { status: "unresolved", closed: undefined };
      probe.closed = () => { probe.status = "failed"; };
      probes.set(worker, probe);
      worker.once?.("close", probe.closed);
      requests.add(worker);
      Promise.resolve().then(() => worker.evaluate(() =>
        Boolean(globalThis.__donnerApplicationWorker)
      )).then((ownsSurface) => {
        requests.delete(worker);
        if (finished || probe.status !== "unresolved") return;
        probe.status = ownsSurface === true ? "true" : "false";
        if (Date.now() >= expiresAt) {
          fail("application worker discovery deadline");
          return;
        }
        if (ownsSurface === true) {
          finished = true;
          cleanup();
          resolve(worker);
        }
      }, () => {
        requests.delete(worker);
        if (!finished && probe.status === "unresolved") probe.status = "failed";
      });
    };
    const deadline = setTimeout(
      () => fail("application worker discovery deadline"),
      APPLICATION_HEARTBEAT_LIMIT_MS,
    );
    page.on?.("worker", identify);
    page.once?.("close", closed);
    if (page.isClosed?.()) closed();
    else for (const worker of page.workers()) identify(worker);
  });
}

export async function startApplicationHeartbeat(
  page,
  heartbeat = process.env.DONNER_WATCHDOG_HEARTBEAT,
) {
  const sessionId = crypto.randomUUID();
  const marker = applicationActiveMarker(heartbeat);
  const workerRequests = new Set();
  const application = await discoverApplicationWorker(page, workerRequests);
  fs.writeFileSync(marker, "active");
  const workerSnapshots = new Map();
  let busy = false;
  let stopped = false;
  let pageRequestPending = false;
  const presentationSnapshot = async () => {
    if (typeof page.evaluate !== "function" || pageRequestPending) return { unavailable: true };
    pageRequestPending = true;
    let deadline;
    try {
      return await Promise.race([
        page.evaluate(() => ({
          interaction: window.__donnerInteractionStats ?? null,
          queue: window.__donnerPresentationQueueStats ?? null,
          repair: window.__donnerPresentationRepairStats ?? null,
          worker: window.__donnerWorkerStats ?? null,
          host: window.__donnerHostFrameTiming ?? null,
          viewport: window.__donnerViewportStats ?? null,
          thumbnails: window.__donnerSampleThumbnailStats ?? null,
        })).finally(() => {
          pageRequestPending = false;
        }),
        new Promise((_, reject) => {
          deadline = setTimeout(() => reject(new Error("page diagnostic deadline")), 500);
        }),
      ]);
    } catch {
      return { unavailable: true };
    } finally {
      clearTimeout(deadline);
    }
  };
  const pulse = async () => {
    if (busy || stopped || workerRequests.has(application)) return;
    busy = true;
    workerRequests.add(application);
    let timeout;
    try {
      const gpu = await Promise.race([
        application.evaluate(() => {
          const state = globalThis;
          return state.__donnerReadGpuObjectStats?.() ?? null;
        }).finally(() => {
          // A wait deadline does not cancel the underlying worker IPC.
          workerRequests.delete(application);
        }),
        new Promise((_, reject) => {
          timeout = setTimeout(() => reject(new Error("application heartbeat deadline")), 500);
        }),
      ]);
      if (stopped) return;
      const applicationAtMs = Date.now();
      const workers = page.workers();
      for (const worker of workerSnapshots.keys()) {
        if (!workers.includes(worker)) workerSnapshots.delete(worker);
      }
      await Promise.all(workers.map(async (worker, index) => {
        if (workerRequests.has(worker)) return;
        workerRequests.add(worker);
        let deadline;
        try {
          const objects = await Promise.race([
            worker.evaluate(() => {
              const state = globalThis;
              return state.__donnerReadGpuObjectStats?.() ?? null;
            }).then((objects) => {
              workerRequests.delete(worker);
              if (!stopped) workerSnapshots.set(worker, { index, atMs: Date.now(), gpu: objects });
              return objects;
            }, (error) => {
              workerRequests.delete(worker);
              if (!stopped) {
                workerSnapshots.set(worker, {
                  index,
                  atMs: Date.now(),
                  gpu: null,
                  unavailable: true,
                });
              }
              throw error;
            }),
            new Promise((_, reject) => {
              deadline = setTimeout(() => reject(new Error("worker diagnostic timeout")), 500);
            }),
          ]);
          if (!stopped) workerSnapshots.set(worker, { index, atMs: Date.now(), gpu: objects });
        } catch {
          // An idle renderer may be parked in Atomics.wait; retain its last explicit sample.
        } finally {
          clearTimeout(deadline);
        }
      }));
      if (stopped) return;
      const presentation = await presentationSnapshot();
      if (stopped) return;
      writeHeartbeat(heartbeat, {
        atMs: applicationAtMs,
        phase: "active",
        sessionId,
        gpu,
        presentation,
        workers: workers.map((worker, index) => {
          const prior = workerSnapshots.get(worker);
          return prior
            ? { ...prior, ageMs: Date.now() - prior.atMs }
            : { index, gpu: null, unavailable: true };
        }),
      });
    } catch {
      /* The external watchdog terminates a stopped application event loop. */
    } finally {
      clearTimeout(timeout);
      busy = false;
    }
  };
  await pulse();
  const timer = setInterval(pulse, 1000);
  return () => {
    stopped = true;
    clearInterval(timer);
    fs.rmSync(marker, { force: true });
    writeHeartbeat(heartbeat, {
      atMs: Date.now(),
      phase: "teardown",
      sessionId,
      phaseStartedAtMs: Date.now(),
    });
  };
}

/**
 * End a finished application session's teardown record. Call it as a case starts: the runner
 * starts a case only after the previous case's page and context closed, so a teardown still
 * recorded then is complete. The new launch session scopes the watchdog's teardown deadline to
 * that close; without it, the deadline also ran through every following case that starts no
 * application heartbeat, and a run of such cases longer than the deadline killed the run.
 * @param {string | undefined} heartbeat Heartbeat file.
 */
export function endApplicationTeardown(heartbeat = process.env.DONNER_WATCHDOG_HEARTBEAT) {
  if (!heartbeat) return;
  let prior;
  try {
    prior = JSON.parse(fs.readFileSync(heartbeat, "utf8"));
  } catch {
    return;
  }
  if (prior?.phase !== "teardown") return;
  writeHeartbeat(heartbeat, { atMs: Date.now(), phase: "launch", sessionId: crypto.randomUUID() });
}
