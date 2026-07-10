import { expect, test } from "@playwright/test";

declare global {
  interface Window {
    __donnerBackend?: string;
    __donnerCanStartWasm?: boolean;
  }
}

const kFatalRuntimePattern =
  /Aborted|Assertion failed|RuntimeError|Pthread .* sent an error|WebAssembly runtime unavailable/i;

test("iPhone-profile WebKit starts the TinySkia fallback", async ({ page }) => {
  const baseUrl = process.env.DONNER_WASM_BASE_URL || "http://127.0.0.1:8000";
  const url = new URL(baseUrl);
  url.searchParams.set("renderer", "tiny_skia");
  const fatalMessages: string[] = [];

  page.on("console", (message) => {
    if (kFatalRuntimePattern.test(message.text())) {
      fatalMessages.push(`[console:${message.type()}] ${message.text()}`);
    }
  });
  page.on("pageerror", (error) => {
    fatalMessages.push(`[pageerror] ${error.message}`);
  });

  await page.goto(url.toString(), { waitUntil: "domcontentloaded" });
  await expect(page.locator("canvas#canvas")).toBeVisible();
  await expect(page.locator("#status")).toBeHidden({ timeout: 30000 });

  const capabilities = await page.evaluate(() => {
    const probe = document.createElement("canvas");
    const webGl2 = probe.getContext("webgl2");
    webGl2?.getExtension("WEBGL_lose_context")?.loseContext();
    const editorCanvas = document.getElementById("canvas");
    return {
      backend: window.__donnerBackend,
      canStartWasm: window.__donnerCanStartWasm,
      crossOriginIsolated,
      hasSharedArrayBuffer: typeof SharedArrayBuffer !== "undefined",
      hasWebGl2: Boolean(webGl2),
      isSecureContext,
      userAgent: navigator.userAgent,
      viewport: {
        width: editorCanvas?.clientWidth || 0,
        height: editorCanvas?.clientHeight || 0,
      },
    };
  });

  expect(capabilities.userAgent).toContain("iPhone");
  expect(capabilities.isSecureContext).toBe(true);
  expect(capabilities.crossOriginIsolated).toBe(true);
  expect(capabilities.hasSharedArrayBuffer).toBe(true);
  expect(capabilities.canStartWasm).toBe(true);
  expect(capabilities.hasWebGl2).toBe(true);
  expect(capabilities.backend).toBe("tiny_skia");
  expect(capabilities.viewport.width).toBeGreaterThan(0);
  expect(capabilities.viewport.height).toBeGreaterThan(0);
  expect(fatalMessages).toEqual([]);
});
