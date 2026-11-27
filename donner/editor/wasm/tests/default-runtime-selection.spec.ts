import { expect, test } from "@playwright/test";

const baseUrl = process.env.DONNER_WASM_BASE_URL || "http://127.0.0.1:8000";
const runtimePath = /\/(editor(?:-asyncify)?\.(?:js|wasm))$/;

test.describe("default editor runtime selection", () => {
  for (const fallback of [false, true]) {
    const behavior = fallback ? "resumes with Asyncify when its probe worker lacks JSPI"
      : "renders with the browser's native JSPI";
    test(behavior, async ({ page, context }, info) => {
      if (fallback) {
        // Only the capability probe changes. The editor's own workers use the real browser.
        await page.addInitScript(() => {
          const BrowserWorker = window.Worker;
          window.Worker = class extends BrowserWorker {
            constructor(url: string | URL, options?: WorkerOptions) {
              const src = new URL(url, location.href);
              if (!src.pathname.endsWith("/editor-jspi.js")) {
                super(url, options);
                return;
              }
              const body = 'Object.defineProperty(WebAssembly, "promising", {value: undefined});\n'
                + "importScripts(" + JSON.stringify(src.href) + ");";
              const blob = URL.createObjectURL(new Blob([body], { type: "text/javascript" }));
              super(blob, options);
              this.addEventListener("message", () => URL.revokeObjectURL(blob), { once: true });
              this.addEventListener("error", () => URL.revokeObjectURL(blob), { once: true });
              document.documentElement.dataset.probeWorkerWithoutJspi = "true";
            }
          };
        });
      }

      const requested = new Set<string>();
      context.on("request", (request) => {
        const name = runtimePath.exec(new URL(request.url()).pathname)?.[1];
        if (name) requested.add(name);
      });
      const url = new URL("index.html", baseUrl.replace(/\/?$/, "/"));
      url.searchParams.set("wgpuReadbackStats", "1");
      await page.goto(url.href, { waitUntil: "domcontentloaded" });
      await expect(page.locator("script[data-asyncify-fallback]")).toHaveCount(1);
      const pixels = () => page.evaluate(() => {
        const app = window as Window & {
          __donnerFirstFramePresented?: boolean;
          __donnerWgpuReadbackStats?: { renderPane?: { coloredPixels: number } };
        };
        return app.__donnerFirstFramePresented ? app.__donnerWgpuReadbackStats?.renderPane?.coloredPixels ?? 0 : 0;
      });
      await expect.poll(pixels, { timeout: 60000, message: "the selected runtime presents artwork" })
        .toBeGreaterThan(0);
      if (fallback) await expect(page.locator("html")).toHaveAttribute("data-probe-worker-without-jspi", "true");
      const runtime = fallback ? "editor-asyncify" : "editor";
      const expected = [runtime + ".js", runtime + ".wasm"];
      const transfers = await page.evaluate(() => performance.getEntriesByType("resource")
        .map((entry) => entry as PerformanceResourceTiming)
        .filter((entry) => /\/editor(?:-asyncify)?\.(?:js|wasm)$/.test(new URL(entry.name).pathname))
        .map((entry) => ({ file: new URL(entry.name).pathname.split("/").pop()!,
          encodedBytes: entry.encodedBodySize, transferBytes: entry.transferSize })));
      await info.attach("selected-runtime", { contentType: "application/json",
        body: JSON.stringify({ selected: runtime, requests: [...requested], transfers, coloredPixels: await pixels() }) });
      expect([...requested].sort()).toEqual(expected);
      expect([...new Set(transfers.map((t) => t.file))].sort()).toEqual(expected);
    });
  }
});
