import { expect, test } from "@playwright/test";

declare global {
  interface Window {
    __donnerCanStartWasm?: boolean;
  }
}

const kRunFallbackTest = process.env.DONNER_WASM_TEST_ISOLATION_FALLBACK === "1";
const kFatalRuntimePattern =
  /Aborted|Assertion failed|RuntimeError|Pthread .* sent an error|WebAssembly runtime unavailable|Unhandled promise rejection/i;

test("secure static host isolation fallback enables Wasm threads", async ({ page }) => {
  test.skip(!kRunFallbackTest, "requires a server without COOP/COEP response headers");
  // A cold browser may need both a service-worker activation reload and a
  // separate pthread Wasm startup, whose existing phase deadlines exceed the
  // suite's 30-second default when combined.
  test.setTimeout(60_000);

  const baseUrl = process.env.DONNER_WASM_BASE_URL || "http://127.0.0.1:8000";
  const fatalMessages: string[] = [];
  page.on("console", (message) => {
    if (kFatalRuntimePattern.test(message.text())) {
      fatalMessages.push(`[console:${message.type()}] ${message.text()}`);
    }
  });
  page.on("pageerror", (error) => fatalMessages.push(`[pageerror] ${error.message}`));

  await page.goto(baseUrl, { waitUntil: "domcontentloaded" });

  await expect
    .poll(async () => {
      try {
        return await page.evaluate(() => ({
          canStartWasm: window.__donnerCanStartWasm,
          controllerUrl: navigator.serviceWorker.controller?.scriptURL || null,
          crossOriginIsolated,
          hasSharedArrayBuffer: typeof SharedArrayBuffer !== "undefined",
          isSecureContext,
        }));
      } catch {
        return null;
      }
    }, {
      message: "expected the service worker reload to establish cross-origin isolation",
      timeout: 20000,
    })
    .toEqual({
      canStartWasm: true,
      controllerUrl: new URL("enable-threads.js", baseUrl).href,
      crossOriginIsolated: true,
      hasSharedArrayBuffer: true,
      isSecureContext: true,
    });

  await expect(page.locator("#capability-error")).toBeHidden();
  await expect(page.locator("canvas#canvas")).toBeVisible();
  await expect(page.locator("#status")).toBeHidden({ timeout: 30000 });
  await expect
    .poll(async () => page.evaluate(() => window.__donnerBackend))
    .toMatch(/^(geode|tiny_skia)$/);
  await page.waitForTimeout(1000);
  expect(fatalMessages).toEqual([]);
});
