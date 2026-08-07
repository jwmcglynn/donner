import { expect, type Page, test } from "@playwright/test";
import {
  captureSplashCompositeFrame,
  type CssRegion,
  type PixelBounds,
  readEditorBackgroundCoverage,
  readCssPngPixelDifferenceStats,
  readEditorPixelBoundsFromPng,
  readEditorResizePixelBounds,
  readEditorPixelBounds,
  readElementColorStats,
  readPngPixelDifferenceStats,
  readSplashCompositeFrameStats,
  readSplashPageCoverageStats,
} from "./canvas-color-stats";

declare global {
  interface Window {
    __donnerBackend?: string;
    __donnerCanStartWasm?: boolean;
    __donnerWorkerStats?: {
      completedResults: number;
    };
    __donnerAcceptedPresentation?: {
      kind: "geode";
      selectionChromeBaked?: boolean;
      token: number;
    };
    __donnerWorkerSurfaceMode?: "direct-surface" | "bitmap-bridge";
    __donnerWorkerSurfaceLayoutPolicy?: "single-visible";
    __donnerAcceptedFrameMutations?: number[];
    __donnerAcceptedFrameBoundaryViolations?: Array<{ acknowledged: number; frame: number }>;
    __donnerAcceptedFrameObserver?: MutationObserver;
    __donnerDirectSurfaceTaskBoundaryToken?: number;
    __donnerObserveDirectSurfaceAcceptance?: (frameToken: number) => void;
  }
}

const kBaseUrl = process.env.DONNER_WASM_BASE_URL || "http://127.0.0.1:8000";

interface DirectSurfaceState {
  acceptedZIndex: number;
  compositorResidentCount: number;
  computedVisibleCount: number;
  frame: number;
  inactiveZIndex: number;
  token: number;
  visibleCount: number;
}

async function readDirectSurfaceState(page: Page): Promise<DirectSurfaceState> {
  return page.evaluate(() => {
    const visible = Array.from(
      document.querySelectorAll<HTMLCanvasElement>(
        "canvas[data-direct-surface-visible=\"true\"]",
      ),
    );
    const surfaces = [
      document.getElementById("donner-document-canvas"),
      document.getElementById("donner-document-canvas-back"),
    ].filter((surface): surface is HTMLCanvasElement => surface instanceof HTMLCanvasElement);
    const accepted = visible[0];
    const inactive = surfaces.find((surface) => surface !== accepted);
    return {
      acceptedZIndex: Number(accepted?.style.zIndex || -1),
      compositorResidentCount: surfaces.filter((surface) =>
        surface.dataset.directSurfaceCompositorResident === "true"
      ).length,
      computedVisibleCount: surfaces.filter((surface) =>
        getComputedStyle(surface).visibility === "visible"
      ).length,
      frame: Number(accepted?.dataset.directSurfaceFrame || 0),
      inactiveZIndex: Number(inactive?.style.zIndex || -1),
      token: window.__donnerAcceptedPresentation?.token || 0,
      visibleCount: visible.length,
    };
  });
}

async function openEditor(page: Page): Promise<string[]> {
  const failures: string[] = [];
  page.on("console", (message) => {
    if (
      /Failed to wake Wasm renderer pthread|Wasm renderer pthread wake rejected|Aborted|RuntimeError|UTILS_RELEASE_ASSERT/i
        .test(
          message.text(),
        )
    ) {
      failures.push(`[console:${message.type()}] ${message.text()}`);
    }
  });
  page.on("pageerror", (error) => failures.push(`[pageerror] ${error.message}`));

  await page.goto(kBaseUrl, { waitUntil: "domcontentloaded" });
  await expect.poll(() => page.evaluate(() => window.__donnerCanStartWasm)).toBe(true);
  await expect(page.locator("#status")).toBeHidden({ timeout: 20_000 });
  return failures;
}

async function openBasicShapes(page: Page): Promise<{
  canvasBounds: { x: number; y: number; width: number; height: number };
  documentClip: { x: number; y: number; width: number; height: number };
}> {
  const editorCanvas = page.locator("canvas#canvas");
  const canvasBounds = await editorCanvas.boundingBox();
  expect(canvasBounds).not.toBeNull();
  if (canvasBounds === null) {
    throw new Error("editor canvas is missing");
  }

  const beforeSampleFrame = await page.evaluate(
    () => window.__donnerAcceptedPresentation?.token || 0,
  );
  await page.mouse.click(canvasBounds.x + canvasBounds.width * 0.5, canvasBounds.y + 282);
  await expect(editorCanvas).toHaveAttribute("data-active-sample-id", "basic-shapes");
  await expect(page.locator("canvas[data-direct-surface-visible=\"true\"]")).toBeVisible();
  await expect
    .poll(() => page.evaluate(() => window.__donnerAcceptedPresentation?.token || 0), {
      message: "Basic Shapes must have its own accepted worker presentation before capture",
      timeout: 5_000,
      intervals: [16, 25, 50, 100],
    })
    .toBeGreaterThan(beforeSampleFrame);
  await waitForBrowserComposite(page);

  return {
    canvasBounds,
    documentClip: {
      x: canvasBounds.x + 280,
      y: canvasBounds.y + 250,
      width: 660,
      height: 430,
    },
  };
}

async function toggleViewMenuItem(page: Page, canvasX: number, itemY: number): Promise<void> {
  await page.mouse.click(canvasX + 260, 11);
  await page.mouse.click(canvasX + 330, itemY);
  await page.evaluate(() => new Promise((resolve) => requestAnimationFrame(() => resolve(null))));
}

async function waitForBrowserComposite(page: Page): Promise<void> {
  await page.evaluate(() =>
    new Promise((resolve) =>
      requestAnimationFrame(() => requestAnimationFrame(() => resolve(null)))
    )
  );
}

test.use({ viewport: { width: 1600, height: 900 } });

