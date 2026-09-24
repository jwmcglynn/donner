import { expect, test } from "@playwright/test";

// The line the renderer prints the first time a selection in the page lands on the browser
// backend because the build asked for it. The raster worker selects when the editor draws its
// first document, during startup, and its stderr reaches the page console.
const kSelectedByBuildSetting = "[Geode] GPU backend: browser, selected by the build setting";

// Every line the browser backend prints when it cannot serve a selection or a device.
const kBrowserBackendFailure = "[Geode/browser]";

// The backend the served package was built to select for headless work. A lane serving the
// package that selects the browser backend says so; every other package must not select it.
const kExpectsBrowserBackend = process.env.DONNER_WASM_EXPECTED_HEADLESS_BACKEND === "browser";

// How the raster worker waited for its readbacks, which only its own results publish. The browser
// backend reports a mapping ready only when it looks again after handing the thread over, which
// the published stats call a device poll; the transitional adapter's WebAssembly path waits on a
// completion event, which they call a timed wait.
const kExpectedRasterWorkerWait = kExpectsBrowserBackend ? "device-poll" : "timed-wait-any";

const baseUrl = process.env.DONNER_WASM_BASE_URL || "http://127.0.0.1:8000";

type WorkerStats = {
  completedResults?: number;
  readbackCount?: number;
  readbackWaitStrategy?: string;
};
type SelectionWindow = Window & {
  __donnerFirstFramePresented?: boolean;
  __donnerWorkerStats?: WorkerStats;
  __donnerSampleThumbnailStats?: {
    completed?: number;
    ready?: number;
    active?: boolean;
    pending?: boolean;
  };
};

// The layout the Basic Shapes card is placed in below.
test.use({ viewport: { width: 1600, height: 900 } });

test("the raster worker selects the backend its package was built for", async ({ page }) => {
  test.setTimeout(60000);
  const lines: string[] = [];
  page.on("console", (message) => lines.push(message.text()));
  const errors: string[] = [];
  page.on("pageerror", (error) => errors.push(error.message));

  await page.goto(`${baseUrl}/index.html`, { waitUntil: "domcontentloaded" });
  await expect
    .poll(() => page.evaluate(() => (window as SelectionWindow).__donnerFirstFramePresented), {
      message: "the editor never presented its first frame",
      timeout: 30000,
    })
    .toBe(true);
  if (kExpectsBrowserBackend) {
    await expect
      .poll(() => lines.some((line) => line.includes(kSelectedByBuildSetting)), {
        message: "no selection in the page landed on the browser backend",
        timeout: 15000,
      })
      .toBe(true);
  } else {
    expect(
      lines.filter((line) => line.includes(kSelectedByBuildSetting)),
      "a package built without the browser backend selected it",
    ).toEqual([]);
  }

  // The line names the backend some selection in the page took, which could be any headless
  // renderer. Opening a document makes the raster worker read back, and the stats only its results
  // publish show which backend that readback went through.
  await expect
    .poll(
      () =>
        page.evaluate(() => {
          const stats = (window as SelectionWindow).__donnerSampleThumbnailStats;
          return !!stats && (stats.completed ?? 0) > 0 && (stats.ready ?? 0) > 0 && !stats.active
            && !stats.pending;
        }),
      { message: "the sample picker's thumbnails never settled", timeout: 20000 },
    )
    .toBe(true);
  const editorCanvas = page.locator("canvas#canvas");
  const bounds = await editorCanvas.boundingBox();
  expect(bounds, "the editor canvas is missing").not.toBeNull();
  await page.mouse.click(bounds!.x + bounds!.width * 0.5, bounds!.y + 282);
  await expect(editorCanvas).toHaveAttribute("data-active-sample-id", "basic-shapes");
  await expect
    .poll(
      () =>
        page.evaluate(() => (window as SelectionWindow).__donnerWorkerStats?.readbackCount ?? 0),
      { message: "the raster worker published no readback", timeout: 10000 },
    )
    .toBeGreaterThan(0);
  expect(
    await page.evaluate(() =>
      (window as SelectionWindow).__donnerWorkerStats?.readbackWaitStrategy
    ),
    "the raster worker read back through another backend than its package was built for",
  ).toBe(kExpectedRasterWorkerWait);

  expect(
    lines.filter((line) => line.includes(kBrowserBackendFailure)),
    "the browser backend reported a failure",
  ).toEqual([]);
  expect(errors, "the page threw").toEqual([]);
});
