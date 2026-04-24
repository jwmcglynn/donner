import { expect, type Page, test } from "@playwright/test";

const DRAG_FIXTURE_SVG = `<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256">
  <defs>
    <filter id="soft-shadow" x="-20%" y="-20%" width="140%" height="140%">
      <feDropShadow dx="0" dy="4" stdDeviation="4" flood-color="#1f2937" flood-opacity="0.35"/>
    </filter>
  </defs>
  <g id="target" filter="url(#soft-shadow)">
    <rect x="72" y="72" width="112" height="112" rx="12" fill="#f97316"/>
    <path d="M128 92 L104 138 H126 L116 164 L154 116 H132 Z" fill="#fef3c7"/>
  </g>
</svg>`;

type ImageRect = {
  left: number;
  top: number;
  width: number;
  height: number;
};

type CanvasPoint = {
  x: number;
  y: number;
};

type RuntimeLog = {
  adapterAcquisitionsAtReady: number;
  consoleLines: string[];
  startupMs: number;
};

async function openEditor(page: Page): Promise<RuntimeLog> {
  const consoleLines: string[] = [];
  page.on("console", (message) => consoleLines.push(message.text()));

  await page.goto("/?test=1");
  await page.waitForFunction(() => (window as any).__donner_ready === true, null, {
    timeout: 45_000,
  });

  const startupMs = await page.evaluate(() => (window as any).__donner_metrics.readyTimestampMs);
  const adapterAcquisitionsAtReady =
    consoleLines.filter((line) => line.includes("[Geode/emscripten] WebGPU adapter acquired"))
      .length;
  return { adapterAcquisitionsAtReady, consoleLines, startupMs };
}

async function loadSvgFixture(page: Page, svg: string): Promise<void> {
  const beforeLoadRenderCount = await page.evaluate(
    () => (window as any).__donner_metrics.renderCount,
  );
  await page.evaluate((source) => {
    const path = "/tmp/donner_drag_perf.svg";
    (window as any).FS.writeFile(path, source);
    (window as any).ccall("OnBrowserFileReadyPath", null, ["string"], [path]);
  }, svg);
  await page.waitForFunction(
    (count) => (window as any).__donner_metrics.renderCount > count,
    beforeLoadRenderCount,
    { timeout: 15_000 },
  );
}

async function waitForDocumentImageRect(page: Page): Promise<ImageRect> {
  await page.waitForTimeout(500);
  const imageRect = await page.waitForFunction(() => {
    const rect = (window as any).__donner_metrics?.documentImageRect;
    return rect && rect.width > 0 && rect.height > 0 ? rect : null;
  });
  return imageRect.jsonValue() as Promise<ImageRect>;
}

function documentPointToCanvas(image: ImageRect, x: number, y: number): CanvasPoint {
  return {
    x: image.left + image.width * (x / 256),
    y: image.top + image.height * (y / 256),
  };
}

async function warmSelectionAndPromotion(page: Page, start: CanvasPoint): Promise<void> {
  const beforeWarmupRenderCount = await page.evaluate(
    () => (window as any).__donner_metrics.renderCount,
  );
  await page.evaluate(async (point) => {
    (window as any).__donner_simulate([
      { type: "pointerdown", x: point.x, y: point.y, buttons: 1 },
    ]);
    await new Promise((resolve) => requestAnimationFrame(() => resolve(null)));
    (window as any).__donner_simulate([
      { type: "pointermove", x: point.x + 4, y: point.y, buttons: 1 },
    ]);
    await new Promise((resolve) => requestAnimationFrame(() => resolve(null)));
  }, start);
  await page.waitForFunction(
    (count) => (window as any).__donner_metrics.renderCount > count,
    beforeWarmupRenderCount,
    { timeout: 15_000 },
  );
}

