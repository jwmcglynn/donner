import { expect, test } from "@playwright/test";
import { execFileSync } from "node:child_process";
import { writeFileSync } from "node:fs";
import { join, resolve } from "node:path";

const baseUrl = process.env.DONNER_WASM_BASE_URL || "http://127.0.0.1:8000";
const selectedBrowserBackend = "[Geode] GPU backend: browser, selected by the build setting";

test("standalone Geode wasm selects the browser backend and paints SVG pixels", async ({ page }) => {
  test.setTimeout(90000);
  const consoleLines: string[] = [];
  const pageErrors: string[] = [];
  page.on("console", (message) => consoleLines.push(message.text()));
  page.on("pageerror", (error) => pageErrors.push(error.message));

  await page.goto(`${baseUrl}/test-geode.html`, { waitUntil: "domcontentloaded" });
  const status = page.locator("#status");
  const render = page.locator("#render-btn");
  await expect(render).toBeEnabled({ timeout: 45000 });
  await render.click();
  await expect(status).toContainText("Rendered 400x400 via Geode", { timeout: 45000 });
  expect(
    consoleLines.some((line) => line.includes(selectedBrowserBackend)),
    "the rendered page never selected the browser backend",
  ).toBe(true);

  const png = await page.locator("#canvas").evaluate((element) =>
    (element as HTMLCanvasElement).toDataURL("image/png")
  );
  expect(png).toMatch(/^data:image\/png;base64,/);
  const outputDirectory = process.env.TEST_UNDECLARED_OUTPUTS_DIR;
  const compare = process.env.DONNER_BROWSER_GOLDEN_COMPARE;
  const golden = process.env.DONNER_BROWSER_GOLDEN_PNG;
  expect(outputDirectory, "Bazel must provide an artifact directory").toBeTruthy();
  expect(compare, "the shared pixelmatch comparator is missing").toBeTruthy();
  expect(golden, "the committed SVG golden is missing").toBeTruthy();
  const actual = join(outputDirectory!, "standalone_geode_browser_actual.png");
  writeFileSync(actual, Buffer.from(png.slice("data:image/png;base64,".length), "base64"));
  execFileSync(resolve(compare!), [], {
    env: {
      ...process.env,
      DONNER_ACTUAL_PNG: actual,
      DONNER_GOLDEN_PNG: resolve(golden!),
    },
    stdio: "inherit",
    timeout: 10000,
  });

  expect(pageErrors).toEqual([]);
});