test("Geode Wasm View overlays render tile metadata and sparse Slug triangle edges", async ({ browserName, page }) => {
  test.skip(browserName !== "firefox", "Firefox Geode direct-surface regression");
  const failures = await openEditor(page);
  const { canvasBounds, documentClip } = await openBasicShapes(page);
  const documentBounds = await page.locator("canvas[data-direct-surface-visible=\"true\"]")
    .boundingBox();
  expect(documentBounds).not.toBeNull();
  if (documentBounds === null) {
    throw new Error("accepted document surface is missing");
  }
  const documentIntersectionLeft = Math.max(documentClip.x, documentBounds.x);
  const documentIntersectionTop = Math.max(documentClip.y, documentBounds.y);
  const documentIntersectionRight = Math.min(
    documentClip.x + documentClip.width,
    documentBounds.x + documentBounds.width,
  );
  const documentIntersectionBottom = Math.min(
    documentClip.y + documentClip.height,
    documentBounds.y + documentBounds.height,
  );
  const documentPixelsInClip = {
    x: documentIntersectionLeft - documentClip.x,
    y: documentIntersectionTop - documentClip.y,
    width: Math.max(0, documentIntersectionRight - documentIntersectionLeft),
    height: Math.max(0, documentIntersectionBottom - documentIntersectionTop),
  };
  const baseline = await page.screenshot({ clip: documentClip });
  await test.info().attach("overlay-baseline", { body: baseline, contentType: "image/png" });

  const beforeCompositorFrame = await page.evaluate(
    () => window.__donnerAcceptedPresentation?.token || 0,
  );
  await toggleViewMenuItem(page, canvasBounds.x, 155);
  await expect
    .poll(() => page.evaluate(() => window.__donnerAcceptedPresentation?.token || 0), {
      message: "Compositor Tile Overlay must publish metadata for a newly accepted worker frame",
      timeout: 2_000,
      intervals: [16, 25, 50, 100],
    })
    .toBeGreaterThan(beforeCompositorFrame);
  await waitForBrowserComposite(page);
  const compositorOverlay = await page.screenshot({ clip: documentClip });
  await test.info().attach("compositor-tile-overlay", {
    body: compositorOverlay,
    contentType: "image/png",
  });
  const compositorDifference = readCssPngPixelDifferenceStats(
    baseline,
    compositorOverlay,
    documentClip,
    documentPixelsInClip,
  );
  expect(
    compositorDifference.changedPixels,
    "Compositor Tile Overlay was checked but contributed no visible document pixels",
  ).toBeGreaterThan(0);

  // Disable the independent compositor overlay before checking renderer
  // geometry pixels, so tile labels cannot masquerade as Slug edges.
  await toggleViewMenuItem(page, canvasBounds.x, 155);
  await waitForBrowserComposite(page);
  const geometryBaseline = await page.screenshot({ clip: documentClip });
  await test.info().attach("overlay-disabled-baseline", {
    body: geometryBaseline,
    contentType: "image/png",
  });
  // The screenshot clip includes a few pixels of animated render-pane chrome
  // above the direct surface. Compare only the accepted document surface;
  // Firefox may change those unrelated top-bar pixels while menus close.
  const restoredBaselineDifference = readCssPngPixelDifferenceStats(
    baseline,
    geometryBaseline,
    documentClip,
    documentPixelsInClip,
  );
  expect(
    restoredBaselineDifference.changedPixels,
    `Disabling Compositor Tile Overlay must restore document pixels: ${
      JSON.stringify(restoredBaselineDifference)
    }`,
  ).toBe(0);

  const documentPointInClip = (documentX: number, documentY: number) => ({
    x: documentBounds.x - documentClip.x + documentX * documentBounds.width / 640,
    y: documentBounds.y - documentClip.y + documentY * documentBounds.height / 400,
  });
  // The blue rounded rectangle spans (32,32)-(212,152). Its emitted
  // triangles share the diagonal through (122,92). Dynamic Slug dilation
  // moves the right submitted edge slightly beyond x=212, so probe a
  // neighborhood around 212.7 rather than assuming the pre-vertex bound.
  // (80,120) is normal fill well away from every triangle edge.
  const sharedTriangleEdge = documentPointInClip(122, 92);
  const dilatedOuterTriangleEdge = documentPointInClip(212.7, 92);
  const untouchedBlueInterior = documentPointInClip(80, 120);

  const beforeGeometryFrame = await page.evaluate(
    () => window.__donnerAcceptedPresentation?.token || 0,
  );
  await toggleViewMenuItem(page, canvasBounds.x, 176);
  await expect
    .poll(() => page.evaluate(() => window.__donnerAcceptedPresentation?.token || 0), {
      message: "Geometry Debug Overlay must schedule and accept a freshly rasterized worker frame",
      timeout: 2_000,
      intervals: [16, 25, 50, 100],
    })
    .toBeGreaterThan(beforeGeometryFrame);
  await waitForBrowserComposite(page);
  const geometryOverlay = await page.screenshot({ clip: documentClip });
  await test.info().attach("geometry-debug-overlay", {
    body: geometryOverlay,
    contentType: "image/png",
  });
  expect(
    geometryOverlay.equals(geometryBaseline),
    "Geometry Debug Overlay was checked and accepted, but contributed no visible canvas pixels",
  ).toBe(false);
  const edgeDifference = readCssPngPixelDifferenceStats(geometryBaseline, geometryOverlay, documentClip, {
    x: sharedTriangleEdge.x - 3,
    y: sharedTriangleEdge.y - 3,
    width: 7,
    height: 7,
  });
  expect(
    edgeDifference.changedPixels,
    "Geometry Debug Overlay must expose the shared edge from the actual Slug triangles",
  ).toBeGreaterThan(0);
  const outerEdgeDifference = readCssPngPixelDifferenceStats(
    geometryBaseline,
    geometryOverlay,
    documentClip,
    {
    x: dilatedOuterTriangleEdge.x - 4,
    y: dilatedOuterTriangleEdge.y - 4,
    width: 9,
    height: 9,
    },
  );
  expect(
    outerEdgeDifference.changedPixels,
    "Geometry Debug Overlay must expose the dynamically-dilated outer Slug edge",
  ).toBeGreaterThan(0);
  const interiorDifference = readCssPngPixelDifferenceStats(
    geometryBaseline,
    geometryOverlay,
    documentClip,
    {
    x: untouchedBlueInterior.x - 6,
    y: untouchedBlueInterior.y - 6,
    width: 13,
    height: 13,
    },
  );
  expect(
    interiorDifference.changedPixels,
    "Geometry Debug Overlay must preserve normal document colors between triangle edges",
  ).toBe(0);
  expect(failures).toEqual([]);
});

