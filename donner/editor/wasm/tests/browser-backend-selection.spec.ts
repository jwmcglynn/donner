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

const baseUrl = process.env.DONNER_WASM_BASE_URL || "http://127.0.0.1:8000";

type FirstFrameWindow = Window & { __donnerFirstFramePresented?: boolean };

test("the raster worker selects the backend its package was built for", async ({ page }) => {
  test.setTimeout(60000);
  const lines: string[] = [];
  page.on("console", (message) => lines.push(message.text()));
  const errors: string[] = [];
  page.on("pageerror", (error) => errors.push(error.message));

  await page.goto(`${baseUrl}/index.html`, { waitUntil: "domcontentloaded" });
  if (kExpectsBrowserBackend) {
    await expect
      .poll(() => lines.some((line) => line.includes(kSelectedByBuildSetting)), {
        message: "no selection in the page landed on the browser backend",
        timeout: 45000,
      })
      .toBe(true);
  } else {
    await page.waitForFunction(
      () => (window as FirstFrameWindow).__donnerFirstFramePresented === true,
      undefined,
      { timeout: 45000 },
    );
    expect(
      lines.filter((line) => line.includes(kSelectedByBuildSetting)),
      "a package built without the browser backend selected it",
    ).toEqual([]);
  }

  expect(
    lines.filter((line) => line.includes(kBrowserBackendFailure)),
    "the browser backend reported a failure",
  ).toEqual([]);
  expect(errors, "the page threw").toEqual([]);
});