async function measureSteadyDrag(page: Page, start: CanvasPoint, image: ImageRect) {
  await warmSelectionAndPromotion(page, start);
  const warmupState = await page.evaluate(() => ({
    marqueeActive: (window as any).__donner_metrics.marqueeActive,
    selectionCount: (window as any).__donner_metrics.selectionCount,
  }));

  const beforeRenderCount = await page.evaluate(
    () => (window as any).__donner_metrics.renderCount,
  );
  const beforeCounters = await page.evaluate(
    () => ({ ...(window as any).__donner_metrics.compositorFastPathCounters }),
  );
  const beforeUploads = await page.evaluate(() => ({
    compositedPreviewBitmapUploads: (window as any).__donner_metrics.compositedPreviewBitmapUploads,
    flatBitmapUploads: (window as any).__donner_metrics.flatBitmapUploads,
  }));

  const dragResult = await page.evaluate(async ({ point, imageWidth }) => {
    const startX = point.x + 4;
    const startY = point.y;
    const dragDistance = Math.max(24, Math.min(96, imageWidth * 0.25));
    const endX = startX + dragDistance;
    const endY = startY;
    const steps = 60;

    const tStart = performance.now();

    for (let i = 1; i <= steps; ++i) {
      const t = i / steps;
      const x = startX + (endX - startX) * t;
      const y = startY + (endY - startY) * t;
      (window as any).__donner_simulate([
        { type: "pointermove", x, y, buttons: 1 },
      ]);
      await new Promise((resolve) => requestAnimationFrame(() => resolve(null)));
    }

    (window as any).__donner_simulate([
      { type: "pointerup", x: endX, y: endY, buttons: 0 },
    ]);
    await new Promise((resolve) => setTimeout(resolve, 100));

    const tEnd = performance.now();
    return { dragMs: tEnd - tStart };
  }, { point: start, imageWidth: image.width });

  const metrics = await page.evaluate(() => (window as any).__donner_metrics);
  const framesDuringDrag = metrics.renderCount - beforeRenderCount;
  const counters = metrics.compositorFastPathCounters;
  const fastDelta = counters.fastPathFrames - beforeCounters.fastPathFrames;
  const slowDirtyDelta = counters.slowPathFramesWithDirty - beforeCounters.slowPathFramesWithDirty;
  const noDirtyDelta = counters.noDirtyFrames - beforeCounters.noDirtyFrames;
  const flatBitmapUploadDelta = metrics.flatBitmapUploads - beforeUploads.flatBitmapUploads;
  const compositedPreviewBitmapUploadDelta = metrics.compositedPreviewBitmapUploads
    - beforeUploads.compositedPreviewBitmapUploads;

  const allTimes: number[] = metrics.frameTimesMs;
  const dragTimes = allTimes.slice(Math.max(0, allTimes.length - framesDuringDrag));
  const sorted = [...dragTimes].sort((a, b) => a - b);

  return {
    framesDuringDrag,
    fastDelta,
    slowDirtyDelta,
    noDirtyDelta,
    flatBitmapUploadDelta,
    compositedPreviewActive: metrics.compositedPreviewActive,
    compositedPreviewBitmapUploadDelta,
    selectionCount: warmupState.selectionCount,
    marqueeActive: warmupState.marqueeActive,
    median: sorted[Math.floor(sorted.length / 2)],
    p95: sorted[Math.min(sorted.length - 1, Math.floor(sorted.length * 0.95))],
    worst: sorted[sorted.length - 1],
    dragMs: dragResult.dragMs,
  };
}

async function expectDragBudget(
  scenario: string,
  page: Page,
  image: ImageRect,
  start: CanvasPoint,
  runtime: RuntimeLog,
): Promise<void> {
  const result = await measureSteadyDrag(page, start, image);
  const geodeAdapterAcquisitions =
    runtime.consoleLines.filter((line) =>
      line.includes("[Geode/emscripten] WebGPU adapter acquired")
    ).length;

  // Log so a CI failure comes with the actual numbers (otherwise the
  // triage loop is "rerun with DEBUG=1 to learn what went wrong").
  // eslint-disable-next-line no-console
  console.log(
    `${scenario}: startup=${runtime.startupMs.toFixed(1)}ms `
      + `webgpuAdapters=${geodeAdapterAcquisitions} `
      + `webgpuAdaptersAtReady=${runtime.adapterAcquisitionsAtReady} `
      + `image=(${image.left.toFixed(1)},${image.top.toFixed(1)} `
      + `${image.width.toFixed(1)}x${image.height.toFixed(1)}) `
      + `start=(${start.x.toFixed(1)},${start.y.toFixed(1)}) `
      + `frames=${result.framesDuringDrag} `
      + `median=${result.median.toFixed(2)}ms `
      + `p95=${result.p95.toFixed(2)}ms `
      + `worst=${result.worst.toFixed(2)}ms `
      + `dragWall=${result.dragMs.toFixed(1)}ms `
      + `fastDelta=${result.fastDelta} `
      + `slowDirtyDelta=${result.slowDirtyDelta} `
      + `noDirtyDelta=${result.noDirtyDelta} `
      + `flatUploads=${result.flatBitmapUploadDelta} `
      + `previewUploads=${result.compositedPreviewBitmapUploadDelta} `
      + `previewActive=${result.compositedPreviewActive} `
      + `selectionCount=${result.selectionCount} `
      + `marqueeActive=${result.marqueeActive}`,
  );

  expect(result.selectionCount, `${scenario} should select the drag target`).toBeGreaterThan(0);
  expect(result.marqueeActive, `${scenario} should not start an empty-space marquee`).toBe(false);
  expect(
    result.framesDuringDrag,
    `expected at least 10 rendered frames during ${scenario} drag`,
  ).toBeGreaterThanOrEqual(10);
  expect(result.slowDirtyDelta).toBe(0);
  expect(result.fastDelta).toBeGreaterThanOrEqual(10);

  // Budgets (deliberately generous): gate "is drag catastrophic", not
  // "is it 120 FPS on a beefy dev machine". Tighten once the harness has
  // CI history and a known noise floor.
  expect(result.median).toBeLessThanOrEqual(20);
  expect(result.worst).toBeLessThanOrEqual(50);
}