test("Firefox presents every accepted drag epoch on one stable surface", async ({
  browserName,
  page,
}) => {
  test.skip(browserName !== "firefox", "Firefox direct-surface regression");
  const failures = await openEditor(page);
  expect(await page.evaluate(() => window.__donnerWorkerSurfaceMode)).toBe("direct-surface");
  expect(await page.evaluate(() => window.__donnerWorkerSurfaceLayoutPolicy)).toBe(
    "single-visible",
  );
  const editorCanvas = page.locator("canvas#canvas");
  const editorBounds = await editorCanvas.boundingBox();
  expect(editorBounds).not.toBeNull();
  if (editorBounds === null) {
    return;
  }

  const beforeSample = await page.evaluate(() => window.__donnerWorkerStats?.completedResults || 0);
  await page.mouse.click(editorBounds.x + editorBounds.width * 0.5, editorBounds.y + 282);
  await expect(editorCanvas).toHaveAttribute("data-active-sample-id", "basic-shapes");
  await expect
    .poll(() => page.evaluate(() => window.__donnerWorkerStats?.completedResults || 0))
    .toBeGreaterThan(beforeSample);

  const documentCanvas = page.locator("canvas[data-direct-surface-visible=\"true\"]");
  await expect(documentCanvas).toBeVisible();
  const documentBounds = await documentCanvas.boundingBox();
  expect(documentBounds).not.toBeNull();
  if (documentBounds === null) {
    return;
  }
  await expect
    .poll(() => readElementColorStats(documentCanvas).then((stats) => stats.coloredPixels), {
      message: "expected the initial Basic Shapes worker surface before starting the drag",
      timeout: 2_000,
      intervals: [16, 25, 50, 100],
    })
    .toBeGreaterThan(500);
  const baseline = await readElementColorStats(documentCanvas);
  const boundaryToken = Number(
    await documentCanvas.getAttribute("data-direct-surface-frame"),
  );
  await page.evaluate((token) => {
    window.__donnerAcceptedFrameMutations = [];
    window.__donnerAcceptedFrameBoundaryViolations = [];
    window.__donnerObserveDirectSurfaceAcceptance = (frame) => {
      const acknowledged = window.__donnerDirectSurfaceTaskBoundaryToken || 0;
      if (frame > acknowledged) {
        window.__donnerAcceptedFrameBoundaryViolations?.push({ acknowledged, frame });
      }
    };
    window.__donnerAcceptedFrameObserver?.disconnect();
    const surfaces = [
      document.getElementById("donner-document-canvas"),
      document.getElementById("donner-document-canvas-back"),
    ].filter((surface): surface is HTMLCanvasElement => surface instanceof HTMLCanvasElement);
    window.__donnerAcceptedFrameObserver = new MutationObserver((mutations) => {
      for (const mutation of mutations) {
        const frame = Number(
          (mutation.target as HTMLCanvasElement).dataset.directSurfaceFrame || 0,
        );
        if (frame > token) {
          window.__donnerAcceptedFrameMutations?.push(frame);
        }
      }
    });
    for (const surface of surfaces) {
      window.__donnerAcceptedFrameObserver.observe(surface, {
        attributeFilter: ["data-direct-surface-frame"],
      });
    }
  }, boundaryToken);

  const dragStart = {
    x: documentBounds.x + documentBounds.width * (122 / 640),
    y: documentBounds.y + documentBounds.height * (92 / 400),
  };
  await page.mouse.move(dragStart.x, dragStart.y);
  await page.mouse.down();

  const samples: Array<{
    acceptedZIndex: number;
    coloredPixels: number;
    compositorResidentCount: number;
    computedVisibleCount: number;
    frame: number;
    inactiveZIndex: number;
    token: number;
    visibleCount: number;
    blue: PixelBounds;
    teal: PixelBounds;
  }> = [];
  const probeRegion = {
    x: editorBounds.x + 280,
    y: editorBounds.y + 260,
    width: 460,
    height: 280,
  };
  let previousFrame = boundaryToken;
  for (let step = 1; step <= 16; ++step) {
    await page.mouse.move(dragStart.x + step * 6, dragStart.y + step * 3);
    await expect
      .poll(async () =>
        Number(
          await page.locator("canvas[data-direct-surface-visible=\"true\"]").getAttribute(
            "data-direct-surface-frame",
          ),
        ), {
        message: `expected an accepted worker surface for drag step ${step}`,
        timeout: 2_000,
        intervals: [16, 25, 50, 100],
      })
      .toBeGreaterThan(previousFrame);
    const visibleCanvas = page.locator("canvas[data-direct-surface-visible=\"true\"]");
    let state: DirectSurfaceState | null = null;
    let geometry: { blue: PixelBounds | null; teal: PixelBounds | null } | null = null;
    let stableFrame = 0;
    for (let attempt = 0; attempt < 4; ++attempt) {
      const beforeState = await readDirectSurfaceState(page);
      geometry = await readEditorResizePixelBounds(page, probeRegion);
      const afterState = await readDirectSurfaceState(page);
      if (beforeState.frame === afterState.frame) {
        stableFrame = afterState.frame;
        state = afterState;
        break;
      }
    }
    expect(state).not.toBeNull();
    if (state === null) {
      continue;
    }
    expect(stableFrame, `accepted epoch changed throughout screenshot for step ${step}`).toBe(
      state.frame,
    );
    expect(geometry?.blue, `accepted epoch ${state.frame} had no blue document pixels`).not
      .toBeNull();
    expect(geometry?.teal, `accepted epoch ${state.frame} had no teal overlay pixels`).not
      .toBeNull();
    if (
      geometry?.blue === null || geometry?.blue === undefined || geometry.teal === null
      || geometry.teal === undefined
    ) {
      continue;
    }
    samples.push({
      ...state,
      blue: geometry.blue,
      coloredPixels: geometry.blue.pixels,
      teal: geometry.teal,
    });
    previousFrame = state.frame;
  }
  await page.mouse.up();

  expect(samples.map((sample) => sample.visibleCount)).toEqual(samples.map(() => 1));
  expect(samples.map((sample) => sample.compositorResidentCount)).toEqual(samples.map(() => 1));
  expect(samples.map((sample) => sample.computedVisibleCount)).toEqual(samples.map(() => 1));
  expect(samples.map((sample) => sample.acceptedZIndex)).toEqual(samples.map(() => 1));
  expect(samples.map((sample) => sample.inactiveZIndex)).toEqual(samples.map(() => 0));
  expect(samples.map((sample) => sample.frame)).toEqual(
    [...samples.map((sample) => sample.frame)].sort((a, b) => a - b),
  );
  expect(samples.at(-1)?.frame || 0).toBeGreaterThan(samples[0]?.frame || 0);
  expect(samples.map((sample) => sample.token)).toEqual(
    samples.map((sample) => sample.frame),
  );
  const centerX = (bounds: PixelBounds) => (bounds.minX + bounds.maxX) * 0.5;
  const centerY = (bounds: PixelBounds) => (bounds.minY + bounds.maxY) * 0.5;
  for (const sample of samples) {
    expect(
      Math.abs(centerX(sample.blue) - centerX(sample.teal)),
      `accepted epoch ${sample.frame} showed historical document X geometry: `
        + `blue=${JSON.stringify(sample.blue)} teal=${JSON.stringify(sample.teal)}`,
    ).toBeLessThanOrEqual(3);
    expect(
      Math.abs(centerY(sample.blue) - centerY(sample.teal)),
      `accepted epoch ${sample.frame} showed historical document Y geometry: `
        + `blue=${JSON.stringify(sample.blue)} teal=${JSON.stringify(sample.teal)}`,
    ).toBeLessThanOrEqual(3);
  }
  const blueCenters = samples.map((sample) => centerX(sample.blue));
  expect(blueCenters).toEqual([...blueCenters].sort((a, b) => a - b));
  expect(blueCenters.at(-1) || 0).toBeGreaterThan(blueCenters[0] || 0);
  const observedFrames = await page.evaluate(() => window.__donnerAcceptedFrameMutations || []);
  expect(observedFrames).toEqual([...observedFrames].sort((a, b) => a - b));
  expect(samples.every((sample) => observedFrames.includes(sample.frame))).toBe(true);
  expect(
    await page.evaluate(() => window.__donnerAcceptedFrameBoundaryViolations || []),
    "a direct surface became accepted before its later worker event-turn acknowledgment",
  ).toEqual([]);
  expect(
    samples.map((sample) => sample.coloredPixels),
    `baseline=${baseline.coloredPixels}; frames=${JSON.stringify(samples)}`,
  ).not.toContain(0);
  expect(failures).toEqual([]);
});

