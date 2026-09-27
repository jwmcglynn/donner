import { expect, test } from "@playwright/test";
import { execFileSync } from "node:child_process";
import { writeFileSync } from "node:fs";
import { join, resolve } from "node:path";

const baseUrl = process.env.DONNER_WASM_BASE_URL || "http://127.0.0.1:8000";
const selectedBrowserBackend = "[Geode] GPU backend: browser, selected by the build setting";

const browserDiagnosticStages = new Map([
  ["[wasm] render_svg entry", "render_entry"],
  ["[wasm] parsed OK", "svg_parsed"],
  ["[wasm] RendererGeode constructed", "renderer_constructed"],
  ["[wasm] draw() returned", "draw_returned"],
  ["[wasm] takeSnapshot() returned", "snapshot_returned"],
  ["[Geode] snapshot map reached the capture deadline", "snapshot_map_deadline"],
]);

function browserDiagnosticStage(line: string): string | null {
  for (const [prefix, stage] of browserDiagnosticStages) {
    if (line.startsWith(prefix)) {
      return stage;
    }
  }
  return null;
}

test("browser diagnostics cannot disclose console content", () => {
  const sensitive = "https://private.example.test/path /Users/operator/private/file";
  expect(browserDiagnosticStage(sensitive)).toBeNull();
  expect(browserDiagnosticStage(`[wasm] parsed OK ${sensitive}`)).toBe("svg_parsed");
});

test("standalone Geode wasm selects the browser backend and paints SVG pixels", async ({ page }) => {
  test.setTimeout(90000);
  const consoleLines: string[] = [];
  let pageErrorCount = 0;
  page.on("console", (message) => consoleLines.push(message.text()));
  page.on("pageerror", () => {
    pageErrorCount += 1;
  });

  await page.addInitScript(() => {
    const originalRequestDevice = GPUAdapter.prototype.requestDevice;
    let deviceRequests = 0;
    GPUAdapter.prototype.requestDevice = function(...args) {
      deviceRequests += 1;
      return originalRequestDevice.apply(this, args);
    };
    Object.defineProperty(window, "__donnerDeviceRequests", {
      get: () => deviceRequests,
    });
  });

  await page.goto(`${baseUrl}/test-geode.html`, { waitUntil: "domcontentloaded" });
  const status = page.locator("#status");
  const render = page.locator("#render-btn");
  await expect(render).toBeEnabled({ timeout: 45000 });
  await render.click();
  try {
    await expect(status).toContainText("Rendered 400x400 via Geode", { timeout: 45000 });
  } catch (error) {
    // Emit fixed stage names and counts; browser messages may carry paths or URLs.
    const stages = [
      ...new Set(consoleLines.map(browserDiagnosticStage).filter((stage) => stage !== null)),
    ];
    console.error(
      `browser stages: ${stages.join(",") || "none"}; page errors: ${pageErrorCount}`,
    );
    throw error;
  }
  expect(
    consoleLines.some((line) => line.includes(selectedBrowserBackend)),
    "the rendered page never selected the browser backend",
  ).toBe(true);
  expect(
    await page.evaluate(() =>
      (window as Window & { __donnerDeviceRequests: number }).__donnerDeviceRequests
    ),
    "the standalone browser module must create only one WebGPU device",
  ).toBe(1);

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
      // The golden helper uses this path in diff filenames; keep the runfiles path short.
      DONNER_GOLDEN_PNG: golden!,
    },
    stdio: "inherit",
    timeout: 10000,
  });

  expect(pageErrorCount).toBe(0);
});