async function beginDrag(page: Page, start: CanvasPoint): Promise<CanvasPoint> {
  const beforeRenderCount = await page.evaluate(
    () => (window as any).__donner_metrics.renderCount,
  );
  const dragPoint = { x: start.x + 8, y: start.y };
  await page.evaluate(async ({ downPoint, movePoint }) => {
    (window as any).__donner_simulate([
      { type: "pointerdown", x: downPoint.x, y: downPoint.y, buttons: 1 },
    ]);
    await new Promise((resolve) => requestAnimationFrame(() => resolve(null)));
    (window as any).__donner_simulate([
      { type: "pointermove", x: movePoint.x, y: movePoint.y, buttons: 1 },
    ]);
  }, { downPoint: start, movePoint: dragPoint });
  await page.waitForFunction(
    (count) => (window as any).__donner_metrics.renderCount > count,
    beforeRenderCount,
    { timeout: 15_000 },
  );
  await page.waitForTimeout(100);
  return dragPoint;
}

test("stationary drag does not post redundant backend move frames", async ({ page }) => {
  test.setTimeout(90_000);

  await openEditor(page);
  await loadSvgFixture(page, DRAG_FIXTURE_SVG);
  const image = await waitForDocumentImageRect(page);
  const start = {
    x: image.left + image.width * 0.5,
    y: image.top + image.height * 0.5,
  };
  const dragPoint = await beginDrag(page, start);

  const beforeCounters = await page.evaluate(
    () => ({ ...(window as any).__donner_metrics.compositorFastPathCounters }),
  );
  await page.evaluate(async (point) => {
    for (let i = 0; i < 10; ++i) {
      (window as any).__donner_simulate([
        { type: "pointermove", x: point.x, y: point.y, buttons: 1 },
      ]);
      await new Promise((resolve) => requestAnimationFrame(() => resolve(null)));
    }
  }, dragPoint);
  await page.waitForTimeout(100);
  const afterCounters = await page.evaluate(
    () => ({ ...(window as any).__donner_metrics.compositorFastPathCounters }),
  );

  expect(afterCounters.fastPathFrames).toBe(beforeCounters.fastPathFrames);
  expect(afterCounters.slowPathFramesWithDirty).toBe(beforeCounters.slowPathFramesWithDirty);
  expect(afterCounters.noDirtyFrames).toBe(beforeCounters.noDirtyFrames);

  await page.evaluate((point) => {
    (window as any).__donner_simulate([
      { type: "pointerup", x: point.x, y: point.y, buttons: 0 },
    ]);
  }, dragPoint);
});

test("filtered subtree drag hits its frame-time budget", async ({ page }) => {
  test.setTimeout(90_000);

  const runtime = await openEditor(page);
  await loadSvgFixture(page, DRAG_FIXTURE_SVG);

  const image = await waitForDocumentImageRect(page);
  const start = {
    x: image.left + image.width * 0.5,
    y: image.top + image.height * 0.5,
  };

  await expectDragBudget("filtered fixture drag", page, image, start, runtime);
});

test("default Donner icon lightning drag hits its frame-time budget", async ({ page }) => {
  test.setTimeout(90_000);

  const runtime = await openEditor(page);
  const iconSvg = await page.evaluate(async () => {
    const response = await fetch("/donner_icon.svg");
    return response.text();
  });
  await loadSvgFixture(page, iconSvg);
  const image = await waitForDocumentImageRect(page);

  // Point inside <g id="Lightning"> on the startup donner_icon.svg.
  const start = documentPointToCanvas(image, 126, 154);

  await expectDragBudget("donner icon lightning drag", page, image, start, runtime);
});