test("Firefox never exposes the checkerboard while dragging a Splash letter", async ({
  browserName,
  page,
}) => {
  test.skip(browserName !== "firefox", "Firefox direct-surface regression");
  const failures = await openEditor(page);
  const editorCanvas = page.locator("canvas#canvas");
  const editorBounds = await editorCanvas.boundingBox();
  expect(editorBounds).not.toBeNull();
  if (editorBounds === null) {
    return;
  }

  await page.mouse.click(editorBounds.x + editorBounds.width * 0.24, editorBounds.y + 282);
  await expect(editorCanvas).toHaveAttribute("data-active-sample-id", "donner-splash");

  const visibleSurface = page.locator("canvas[data-direct-surface-visible=\"true\"]");
  await expect(visibleSurface).toBeVisible();
  const documentBounds = await visibleSurface.boundingBox();
  expect(documentBounds).not.toBeNull();
  if (documentBounds === null) {
    return;
  }
  const documentRegion = {
    x: Math.max(editorBounds.x, documentBounds.x) + 2,
    y: Math.max(editorBounds.y, documentBounds.y) + 2,
    width: Math.min(
      editorBounds.x + editorBounds.width,
      documentBounds.x + documentBounds.width,
    ) - Math.max(editorBounds.x, documentBounds.x) - 4,
    height: Math.min(
      editorBounds.y + editorBounds.height,
      documentBounds.y + documentBounds.height,
    ) - Math.max(editorBounds.y, documentBounds.y) - 4,
  };
  const baseline = await readSplashPageCoverageStats(page, documentRegion);
  expect(
    baseline.darkBackgroundPixels,
    `Splash background was not present before drag: ${JSON.stringify(baseline)}`,
  ).toBeGreaterThan(baseline.samples * 0.25);
  expect(
    baseline.checkerboardPixels,
    `checkerboard leaked through the accepted Splash baseline: ${JSON.stringify(baseline)}`,
  ).toBeLessThan(baseline.samples * 0.1);

  // Target Donner_D's solid left stem rather than its counter, where hit-testing would correctly
  // select the background behind the letter. Tiny pointer steps keep the worker continuously
  // superseded, which is the presentation window where an unready direct surface used to replace
  // the last complete epoch.
  const dragStart = {
    x: documentBounds.x + 282,
    y: documentBounds.y + 400,
  };
  await page.mouse.move(dragStart.x, dragStart.y);
  await page.mouse.down();
  const samples: Array<{
    coverage: Awaited<ReturnType<typeof readSplashPageCoverageStats>>;
    frame: number;
    teal: PixelBounds | null;
    yellow: PixelBounds | null;
  }> = [];
  const letterRegion = {
    x: documentBounds.x + 195,
    y: documentBounds.y + 295,
    width: 138,
    height: Math.min(115, documentBounds.height - 297),
  };
  let previousFrame = Number(await visibleSurface.getAttribute("data-direct-surface-frame"));
  for (let step = 1; step <= 10; ++step) {
    await page.mouse.move(dragStart.x - step * 5, dragStart.y - step * 2.5);
    await expect
      .poll(async () => Number(
        await page.locator("canvas[data-direct-surface-visible=\"true\"]").getAttribute(
          "data-direct-surface-frame",
        ),
      ), {
        message: `expected an accepted Splash drag epoch for step ${step}`,
        timeout: 2_000,
        intervals: [16, 25, 50, 100],
      })
      .toBeGreaterThan(previousFrame);
    const frame = Number(
      await page.locator("canvas[data-direct-surface-visible=\"true\"]").getAttribute(
        "data-direct-surface-frame",
      ),
    );
    await waitForBrowserComposite(page);
    samples.push({
      coverage: await readSplashPageCoverageStats(page, documentRegion),
      frame,
      teal: await readEditorPixelBounds(page, letterRegion, "selection-teal"),
      yellow: await readEditorPixelBounds(page, letterRegion, "splash-yellow"),
    });
    previousFrame = frame;
  }
  await page.mouse.up();

  const centerX = (bounds: PixelBounds) => (bounds.minX + bounds.maxX) * 0.5;
  const firstAlignedSample = samples.find(
    (sample) => sample.yellow !== null && sample.teal !== null,
  );
  expect(firstAlignedSample, "no accepted Splash epoch contained both letter and outline pixels")
    .toBeDefined();
  const baselineAlignment = firstAlignedSample?.yellow !== null
      && firstAlignedSample?.yellow !== undefined
      && firstAlignedSample.teal !== null
      && firstAlignedSample.teal !== undefined
    ? {
      x: firstAlignedSample.yellow.minX - firstAlignedSample.teal.minX,
    }
    : { x: 0 };
  for (const [index, sample] of samples.entries()) {
    expect(
      sample.coverage.darkBackgroundPixels,
      `drag sample ${index + 1} exposed an incomplete document epoch: ${JSON.stringify(sample)}`,
    ).toBeGreaterThan(baseline.darkBackgroundPixels * 0.8);
    expect(
      sample.coverage.checkerboardPixels,
      `drag sample ${index + 1} flashed the document checkerboard: ${JSON.stringify(sample)}`,
    ).toBeLessThan(sample.coverage.samples * 0.1);
    expect(sample.yellow, `drag epoch ${sample.frame} lost the selected Splash letter`).not
      .toBeNull();
    expect(sample.teal, `drag epoch ${sample.frame} lost the selection outline`).not.toBeNull();
  }
  const alignmentErrors = samples
    .filter((sample) => sample.yellow !== null && sample.teal !== null)
    .map((sample) => ({
      error: Math.abs(sample.yellow!.minX - sample.teal!.minX - baselineAlignment.x),
      frame: sample.frame,
      tealMinX: sample.teal!.minX,
      yellowMinX: sample.yellow!.minX,
    }));
  expect(
    Math.max(...alignmentErrors.map((sample) => sample.error)),
    `accepted Splash epochs diverged from their outlines: ${JSON.stringify(alignmentErrors)}`,
  ).toBeLessThanOrEqual(2);
  const yellowCenters = samples
    .map((sample) => sample.yellow)
    .filter((bounds): bounds is PixelBounds => bounds !== null)
    .map(centerX);
  expect(yellowCenters).toEqual([...yellowCenters].sort((a, b) => b - a));
  expect(failures).toEqual([]);
});

