import { expect, test } from "@playwright/test";

const kFatalRuntimePattern =
  /Aborted|Assertion failed|RuntimeError|Pthread .* sent an error|getJsObject/i;

test("wasm editor starts without runtime abort", async ({ page }) => {
  const baseUrl = process.env.DONNER_WASM_BASE_URL || "http://127.0.0.1:8000";
  const fatalMessages: string[] = [];

  page.on("console", (message) => {
    const text = message.text();
    if (kFatalRuntimePattern.test(text)) {
      fatalMessages.push(`[console:${message.type()}] ${text}`);
    }
  });
  page.on("pageerror", (error) => {
    fatalMessages.push(`[pageerror] ${error.message}`);
  });

  await page.goto(baseUrl, { waitUntil: "domcontentloaded" });
  const hasWebGpu = await page.evaluate(() => "gpu" in navigator);
  test.skip(!hasWebGpu, "Browser does not expose navigator.gpu");

  await expect(page.locator("canvas#canvas")).toBeVisible();
  await expect(page.locator("#status")).toBeHidden({ timeout: 20000 });
  await page.waitForTimeout(2000);

  expect(fatalMessages).toEqual([]);
});
