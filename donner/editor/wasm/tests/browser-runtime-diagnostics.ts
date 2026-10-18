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
