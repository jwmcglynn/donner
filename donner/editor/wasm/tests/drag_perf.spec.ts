import { test, expect } from "@playwright/test";

// Drag-perf gate: dispatch a 1-second, 60-event pointerdown→move…→pointerup
// sequence across the canvas and assert the WASM editor keeps up. This is
// the regression class that motivated building this harness in the first
// place — drag lag only reproduces under real browser pointer events +
// emscripten-glfw's handler wiring, which desktop (GLFW/Cocoa) tests miss.
//
// Budgets are intentionally loose: we're gating "is drag catastrophic",
// not "is it 120 FPS on a beefy dev machine". Tighten later, once the
// harness has run in CI for long enough to know the real noise floor.

test("drag across the canvas hits its frame-time budget", async ({ page }) => {
  test.setTimeout(90_000);

  await page.goto("/?test=1");
  await page.waitForFunction(() => (window as any).__donner_ready === true, null, {
    timeout: 45_000,
  });

  // Let the editor settle into idle before we start measuring — otherwise
  // startup/layout frames pollute the buffer.
  await page.waitForTimeout(500);

  // Snapshot the metrics baseline, then drive the drag.
  const beforeRenderCount = await page.evaluate(
      () => (window as any).__donner_metrics.renderCount);

  // Build a 60-step horizontal sweep (left-ish → right-ish) spread over
  // ~1 second. 60 events at ~16ms apart matches a 60 Hz move cadence.
  // We stay inside the canvas interior to avoid hitting chrome / toolbar.
  const dragResult = await page.evaluate(async () => {
    const canvas = document.getElementById("canvas") as HTMLCanvasElement;
    const rect = canvas.getBoundingClientRect();
    const startX = rect.width * 0.3;
    const startY = rect.height * 0.5;
    const endX = rect.width * 0.7;
    const endY = rect.height * 0.5;
    const steps = 60;

    const tStart = performance.now();

    // Press.
    (window as any).__donner_simulate([
      { type: "pointerdown", x: startX, y: startY, buttons: 1 },
    ]);

    for (let i = 1; i <= steps; ++i) {
      const t = i / steps;
      const x = startX + (endX - startX) * t;
      const y = startY + (endY - startY) * t;
      (window as any).__donner_simulate([
        { type: "pointermove", x, y, buttons: 1 },
      ]);
      // Wait one animation frame between moves so the render loop actually
      // gets a chance to process each event, rather than the browser
      // batching all 60 and sampling one frame.
      await new Promise((resolve) => requestAnimationFrame(() => resolve(null)));
    }

    (window as any).__donner_simulate([
      { type: "pointerup", x: endX, y: endY, buttons: 0 },
    ]);

    // Small tail so the final frame lands in the metrics buffer.
    await new Promise((resolve) => setTimeout(resolve, 100));

    const tEnd = performance.now();
    return { dragMs: tEnd - tStart };
  });

  const metrics = await page.evaluate(() => (window as any).__donner_metrics);
  const framesDuringDrag = metrics.renderCount - beforeRenderCount;

  expect(framesDuringDrag,
         `expected at least 10 rendered frames during drag, got ${framesDuringDrag}`)
      .toBeGreaterThanOrEqual(10);

  // Pull the last `framesDuringDrag` entries from the frame-time ring —
  // those are the drag-window samples. Fall back to the whole ring if
  // ordering got weird, but we should normally only measure drag frames.
  const allTimes: number[] = metrics.frameTimesMs;
  const dragTimes = allTimes.slice(Math.max(0, allTimes.length - framesDuringDrag));

  const sorted = [...dragTimes].sort((a, b) => a - b);
  const median = sorted[Math.floor(sorted.length / 2)];
  const p95 = sorted[Math.min(sorted.length - 1, Math.floor(sorted.length * 0.95))];
  const worst = sorted[sorted.length - 1];

  // Log so a CI failure comes with the actual numbers (otherwise the
  // triage loop is "rerun with DEBUG=1 to learn what went wrong").
  // eslint-disable-next-line no-console
  console.log(`drag: frames=${framesDuringDrag} median=${median.toFixed(2)}ms ` +
              `p95=${p95.toFixed(2)}ms worst=${worst.toFixed(2)}ms ` +
              `dragWall=${dragResult.dragMs.toFixed(1)}ms`);

  // Budgets (deliberately generous — see file-header comment):
  //   median frame  ≤ 20ms  (50 FPS floor for the typical drag frame)
  //   worst frame   ≤ 50ms  (a 20 FPS hiccup is the red-line, not the norm)
  expect(median).toBeLessThanOrEqual(20);
  expect(worst).toBeLessThanOrEqual(50);
});
