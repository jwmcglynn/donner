import type { Page } from "@playwright/test";

export async function boundBrowserDiagnostic<T>(
  work: Promise<T>,
  timeoutMs: number,
): Promise<T | null> {
  let timer: ReturnType<typeof setTimeout> | undefined;
  try {
    return await Promise.race([
      work.catch(() => null),
      new Promise<null>((resolve) => {
        timer = setTimeout(() => resolve(null), timeoutMs);
      }),
    ]);
  } finally {
    if (timer !== undefined) clearTimeout(timer);
  }
}

export async function readGpuOwnerDispatchStats(page: Page) {
  return Promise.any(
    page.workers().map(async (worker) => {
      const result = await boundBrowserDiagnostic(
        worker.evaluate(() => {
          const scope = globalThis as typeof globalThis & {
            __donnerReadGpuOwnerWaitStats?: () => unknown;
            __donnerReadGpuObjectStats?: () => unknown;
          };
          return scope.__donnerReadGpuOwnerWaitStats
            ? {
              sampledAtMs: performance.now(),
              stats: scope.__donnerReadGpuOwnerWaitStats(),
              gpu: scope.__donnerReadGpuObjectStats?.() ?? null,
            }
            : null;
        }),
        1000,
      );
      if (result === null) throw new Error("GPU owner probe unavailable");
      return result;
    }),
  ).catch(() => null);
}

export function wgpuDiagnosticOutcome(input?: Window | void) {
  const scope = input ?? window;
  return {
    requested: scope.__donnerWgpuReadbackRequested ?? 0,
    completed: scope.__donnerWgpuReadbackCompleted ?? 0,
    successfulRequest: scope.__donnerWgpuReadbackStats?.request ?? 0,
    lastStartedRequest: scope.__donnerWgpuReadbackLastStartedRequest ?? 0,
    lastFailedRequest: scope.__donnerWgpuReadbackLastFailedRequest ?? 0,
    starts: scope.__donnerWgpuReadbackCaptureStarts ?? 0,
    captures: scope.__donnerWgpuReadbackCaptureCompletions ?? 0,
    failures: scope.__donnerWgpuReadbackCaptureFailures ?? 0,
    hostFrame: scope.__donnerHostFrameTiming?.frames ?? 0,
    hostPresented: scope.__donnerHostFrameTiming?.lastSurfacePresented ?? false,
    queue: scope.__donnerPresentationQueueStats ?? null,
  };
}

export function hasSuccessfulDiagnosticRequest(
  state: ReturnType<typeof wgpuDiagnosticOutcome>,
  request: number,
): boolean {
  return request > 0 && state.completed >= request && state.successfulRequest >= request
    && state.lastFailedRequest < request;
}