test("Firefox bakes the Splash letter and overlay into one accepted surface epoch", async ({
  browserName,
  page,
}, testInfo) => {
  test.skip(browserName !== "firefox", "Firefox direct-surface regression");
  const failures = await openEditor(page);
  const editorCanvas = page.locator("canvas#canvas");
  const editorBounds = await editorCanvas.boundingBox();
  expect(editorBounds).not.toBeNull();
  if (editorBounds === null) {
    return;
  }

  const beforeSampleFrame = await page.evaluate(
    () => window.__donnerAcceptedPresentation?.token || 0,
  );
  await page.mouse.click(editorBounds.x + editorBounds.width * 0.24, editorBounds.y + 282);
  await expect(editorCanvas).toHaveAttribute("data-active-sample-id", "donner-splash");
  await expect
    .poll(() => page.evaluate(() => window.__donnerAcceptedPresentation?.token || 0), {
      message: "expected a newly accepted Splash surface",
      timeout: 5_000,
      intervals: [16, 25, 50, 100],
    })
    .toBeGreaterThan(beforeSampleFrame);
  await waitForBrowserComposite(page);

  const visibleSurface = page.locator("canvas[data-direct-surface-visible=\"true\"]");
  await expect(visibleSurface).toBeVisible();
  const documentBounds = await visibleSurface.boundingBox();
  expect(documentBounds).not.toBeNull();
  if (documentBounds === null) {
    return;
  }
  const documentRegion = {
    x: Math.max(editorBounds.x, documentBounds.x) + 2,
    y: Math.max(editorBounds.y, documentBounds.y) + 2,
    width: Math.min(
      editorBounds.x + editorBounds.width,
      documentBounds.x + documentBounds.width,
    ) - Math.max(editorBounds.x, documentBounds.x) - 4,
    height: Math.min(
      editorBounds.y + editorBounds.height,
      documentBounds.y + documentBounds.height,
    ) - Math.max(editorBounds.y, documentBounds.y) - 4,
  };
  const letterSearchBounds = {
    minX: 20,
    minY: 260,
    maxX: 335,
    maxY: Math.min(430, documentRegion.height - 1),
  };
  let unselectedBaseline = await readSplashCompositeFrameStats(
    page, documentRegion, letterSearchBounds
  );
  await expect.poll(async () => {
    unselectedBaseline = await readSplashCompositeFrameStats(
      page, documentRegion, letterSearchBounds
    );
    return unselectedBaseline.yellow?.pixels || 0;
  }, {
    message: "the rendered Splash D must become visible before the immediate drag",
    timeout: 5_000,
    intervals: [0, 8, 16],
  }).toBeGreaterThan(20);
  expect(unselectedBaseline.yellow, "the rendered Splash D was not present before selection").not
    .toBeNull();
  if (unselectedBaseline.yellow === null) {
    return;
  }
  const dragStart = {
    x: documentRegion.x + unselectedBaseline.yellow.minX + 10,
    y: documentRegion.y + unselectedBaseline.yellow.maxY - 3,
  };
  const frames: Array<{
    checkerboardPixels: number;
    darkBackgroundPixels: number;
    historicalPositionYellowPixels: number;
    localYellow: PixelBounds | null;
    png: Buffer;
    step: number;
    teal: PixelBounds | null;
    token: number;
    yellow: PixelBounds | null;
  }> = [];
  const historicalPositionBounds = {
    minX: unselectedBaseline.yellow.minX - 2,
    minY: unselectedBaseline.yellow.minY - 2,
    maxX: unselectedBaseline.yellow.maxX + 2,
    maxY: unselectedBaseline.yellow.maxY + 2,
  };
  const tokenBeforeDrag = await page.evaluate(
    () => window.__donnerAcceptedPresentation?.token || 0,
  );
  await page.mouse.move(dragStart.x, dragStart.y);
  await page.mouse.down();
  await page.mouse.move(dragStart.x - 90, dragStart.y - 40.5, { steps: 12 });
  await expect
    .poll(() => page.evaluate(() => window.__donnerAcceptedPresentation?.token || 0), {
      message: "expected a drag epoch after moving the Splash D",
      timeout: 5_000,
      intervals: [16, 25, 50, 100],
    })
    .toBeGreaterThan(tokenBeforeDrag);
  await waitForBrowserComposite(page);
  await expect(visibleSurface).toHaveAttribute(
    "data-direct-surface-selection-chrome-baked",
    "true",
  );
  expect(
    await page.evaluate(() => window.__donnerAcceptedPresentation?.selectionChromeBaked),
  ).toBe(true);

  // Hide the main ImGui canvas for one settled capture. The selected D and its
  // teal path/bounds must both remain because they are pixels in the same
  // accepted worker texture. Repeated compositor screenshots can itself
  // invalidate Firefox WebGPU surfaces, so this test takes one architectural
  // co-location sample instead of racing screenshots against pointer input.
  await page.evaluate(() => {
    const canvas = document.getElementById("canvas");
    if (canvas instanceof HTMLCanvasElement) {
      canvas.style.visibility = "hidden";
    }
  });
  await waitForBrowserComposite(page);
  const capture = await captureSplashCompositeFrame(page, documentRegion, letterSearchBounds);
  await page.evaluate(() => {
    const canvas = document.getElementById("canvas");
    if (canvas instanceof HTMLCanvasElement) {
      canvas.style.visibility = "visible";
    }
  });
  const frame = capture.stats;
  const localYellow = frame.teal === null
    ? null
    : readEditorPixelBoundsFromPng(
      capture.png,
      "splash-yellow",
      documentRegion,
      {
        minX: frame.teal.minX - 8,
        minY: letterSearchBounds.minY,
        maxX: frame.teal.maxX + 8,
        maxY: letterSearchBounds.maxY,
      },
    );
  frames.push({
    checkerboardPixels: frame.checkerboardPixels,
    darkBackgroundPixels: frame.darkBackgroundPixels,
    historicalPositionYellowPixels: readEditorPixelBoundsFromPng(
      capture.png,
      "splash-yellow",
      documentRegion,
      historicalPositionBounds,
    )?.pixels || 0,
    localYellow,
    png: capture.png,
    step: 1,
    teal: frame.teal,
    token: await page.evaluate(() => window.__donnerAcceptedPresentation?.token || 0),
    yellow: frame.yellow,
  });
  await page.mouse.up();
  await waitForBrowserComposite(page);

  const settledCapture = await captureSplashCompositeFrame(
    page, documentRegion, letterSearchBounds
  );
  const settled = settledCapture.stats;
  expect(settled.yellow).not.toBeNull();
  expect(settled.teal).not.toBeNull();
  if (settled.yellow === null || settled.teal === null) {
    return;
  }
  const settledAlignment = settled.yellow.minX - settled.teal.minX;
  const settledHistoricalPositionPixels = readEditorPixelBoundsFromPng(
    settledCapture.png,
    "splash-yellow",
    documentRegion,
    historicalPositionBounds,
  )?.pixels || 0;

  const lostFrames = frames.filter((frame) => frame.yellow === null);
  const alignmentErrors = frames.map((frame) => frame.localYellow !== null && frame.teal !== null
    ? Math.abs(frame.localYellow.minX - frame.teal.minX - settledAlignment)
    : null);
  const historicalFrames = frames.filter(
    (_frame, index) => alignmentErrors[index] !== null && alignmentErrors[index]! > 2,
  );
  const ghostFrames = frames.filter((frame) =>
    frame.localYellow !== null
    && frame.localYellow.maxX < historicalPositionBounds.minX - 2
    && frame.historicalPositionYellowPixels > settledHistoricalPositionPixels + 128
  );
  const draggedFrames = frames.filter((frame) => frame.localYellow !== null);
  const reversedFrames = draggedFrames.filter((frame, index) =>
    index > 0 && frame.localYellow !== null && draggedFrames[index - 1].localYellow !== null
      && frame.localYellow.minX > draggedFrames[index - 1].localYellow.minX + 0.5
  );
  const diagnosticFrames = [
    ...lostFrames,
    ...historicalFrames,
    ...ghostFrames,
    ...reversedFrames,
  ].filter((frame, index, all) =>
    all.findIndex((candidate) => candidate.step === frame.step) === index
  ).slice(0, 6);
  for (const frame of diagnosticFrames) {
    await testInfo.attach(`composited-drag-step-${frame.step}`, {
      body: frame.png,
      contentType: "image/png",
    });
  }
  const summarize = (frame: typeof frames[number]) => ({
    checkerboardPixels: frame.checkerboardPixels,
    darkBackgroundPixels: frame.darkBackgroundPixels,
    historicalPositionYellowPixels: frame.historicalPositionYellowPixels,
    localYellow: frame.localYellow,
    step: frame.step,
    teal: frame.teal,
    token: frame.token,
    yellow: frame.yellow,
  });
  expect.soft(
    lostFrames.map(summarize),
    `a composited drag frame lost the Splash letter or its overlay: ${JSON.stringify(lostFrames.map(summarize))}`,
  ).toEqual([]);
  expect.soft(
    Math.max(...alignmentErrors.filter((error): error is number => error !== null), 0),
    `a browser-composited frame showed historical letter pixels (${historicalFrames.length} frames): ${JSON.stringify(historicalFrames.slice(0, 8).map(summarize))}`,
  ).toBeLessThanOrEqual(2);
  expect.soft(
    ghostFrames.map(summarize),
    `a browser-composited frame retained the D at a historical position: ${JSON.stringify(ghostFrames.map(summarize))}`,
  ).toEqual([]);
  expect.soft(
    reversedFrames.map(summarize),
    `the Splash letter moved back to a historical drag position: ${JSON.stringify(reversedFrames.map(summarize))}`,
  ).toEqual([]);
  expect(failures).toEqual([]);
});

