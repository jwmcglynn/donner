import crypto from "node:crypto";
import fs from "node:fs";
import { writeHeartbeat } from "./browser-heartbeat.cjs";

export async function startApplicationHeartbeat(
  page,
  heartbeat = process.env.DONNER_WATCHDOG_HEARTBEAT,
) {
  const sessionId = crypto.randomUUID();
  const marker = `${heartbeat}.app-active`;
  const application = await Promise.any(
    page.workers().map(async (worker) => {
      const ownsSurface = await Promise.race([
        worker.evaluate(() =>
          Boolean(
            globalThis.__donnerApplicationGpuSurfaceWorker,
          )
        ),
        new Promise((resolve) => setTimeout(() => resolve(false), 1000)),
      ]);
      if (!ownsSurface) throw new Error("worker does not own the presented canvas");
      return worker;
    }),
  );
  fs.writeFileSync(marker, "active");
  const workerSnapshots = new Map();
  const workerRequests = new Set();
  let busy = false;
  let stopped = false;
  const pulse = async () => {
    if (busy || stopped) return;
    busy = true;
    let timeout;
    try {
      const gpu = await Promise.race([
        application.evaluate(() => {
          const state = globalThis;
          return state.__donnerReadGpuObjectStats?.() ?? null;
        }),
        new Promise((_, reject) => {
          timeout = setTimeout(() => reject(new Error("application heartbeat deadline")), 500);
        }),
      ]);
      if (stopped) return;
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
      writeHeartbeat(heartbeat, {
        atMs: Date.now(),
        phase: "active",
        sessionId,
        gpu,
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
