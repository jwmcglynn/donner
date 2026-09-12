import { expect, test } from "@playwright/test";

const baseUrl = process.env.DONNER_WASM_BASE_URL || "http://127.0.0.1:8000";

type BootSample = {
  firstFrame: boolean;
  covered: boolean;
  canvasCount: number;
  width: number;
  height: number;
  viewportWidth: number;
  viewportHeight: number;
  expectedWidth: number;
  expectedHeight: number;
};
type BootWindow = Window & {
  __donnerFirstFramePresented?: boolean;
  __bootPresentationProbe: { running: boolean; samples: BootSample[] };
};

test("delayed startup never exposes an unconfigured canvas", async ({ page }, testInfo) => {
  test.setTimeout(60000);
  const errors: string[] = [];
  page.on("pageerror", (error) => errors.push(error.message));

  let releaseStartup!: () => void;
  const startupGate = new Promise<void>((resolve) => {
    releaseStartup = resolve;
  });
  await page.route("**/editor.js", async (route) => {
    await startupGate;
    await route.continue();
  });
  await page.addInitScript(() => {
    const state = window as BootWindow;
    state.__bootPresentationProbe = { running: true, samples: [] };
    const sample = () => {
      if (!state.__bootPresentationProbe.running) return;
      const canvas = document.querySelector("canvas");
      const loader = document.getElementById("loading-screen");
      if (canvas && loader) {
        const style = getComputedStyle(loader);
        const bounds = loader.getBoundingClientRect();
        state.__bootPresentationProbe.samples.push({
          firstFrame: state.__donnerFirstFramePresented === true,
          covered: !loader.hidden && style.display !== "none" && style.visibility === "visible"
            && Number(style.opacity) === 1 && bounds.left <= 0 && bounds.top <= 0
            && bounds.right >= innerWidth && bounds.bottom >= innerHeight,
          canvasCount: document.querySelectorAll("canvas").length,
          width: canvas.width,
          height: canvas.height,
          viewportWidth: innerWidth,
          viewportHeight: innerHeight,
          expectedWidth: Math.max(1, Math.floor(innerWidth * devicePixelRatio)),
          expectedHeight: Math.max(1, Math.floor(innerHeight * devicePixelRatio)),
        });
      }
      requestAnimationFrame(sample);
    };
    requestAnimationFrame(sample);
  });

  try {
    try {
      await page.goto(`${baseUrl}/index.html`, { waitUntil: "domcontentloaded" });
      await expect(page.locator("#loading-screen")).toBeVisible();
      await expect(page.locator("canvas")).toHaveCount(1);
      await page.setViewportSize({ width: 1390, height: 1121 });
      await expect.poll(() =>
        page.evaluate(() =>
          (window as BootWindow).__bootPresentationProbe.samples.filter((sample) =>
            sample.viewportWidth === 1390 && sample.viewportHeight === 1121
          ).length
        )
      ).toBeGreaterThanOrEqual(5);
      expect(await page.evaluate(() => (window as BootWindow).__donnerFirstFramePresented === true))
        .toBe(false);
    } finally {
      releaseStartup();
    }

    await expect(page.locator("#loading-screen")).toBeHidden({ timeout: 45000 });
    await page.evaluate(() =>
      new Promise<void>((resolve) => {
        requestAnimationFrame(() => requestAnimationFrame(() => resolve()));
      })
    );
    const samples = await page.evaluate(() =>
      (window as BootWindow).__bootPresentationProbe.samples
    );
    expect(samples.some((sample) => !sample.firstFrame && sample.covered)).toBe(true);
    expect(samples.some((sample) => !sample.covered)).toBe(true);
    expect(samples.filter((sample) => sample.canvasCount !== 1)).toEqual([]);
    expect(samples.filter((sample) =>
      !sample.covered && (
        !sample.firstFrame || sample.width !== sample.expectedWidth
        || sample.height !== sample.expectedHeight
      )
    )).toEqual([]);
    expect(errors).toEqual([]);
  } finally {
    releaseStartup();
    let timer: ReturnType<typeof setTimeout> | undefined;
    const diagnostics = await Promise.race([
      page.evaluate(() => {
        const probe = (window as BootWindow).__bootPresentationProbe;
        if (probe) probe.running = false;
        return {
          samples: probe?.samples ?? [],
          loader: document.getElementById("loading-screen")?.outerHTML,
          canvas: document.querySelector("canvas")?.outerHTML,
        };
      }).catch((error) => ({ error: String(error) })),
      new Promise((resolve) => {
        timer = setTimeout(() => resolve({ error: "diagnostic capture timed out" }), 5000);
      }),
    ]).finally(() => clearTimeout(timer));
    await testInfo.attach("boot-presentation-samples", {
      body: JSON.stringify({ diagnostics, errors }, null, 2),
      contentType: "application/json",
    });
  }
});