test("WebKit Geode survives a burst of drag wakeups without fatal errors", async ({ browserName, page }) => {
  test.skip(browserName !== "webkit", "WebKit Geode regression");
  const failures = await openEditor(page);
  expect(await page.evaluate(() => window.__donnerBackend)).toBe("geode");

  const editorCanvas = page.locator("canvas#canvas");
  const editorBounds = await editorCanvas.boundingBox();
  expect(editorBounds).not.toBeNull();
  if (editorBounds === null) {
    return;
  }

  const beforeSample = await page.evaluate(() => window.__donnerWorkerStats?.completedResults || 0);
  await page.mouse.click(editorBounds.x + editorBounds.width * 0.5, editorBounds.y + 282);
  await expect(editorCanvas).toHaveAttribute("data-active-sample-id", "basic-shapes");
  await expect
    .poll(() => page.evaluate(() => window.__donnerWorkerStats?.completedResults || 0), {
      message: "expected Basic Shapes to finish presenting before the WebKit drag burst",
      timeout: 2_000,
      intervals: [16, 25, 50, 100],
    })
    .toBeGreaterThan(beforeSample);

  const dragStart = { x: editorBounds.x + 408, y: editorBounds.y + 353 };
  const beforeDrag = await page.evaluate(() => window.__donnerWorkerStats?.completedResults || 0);
  await page.mouse.move(dragStart.x, dragStart.y);
  await page.mouse.down();
  for (let step = 1; step <= 48; ++step) {
    await page.mouse.move(dragStart.x + step * 2, dragStart.y + step);
    if (step % 8 === 0) {
      await page.evaluate(() => new Promise((resolve) => requestAnimationFrame(resolve)));
    }
  }
  await page.mouse.up();
  await expect
    .poll(() => page.evaluate(() => window.__donnerWorkerStats?.completedResults || 0), {
      message: "expected WebKit to complete a render after the drag wakeup burst",
      timeout: 2_000,
      intervals: [16, 25, 50, 100],
    })
    .toBeGreaterThan(beforeDrag);
  expect(failures).toEqual([]);
});

