import { chromium } from "@playwright/test";
import assert from "node:assert/strict";
import test from "node:test";

for (
  const [name, channel] of [
    ["Chrome for Testing", "chromium"],
    ["Chrome headless shell", undefined],
  ]
) {
  test(`${name} launches from the Bazel browser archive`, async () => {
    const browser = await chromium.launch({
      channel,
      headless: true,
      args: ["--no-sandbox"],
    });
    try {
      const page = await browser.newPage();
      await page.goto("data:text/html,<title>archive launch</title>");
      assert.equal(await page.title(), "archive launch");
    } finally {
      await browser.close();
    }
  });
}
