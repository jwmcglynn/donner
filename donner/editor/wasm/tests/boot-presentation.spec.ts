import { expect, test } from "@playwright/test";
import { writeFile } from "node:fs/promises";

const baseUrl = process.env.DONNER_WASM_BASE_URL || "http://127.0.0.1:8000";

type BootSample = {
  /** Page clock when the animation frame ran, so a failure shows the cadence. */
  atMs: number;
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
  __bootPresentationProbe: {
    running: boolean;
    samples: BootSample[];
    // Every visibility the page had, timed. A hidden page is delivered no
    // animation frames at all, so a gap in the samples' cadence reads as a
    // starved compositor or a hidden page only with this beside it.
    visibility: Array<{ atMs: number; state: DocumentVisibilityState }>;
  };
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
    state.__bootPresentationProbe = {
      running: true,
      samples: [],
      visibility: [{ atMs: performance.now(), state: document.visibilityState }],
    };
    document.addEventListener("visibilitychange", () => {
      state.__bootPresentationProbe.visibility.push({
        atMs: performance.now(),
        state: document.visibilityState,
      });
    });
    const sample = () => {
      if (!state.__bootPresentationProbe.running) return;
      const canvas = document.querySelector("canvas");
      const loader = document.getElementById("loading-screen");
      if (canvas && loader) {
        const style = getComputedStyle(loader);
        const bounds = loader.getBoundingClientRect();
        state.__bootPresentationProbe.samples.push({
          atMs: performance.now(),
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
      // Startup must begin after the page is laid out at the new size, or the
      // canvas it configures says nothing about a resize. An animation frame
      // sampled at the new viewport with the loader still covering the page is
      // that evidence, and one is all it takes: nothing on the page changes
      // until startup is released, so every later frame repeats it.
      //
      // Waiting for five such frames inside a fixed five seconds measured the
      // browser's frame rate rather than the page. A cold browser on a shared
      // runner delivered three and four, failing runs whose loader covered
      // every frame it drew.
      await expect.poll(
        () =>
          page.evaluate(() =>
            (window as BootWindow).__bootPresentationProbe.samples.filter((sample) =>
              sample.viewportWidth === 1390 && sample.viewportHeight === 1121 && sample.covered
              && !sample.firstFrame
            ).length
          ),
        { message: "the page never drew a covered frame at the resized viewport" },
      ).toBeGreaterThanOrEqual(1);
      expect(await page.evaluate(() => (window as BootWindow).__donnerFirstFramePresented === true))
        .toBe(false);
    } finally {
      releaseStartup();
    }

    await expect(page.locator("#loading-screen")).toBeHidden({ timeout: 45000 });
    // The claim below is about the uncovered samples, so wait until there are
    // enough of them to carry it. Two animation frames is not enough of a wait:
    // this suite runs on a software rasterizer that can be starved to a handful
    // of frames per second, and an empty uncovered set would pass every filter.
    await expect.poll(
      () =>
        page.evaluate(() =>
          (window as BootWindow).__bootPresentationProbe.samples
            .filter((sample) => !sample.covered).length
        ),
      { timeout: 10000 },
    ).toBeGreaterThanOrEqual(5);
    const samples = await page.evaluate(() =>
      (window as BootWindow).__bootPresentationProbe.samples
    );
    expect(samples.some((sample) => !sample.firstFrame && sample.covered)).toBe(true);
    expect(samples.filter((sample) => sample.canvasCount !== 1)).toEqual([]);
    // The claim below is about the uncovered samples, so it is only worth
    // anything if there are some. The loader fades out over 160ms before it
    // leaves the page, so an uncovered run of one or two frames means the probe
    // stopped sampling, not that the editor was exposed for that long.
    const uncovered = samples.filter((sample) => !sample.covered);
    expect(uncovered.length).toBeGreaterThanOrEqual(5);
    expect(uncovered.filter((sample) =>
      !sample.firstFrame || sample.width !== sample.expectedWidth
      || sample.height !== sample.expectedHeight
    )).toEqual([]);
    // The reveal waits for the presented frame to reach the page and has a
    // 120-frame bound behind it for an engine that never reports the size. That
    // bound has to stay a backstop: if this ever climbs toward it, the engine
    // running here is not propagating the placeholder size and the gate needs a
    // capability probe rather than a larger bound.
    const framesAwaited = await page.evaluate(() =>
      (window as BootWindow).__donnerFramesAwaitingPresentedFrame
    );
    expect(framesAwaited).toBeGreaterThanOrEqual(0);
    expect(framesAwaited).toBeLessThan(20);
    expect(errors).toEqual([]);
  } finally {
    releaseStartup();
    let timer: ReturnType<typeof setTimeout> | undefined;
    const diagnostics = await Promise.race([
      page.evaluate(() => {
        const state = window as BootWindow;
        const probe = state.__bootPresentationProbe;
        if (probe) probe.running = false;
        return {
          samples: probe?.samples ?? [],
          visibility: probe?.visibility ?? [],
          framesAwaitingPresentedFrame: state.__donnerFramesAwaitingPresentedFrame,
          loader: document.getElementById("loading-screen")?.outerHTML,
          canvas: document.querySelector("canvas")?.outerHTML,
        };
      }).catch((error) => ({ error: String(error) })),
      new Promise((resolve) => {
        timer = setTimeout(() => resolve({ error: "diagnostic capture timed out" }), 5000);
      }),
    ]).finally(() => clearTimeout(timer));
    // Written to a file rather than attached as a body: the list reporter
    // prints neither, and a body attachment never reaches the output directory
    // that CI and Bazel keep from a failed run.
    const diagnosticsPath = testInfo.outputPath("boot-presentation-samples.json");
    await writeFile(diagnosticsPath, JSON.stringify({ diagnostics, errors }, null, 2));
    await testInfo.attach("boot-presentation-samples", {
      path: diagnosticsPath,
      contentType: "application/json",
    });
  }
});