interface SurfaceLayoutSample {
  frame: number;
  height: number;
  left: number;
  top: number;
  visible: boolean;
  width: number;
}

declare global {
  interface Window {
    __donnerSurfaceLayoutSamples?: SurfaceLayoutSample[];
    __donnerSurfaceLayoutObserver?: MutationObserver;
    __donnerStopSurfaceLayoutSampling?: () => void;
  }
}

/**
 * Record every worker-surface layout write the editor performs.
 *
 * The presenter rewrites the surface's CSS geometry once per UI frame, so a
 * mutation observer sees every intermediate placement. Screenshot sampling
 * cannot: a single capture costs several frames, which is long enough for the
 * worker to land the epoch that hides the defect.
 */
async function recordSurfaceLayouts(page: Page): Promise<void> {
  await page.evaluate(() => {
    const surfaces = ["donner-document-canvas", "donner-document-canvas-back"];
    const samples: SurfaceLayoutSample[] = [];
    // One sample per capture describing what the pane actually shows: the editor
    // keeps a front/back surface pair and only ever marks one of them visible,
    // so the inactive slot's permanent `visible=false` is not a blank frame.
    const capture = () => {
      const shown = surfaces
        .map((id) => document.getElementById(id) as HTMLCanvasElement | null)
        .find((surface) => surface?.dataset.directSurfaceVisible === "true") || null;
      const rect = shown?.getBoundingClientRect();
      const sample: SurfaceLayoutSample = {
        frame: Number(shown?.dataset.directSurfaceFrame || 0),
        height: rect?.height || 0,
        left: rect?.x || 0,
        top: rect?.y || 0,
        visible: shown !== null,
        width: rect?.width || 0,
      };
      const previous = samples.at(-1);
      if (previous === undefined || JSON.stringify(previous) !== JSON.stringify(sample)) {
        samples.push(sample);
      }
      window.__donnerSurfaceLayoutSamples = samples;
    };
    const observer = new MutationObserver(capture);
    for (const id of surfaces) {
      const surface = document.getElementById(id);
      if (surface !== null) {
        observer.observe(surface, { attributes: true });
      }
    }
    // Also sample once per animation frame. Several editor frames can land
    // inside one microtask checkpoint, and a mutation callback only ever sees
    // the DOM's final state for that checkpoint; the per-frame sampler is what
    // makes a one-frame placement defect observable.
    let sampling = true;
    const sampleFrame = () => {
      if (!sampling) {
        return;
      }
      capture();
      requestAnimationFrame(sampleFrame);
    };
    requestAnimationFrame(sampleFrame);
    window.__donnerStopSurfaceLayoutSampling = () => {
      sampling = false;
    };
    window.__donnerSurfaceLayoutObserver = observer;
    window.__donnerSurfaceLayoutSamples = samples;
    capture();
  });
}

async function readSurfaceLayouts(page: Page): Promise<SurfaceLayoutSample[]> {
  return page.evaluate(() => {
    window.__donnerStopSurfaceLayoutSampling?.();
    window.__donnerSurfaceLayoutObserver?.disconnect();
    return window.__donnerSurfaceLayoutSamples || [];
  });
}

/**
 * Dispatch `count` ctrl+wheel pinch-zoom notches, as Chromium delivers trackpad
 * pinch. All notches go out in one task so the editor sees the whole zoom delta
 * before it can render, which is exactly the trackpad case: the viewport runs
 * ahead of the worker's accepted epoch.
 */
async function pinchZoom(
  page: Page,
  at: { x: number; y: number },
  deltaY: number,
  count = 1,
): Promise<void> {
  await page.evaluate(({ x, y, deltaY, count }) => {
    const target = document.getElementById("canvas");
    if (target === null) {
      throw new Error("canvas not found");
    }
    for (let notch = 0; notch < count; ++notch) {
      target.dispatchEvent(
        new WheelEvent("wheel", {
          bubbles: true,
          cancelable: true,
          clientX: x,
          clientY: y,
          ctrlKey: true,
          deltaMode: WheelEvent.DOM_DELTA_PIXEL,
          deltaY,
        }),
      );
    }
  }, { ...at, deltaY, count });
}

function coversRegion(sample: SurfaceLayoutSample, region: CssRegion): boolean {
  return sample.visible
    && sample.left <= region.x + 0.5
    && sample.top <= region.y + 0.5
    && sample.left + sample.width >= region.x + region.width - 0.5
    && sample.top + sample.height >= region.y + region.height - 0.5;
}

async function openDonnerSplash(page: Page): Promise<{
  editorBounds: { x: number; y: number; width: number; height: number };
}> {
  const editorCanvas = page.locator("canvas#canvas");
  const editorBounds = await editorCanvas.boundingBox();
  expect(editorBounds).not.toBeNull();
  if (editorBounds === null) {
    throw new Error("editor canvas is missing");
  }
  await page.mouse.click(editorBounds.x + editorBounds.width * 0.24, editorBounds.y + 282);
  await expect(editorCanvas).toHaveAttribute("data-active-sample-id", "donner-splash");
  await expect(page.locator("canvas[data-direct-surface-visible=\"true\"]")).toBeVisible();
  await expect
    .poll(() => page.evaluate(() => window.__donnerAcceptedPresentation?.token || 0), {
      message: "Donner Splash must publish an accepted worker presentation epoch",
      timeout: 5_000,
      intervals: [16, 25, 50, 100],
    })
    .toBeGreaterThan(0);
  return { editorBounds };
}

test("a zoom storm never uncovers the editor background under the Donner Splash", async ({ page }) => {
  const failures = await openEditor(page);
  const { editorBounds } = await openDonnerSplash(page);

  // A rectangle strictly inside the render pane. Once the document is zoomed in
  // far enough to cover it, every frame that fails to cover it is showing the
  // editor background where document pixels belong.
  const probeRegion = {
    x: editorBounds.x + 320,
    y: editorBounds.y + 200,
    width: 480,
    height: 420,
  };
  const probeCenter = {
    x: probeRegion.x + probeRegion.width * 0.5,
    y: probeRegion.y + probeRegion.height * 0.5,
  };

  // Zoom in one settled notch at a time until the surface covers the probe.
  const surfaceCoversProbe = async () => {
    const box = await page.locator("canvas[data-direct-surface-visible=\"true\"]").boundingBox();
    return box !== null
      && coversRegion(
        { frame: 0, height: box.height, left: box.x, top: box.y, visible: true, width: box.width },
        probeRegion,
      );
  };
  // The render pane classifies wheel input only while it is the hovered window.
  await page.mouse.move(probeCenter.x, probeCenter.y);
  const zoomInBoxes: Array<{ height: number; width: number } | null> = [];
  for (let notch = 0; notch < 10 && !(await surfaceCoversProbe()); ++notch) {
    await pinchZoom(page, probeCenter, -250);
    await page.waitForTimeout(200);
    const box = await page.locator("canvas[data-direct-surface-visible=\"true\"]").boundingBox();
    zoomInBoxes.push(box === null ? null : { height: box.height, width: box.width });
  }
  expect(
    await surfaceCoversProbe(),
    `zooming in never produced a document surface covering the probe region;`
      + ` boxes=${JSON.stringify(zoomInBoxes)}`,
  ).toBe(true);
  expect(
    zoomInBoxes.length,
    `the document already covered the probe region without zooming: ${JSON.stringify(zoomInBoxes)}`,
  ).toBeGreaterThan(0);

  await recordSurfaceLayouts(page);

  // Zoom out and back in. Each notch changes the live viewport by ~27% while the
  // worker's accepted epoch is still the previous raster, so placing that epoch
  // through the live viewport shrinks the surface inside the pane and uncovers
  // the editor background until the worker catches up.
  //
  // The assertion is on the surface's CSS geometry rather than on screenshots:
  // headless Chromium does not capture the worker-owned WebGPU canvas in
  // `page.screenshot()` (every pixel of it reads back as the page background),
  // so a pixel probe cannot tell "document missing" from "document present"
  // here. Geometry is the same defect one step earlier, and the per-frame
  // sampler sees every intermediate placement.
  for (let burst = 0; burst < 3; ++burst) {
    await pinchZoom(page, probeCenter, 250);
    await page.waitForTimeout(200);
    await pinchZoom(page, probeCenter, -250);
    await page.waitForTimeout(200);
  }

  const samples = await readSurfaceLayouts(page);
  expect(samples.length, "no worker surface layout writes were observed").toBeGreaterThan(0);
  // Guard against an inconclusive run: synthesized wheel notches are not always
  // accepted, and a storm the editor ignored proves nothing.
  const presentedWidths = new Set(
    samples.filter((sample) => sample.visible).map((sample) => Math.round(sample.width)),
  );
  expect(
    presentedWidths.size,
    `the zoom storm never changed the presented document scale: ${JSON.stringify(samples)}`,
  ).toBeGreaterThan(1);
  const uncovered = samples.filter((sample) => !coversRegion(sample, probeRegion));
  expect(
    uncovered,
    `zoom storm exposed the editor background on ${uncovered.length}/${samples.length} surface`
      + ` layouts; probe=${JSON.stringify(probeRegion)} first=${JSON.stringify(uncovered.slice(0, 6))}`,
  ).toEqual([]);
  console.log(`zoom-storm zoomIn=${JSON.stringify(zoomInBoxes)} layouts=${samples.length}`);
  expect(failures).toEqual([]);
});

test("loading a document never leaves the render pane without a presenter", async ({ page }) => {
  const failures = await openEditor(page);
  const editorCanvas = page.locator("canvas#canvas");
  const editorBounds = await editorCanvas.boundingBox();
  expect(editorBounds).not.toBeNull();
  if (editorBounds === null) {
    return;
  }

  const acceptedFrame = () =>
    page.evaluate(() =>
      Math.max(
        ...["donner-document-canvas", "donner-document-canvas-back"].map((id) =>
          Number(document.getElementById(id)?.getAttribute("data-direct-surface-frame") || 0)
        ),
      )
    );
  const beforeFrame = await acceptedFrame();
  await recordSurfaceLayouts(page);

  await page.mouse.click(editorBounds.x + editorBounds.width * 0.24, editorBounds.y + 282);
  await expect(editorCanvas).toHaveAttribute("data-active-sample-id", "donner-splash");
  await expect
    .poll(acceptedFrame, {
      message: "expected Donner Splash to present an accepted document surface",
      timeout: 5_000,
      intervals: [8, 16, 25, 50],
    })
    .toBeGreaterThan(beforeFrame);
  // Let the picker close and steady state settle, so any post-load blank frame
  // is inside the sampled window.
  await page.waitForTimeout(500);

  const samples = await readSurfaceLayouts(page);
  // Everything up to and including the first visible placement is the sample
  // picker owning the pane, which suppresses document presentation by design.
  // From there on the pane belongs to the document and a hidden surface is a
  // blank frame.
  const firstVisible = samples.findIndex((sample) => sample.visible);
  expect(firstVisible, "the document surface never became visible").toBeGreaterThanOrEqual(0);
  const blank = samples.slice(firstVisible).filter((sample) => !sample.visible);
  expect(
    blank,
    `the document surface went blank after presenting: ${JSON.stringify(samples)}`,
  ).toEqual([]);
  expect(failures).toEqual([]);
});
