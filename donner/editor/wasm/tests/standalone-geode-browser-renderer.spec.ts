import { expect, type Page, test } from "@playwright/test";
import { writeFileSync } from "node:fs";
import { join } from "node:path";
import { PNG } from "pngjs";
import { type BitmapComparatorResult, runBitmapComparator } from "./browser-bitmap-comparator";
import { installBrowserStallDiagnostics } from "./browser-stall-diagnostics";

installBrowserStallDiagnostics(test);

const baseUrl = process.env.DONNER_WASM_BASE_URL || "http://127.0.0.1:8000";
const selectedBrowserBackend = "[Geode] GPU backend: browser, selected by the build setting";

const gpuCounterNames = [
  "deviceRequests",
  "queueSubmits",
  "queueSubmitFailures",
  "queueWaits",
  "queueDone",
  "queueRejected",
  "mapRequests",
  "mapResolved",
  "mapRejected",
  "mapPending",
  "observedAtMs",
  "lastQueueSubmitAtMs",
  "lastQueueDoneAtMs",
  "lastMapRequestAtMs",
  "lastMapResolvedAtMs",
  "lastMapRejectedAtMs",
  "gpuValidationErrors",
  "gpuOutOfMemoryErrors",
  "gpuInternalErrors",
  "gpuOtherErrors",
  "diagnosticObserverErrors",
  "deviceLostDestroyed",
  "deviceLostOther",
] as const;

type GpuCounters = Record<(typeof gpuCounterNames)[number], number>;
type GpuDiagnostics = {
  current: GpuCounters;
  atDeadline: GpuCounters | null;
  attachDeviceForTesting: (device: GPUDevice) => void;
};

function formatGpuCounters(value: unknown): string {
  const counters = value !== null && typeof value === "object"
    ? value as Record<string, unknown>
    : {};
  return gpuCounterNames.map((name) => {
    const count = counters[name];
    return `${name}=${
      typeof count === "number" && Number.isSafeInteger(count) && count >= 0
        ? count
        : "unknown"
    }`;
  }).join(",");
}

async function readGpuDiagnosticsBounded(
  page: Page,
): Promise<Pick<GpuDiagnostics, "current" | "atDeadline"> | null> {
  let timer: ReturnType<typeof setTimeout> | undefined;
  try {
    return await Promise.race([
      page.evaluate(() => {
        const { current, atDeadline } =
          (window as Window & { __donnerGpuDiagnostics: GpuDiagnostics }).__donnerGpuDiagnostics;
        return { current, atDeadline };
      }).catch(() => null),
      new Promise<null>((resolve) => {
        timer = setTimeout(() => resolve(null), 500);
      }),
    ]);
  } catch {
    return null;
  } finally {
    if (timer !== undefined) clearTimeout(timer);
  }
}

function installGpuDiagnostics(): void {
  const counters = {
    deviceRequests: 0,
    queueSubmits: 0,
    queueSubmitFailures: 0,
    queueWaits: 0,
    queueDone: 0,
    queueRejected: 0,
    mapRequests: 0,
    mapResolved: 0,
    mapRejected: 0,
    lastQueueSubmitAtMs: 0,
    lastQueueDoneAtMs: 0,
    lastMapRequestAtMs: 0,
    lastMapResolvedAtMs: 0,
    lastMapRejectedAtMs: 0,
    gpuValidationErrors: 0,
    gpuOutOfMemoryErrors: 0,
    gpuInternalErrors: 0,
    gpuOtherErrors: 0,
    diagnosticObserverErrors: 0,
    deviceLostDestroyed: 0,
    deviceLostOther: 0,
  };
  const snapshot = (): GpuCounters => ({
    ...counters,
    observedAtMs: Math.round(performance.now()),
    mapPending: counters.mapRequests - counters.mapResolved - counters.mapRejected,
  });
  let atDeadline: GpuCounters | null = null;

  const attachDevice = (device: GPUDevice): void => {
    device.addEventListener("uncapturederror", (event) => {
      const error = (event as Event & { error?: unknown }).error;
      const isGpuError = (constructorName: string): boolean => {
        const constructor = (globalThis as unknown as Record<string, unknown>)[constructorName];
        return typeof constructor === "function" && error instanceof constructor;
      };
      if (isGpuError("GPUValidationError")) counters.gpuValidationErrors += 1;
      else if (isGpuError("GPUOutOfMemoryError")) counters.gpuOutOfMemoryErrors += 1;
      else if (isGpuError("GPUInternalError")) counters.gpuInternalErrors += 1;
      else counters.gpuOtherErrors += 1;
    });
    void device.lost.then(
      (info) => {
        if (info.reason === "destroyed") counters.deviceLostDestroyed += 1;
        else counters.deviceLostOther += 1;
      },
      () => {
        counters.deviceLostOther += 1;
      },
    );
  };

  const originalRequestDevice = GPUAdapter.prototype.requestDevice;
  GPUAdapter.prototype.requestDevice = function(...args) {
    counters.deviceRequests += 1;
    const requested = originalRequestDevice.apply(this, args);
    void requested.then(
      (device) => {
        try {
          attachDevice(device);
        } catch {
          counters.diagnosticObserverErrors += 1;
        }
      },
      () => {},
    );
    return requested;
  };

  const originalSubmit = GPUQueue.prototype.submit;
  GPUQueue.prototype.submit = function(...args) {
    counters.queueSubmits += 1;
    counters.lastQueueSubmitAtMs = Math.round(performance.now());
    try {
      return originalSubmit.apply(this, args);
    } catch (error) {
      counters.queueSubmitFailures += 1;
      throw error;
    }
  };
  const originalOnSubmittedWorkDone = GPUQueue.prototype.onSubmittedWorkDone;
  GPUQueue.prototype.onSubmittedWorkDone = function(...args) {
    counters.queueWaits += 1;
    const completed = originalOnSubmittedWorkDone.apply(this, args);
    void completed.then(
      () => {
        counters.queueDone += 1;
        counters.lastQueueDoneAtMs = Math.round(performance.now());
      },
      () => {
        counters.queueRejected += 1;
      },
    );
    return completed;
  };

  const originalMapAsync = GPUBuffer.prototype.mapAsync;
  GPUBuffer.prototype.mapAsync = function(...args) {
    counters.mapRequests += 1;
    counters.lastMapRequestAtMs = Math.round(performance.now());
    const mapped = originalMapAsync.apply(this, args);
    void mapped.then(
      () => {
        counters.mapResolved += 1;
        counters.lastMapResolvedAtMs = Math.round(performance.now());
      },
      () => {
        counters.mapRejected += 1;
        counters.lastMapRejectedAtMs = Math.round(performance.now());
      },
    );
    return mapped;
  };

  const captureDeadline = (args: unknown[]): void => {
    if (
      typeof args[0] === "string"
      && args[0].startsWith("[Geode] snapshot map reached the capture deadline")
    ) {
      atDeadline = snapshot();
    }
  };
  const originalError = console.error;
  console.error = (...args) => {
    captureDeadline(args);
    originalError.apply(console, args);
  };
  const originalLog = console.log;
  console.log = (...args) => {
    captureDeadline(args);
    originalLog.apply(console, args);
  };
  const originalWarn = console.warn;
  console.warn = (...args) => {
    captureDeadline(args);
    originalWarn.apply(console, args);
  };

  Object.defineProperty(window, "__donnerGpuDiagnostics", {
    get: (): GpuDiagnostics => ({
      current: snapshot(),
      atDeadline,
      attachDeviceForTesting: attachDevice,
    }),
  });
}

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

type RgbaCapture = { width: number; height: number; rgba: string };

/** Write base64 RGBA bytes read from the page as a PNG for the native comparator. */
function writeRgbaPng(path: string, capture: RgbaCapture): void {
  const rgba = Buffer.from(capture.rgba, "base64");
  expect(rgba.length, `${path} must hold one RGBA value per pixel`).toBe(
    capture.width * capture.height * 4,
  );
  const image = new PNG({ width: capture.width, height: capture.height });
  rgba.copy(image.data);
  writeFileSync(path, PNG.sync.write(image));
}

/**
 * Run one comparison through the shared pixelmatch comparator. A mismatch fails the test with the
 * comparison's description and lets the test go on to its other comparisons; the comparator
 * leaves its actual, expected and diff PNGs with the test outputs. A comparator that cannot run
 * or complete its case stops the test with the description and the comparator's output.
 */
function expectBitmapsMatch(
  description: string,
  comparator: string,
  reportPath: string,
  comparison: Record<string, string>,
): void {
  let result: BitmapComparatorResult;
  try {
    result = runBitmapComparator({ executable: comparator, comparison, reportPath });
  } catch (error) {
    throw new Error(`${description}: ${error instanceof Error ? error.message : String(error)}`);
  }
  console.log(result.detail);
  expect.soft(result.matched, `${description}; the comparator output above names its diff PNGs`)
    .toBe(true);
}

function pageErrorCategory(error: Error): string {
  const message = error.message;
  if (/device.{0,24}lost|lost.{0,24}device/i.test(message)) {
    return "WebGPU device lost";
  }
  if (error.name === "GPUValidationError" || /GPUValidationError|validation error/i.test(message)) {
    return "WebGPU validation error";
  }
  if (error.name === "GPUOutOfMemoryError" || /GPUOutOfMemoryError|out of memory/i.test(message)) {
    return "WebGPU out of memory";
  }
  if (/Asyncify/i.test(message)) {
    return "Wasm Asyncify error";
  }
  if (error.name === "RuntimeError") {
    return "Wasm runtime error";
  }
  if (error.name === "TypeError") {
    return "JavaScript type error";
  }
  if (error.name === "RangeError") {
    return "JavaScript range error";
  }
  if (error.name === "ReferenceError") {
    return "JavaScript reference error";
  }
  return "Unclassified browser page error";
}

test("browser diagnostics cannot disclose console content", async () => {
  const sensitive = "https://private.example.test/path /Users/operator/private/file";
  expect(browserDiagnosticStage(sensitive)).toBeNull();
  expect(browserDiagnosticStage(`[wasm] parsed OK ${sensitive}`)).toBe("svg_parsed");
  expect(pageErrorCategory(new Error(`Asyncify failed at ${sensitive}`))).toBe(
    "Wasm Asyncify error",
  );
  expect(pageErrorCategory(new Error(`device was lost at ${sensitive}`))).toBe(
    "WebGPU device lost",
  );
  const validationError = new Error(sensitive);
  validationError.name = "GPUValidationError";
  expect(pageErrorCategory(validationError)).toBe("WebGPU validation error");
  const memoryError = new Error(sensitive);
  memoryError.name = "GPUOutOfMemoryError";
  expect(pageErrorCategory(memoryError)).toBe("WebGPU out of memory");
  expect(pageErrorCategory(new Error(sensitive))).toBe("Unclassified browser page error");
  expect(formatGpuCounters({ mapRequests: sensitive, extra: sensitive })).not.toContain(sensitive);
  const stalledPage = { evaluate: () => new Promise<never>(() => {}) } as unknown as Page;
  expect(await readGpuDiagnosticsBounded(stalledPage)).toBeNull();
});

test("browser GPU diagnostics whitelist errors and snapshot the map deadline", async ({ page }) => {
  const fixtureUrl = `${baseUrl}/__gpu_diagnostic_blank`;
  await page.route(fixtureUrl, (route) =>
    route.fulfill({
      status: 200,
      contentType: "text/html",
      body: "<!doctype html><title>GPU diagnostic fixture</title>",
    }));
  await page.goto(fixtureUrl);
  await page.evaluate(() => {
    let rejectMap: () => void = () => {};
    const pending = new Promise<undefined>((_, reject) => {
      rejectMap = () => reject(new Error("https://private.example.test/path"));
    });
    GPUBuffer.prototype.mapAsync = function() {
      return pending;
    };
    Object.defineProperty(window, "__rejectSyntheticMap", { value: rejectMap });
  });
  await page.evaluate(installGpuDiagnostics);
  const diagnostics = await page.evaluate(async () => {
    const browserWindow = window as Window & { __donnerGpuDiagnostics: GpuDiagnostics };
    const sensitive = "https://private.example.test/path";
    const pendingMap = GPUBuffer.prototype.mapAsync.call({} as GPUBuffer, 1);
    void pendingMap.catch(() => {});
    const fakeDevice = new EventTarget() as unknown as GPUDevice;
    Object.defineProperty(fakeDevice, "lost", { value: Promise.resolve({ reason: "destroyed" }) });
    browserWindow.__donnerGpuDiagnostics.attachDeviceForTesting(fakeDevice);
    const unknownLoss = new EventTarget() as unknown as GPUDevice;
    Object.defineProperty(unknownLoss, "lost", { value: Promise.resolve({ reason: "unknown" }) });
    browserWindow.__donnerGpuDiagnostics.attachDeviceForTesting(unknownLoss);
    const errorObjects: unknown[] = [
      new GPUValidationError(sensitive),
      new GPUOutOfMemoryError(sensitive),
    ];
    const internalConstructor = (globalThis as unknown as Record<string, unknown>).GPUInternalError;
    const hasInternal = typeof internalConstructor === "function";
    if (hasInternal) {
      errorObjects.push(new (internalConstructor as new(message: string) => object)(sensitive));
    }
    errorObjects.push({ name: "GPUValidationError", message: sensitive });
    for (const errorObject of errorObjects) {
      const event = new Event("uncapturederror");
      Object.defineProperty(event, "error", {
        value: errorObject,
      });
      fakeDevice.dispatchEvent(event);
    }
    await Promise.resolve();
    console.error("[Geode] snapshot map reached the capture deadline");
    (window as Window & { __rejectSyntheticMap: () => void }).__rejectSyntheticMap();
    await Promise.resolve();
    await Promise.resolve();
    const { current, atDeadline } = browserWindow.__donnerGpuDiagnostics;
    return { current, atDeadline, hasInternal };
  });
  expect(diagnostics.current.gpuValidationErrors).toBe(1);
  expect(diagnostics.current.gpuOutOfMemoryErrors).toBe(1);
  expect(diagnostics.current.gpuInternalErrors).toBe(diagnostics.hasInternal ? 1 : 0);
  expect(diagnostics.current.gpuOtherErrors).toBe(1);
  expect(diagnostics.current.deviceLostDestroyed).toBe(1);
  expect(diagnostics.current.deviceLostOther).toBe(1);
  expect(diagnostics.atDeadline?.mapRequests).toBe(1);
  expect(diagnostics.atDeadline?.mapPending).toBe(1);
  expect(diagnostics.atDeadline?.mapRejected).toBe(0);
  expect(diagnostics.current.mapPending).toBe(0);
  expect(diagnostics.current.mapRejected).toBe(1);
  expect(diagnostics.current.lastMapRejectedAtMs).toBeGreaterThanOrEqual(
    diagnostics.atDeadline?.observedAtMs ?? 0,
  );
  expect(diagnostics.atDeadline?.gpuValidationErrors).toBe(1);
  expect(diagnostics.atDeadline?.deviceLostDestroyed).toBe(1);
  expect(diagnostics.atDeadline?.observedAtMs).toBeLessThanOrEqual(
    diagnostics.current.observedAtMs,
  );
  expect(formatGpuCounters(diagnostics.atDeadline)).not.toContain("private.example.test");
});

test("standalone Geode wasm selects the browser backend and paints SVG pixels", async ({ page }) => {
  test.setTimeout(90000);
  const browserStages = new Set<string>();
  let browserBackendSelected = false;
  const pageErrorCategories: string[] = [];
  let pageErrorCount = 0;
  page.on("console", (message) => {
    const line = message.text();
    browserBackendSelected ||= line.includes(selectedBrowserBackend);
    const stage = browserDiagnosticStage(line);
    if (stage !== null) {
      browserStages.add(stage);
    }
  });
  page.on("pageerror", (error) => {
    pageErrorCount += 1;
    const category = pageErrorCategory(error);
    if (pageErrorCategories.length < 8 && !pageErrorCategories.includes(category)) {
      pageErrorCategories.push(category);
    }
  });

  await page.addInitScript(installGpuDiagnostics);

  await page.goto(`${baseUrl}/test-geode.html`, { waitUntil: "domcontentloaded" });
  const status = page.locator("#status");
  const render = page.locator("#render-btn");
  await expect(render).toBeEnabled({ timeout: 45000 });
  await render.click();
  try {
    await expect(status).toContainText("Rendered 400x400 via Geode", { timeout: 45000 });
  } catch (error) {
    // Emit fixed stage names and counts; browser messages may carry paths or URLs.
    console.error(
      `browser stages: ${
        [...browserStages].join(",") || "none"
      }; page errors: ${pageErrorCount}; categories: ${pageErrorCategories.join(",") || "none"}`,
    );
    try {
      const gpuDiagnostics = await readGpuDiagnosticsBounded(page);
      console.error(
        `gpu at map deadline: ${formatGpuCounters(gpuDiagnostics?.atDeadline)}`
          + `; gpu at failure: ${formatGpuCounters(gpuDiagnostics?.current)}`,
      );
    } catch {
      console.error("gpu diagnostics unavailable");
    }
    throw error;
  }
  expect(
    browserBackendSelected,
    "the rendered page never selected the browser backend",
  ).toBe(true);
  expect(
    await page.evaluate(() =>
      (window as Window & { __donnerGpuDiagnostics: GpuDiagnostics })
        .__donnerGpuDiagnostics.current.deviceRequests
    ),
    "the standalone browser module must create only one WebGPU device",
  ).toBe(1);

  // Read the renderer's RGBA and the canvas the page presented it on. The golden covers the
  // renderer's output rather than the canvas: the 2D canvas premultiplies alpha, and browser
  // releases round its readback differently. The readback then shows that the page presented it.
  const captured = await page.evaluate(() => {
    const encode = (rgba: Uint8ClampedArray): string => {
      let binary = "";
      for (let offset = 0; offset < rgba.length; offset += 0x8000) {
        binary += String.fromCharCode(...rgba.subarray(offset, offset + 0x8000));
      }
      return btoa(binary);
    };
    const render = (window as Window & {
      __donnerLastRender?: { width: number; height: number; rgba: Uint8ClampedArray };
    }).__donnerLastRender;
    if (!render) return null;
    const canvas = document.getElementById("canvas") as HTMLCanvasElement;
    const shown = canvas.getContext("2d")!.getImageData(0, 0, canvas.width, canvas.height);
    return {
      rendered: { width: render.width, height: render.height, rgba: encode(render.rgba) },
      presented: { width: shown.width, height: shown.height, rgba: encode(shown.data) },
    };
  });
  expect(captured, "the page must expose the renderer's RGBA output").not.toBeNull();
  const outputDirectory = process.env.TEST_UNDECLARED_OUTPUTS_DIR;
  const compare = process.env.DONNER_BROWSER_GOLDEN_COMPARE;
  const golden = process.env.DONNER_BROWSER_GOLDEN_PNG;
  expect(outputDirectory, "Bazel must provide an artifact directory").toBeTruthy();
  expect(compare, "the shared pixelmatch comparator is missing").toBeTruthy();
  expect(golden, "the committed SVG golden is missing").toBeTruthy();
  // Keep both images with the test outputs before either comparison can fail.
  const actual = join(outputDirectory!, "standalone_geode_browser_actual.png");
  const presented = join(outputDirectory!, "standalone_geode_browser_presented.png");
  writeRgbaPng(actual, captured!.rendered);
  writeRgbaPng(presented, captured!.presented);
  expectBitmapsMatch(
    "the renderer output must match the committed golden",
    compare!,
    join(outputDirectory!, "standalone_geode_browser_golden_comparison.json"),
    {
      DONNER_ACTUAL_PNG: actual,
      // The golden helper uses this path in diff filenames; keep the runfiles path short.
      DONNER_GOLDEN_PNG: golden!,
    },
  );

  // The canvas readback must reproduce the renderer output exactly, so a skipped, blank,
  // misplaced or altered putImageData() fails. A browser whose readback rounds partially covered
  // pixels differently also fails here; see the comparator's presented_canvas mode.
  expectBitmapsMatch(
    "the canvas must present the renderer output",
    compare!,
    join(outputDirectory!, "standalone_geode_browser_presented_comparison.json"),
    {
      DONNER_BROWSER_COMPARE_MODE: "presented_canvas",
      DONNER_ACTUAL_PNG: presented,
      DONNER_EXPECTED_PNG: actual,
    },
  );

  expect(pageErrorCount, `browser page error categories: ${pageErrorCategories.join(",")}`).toBe(0);
});

test("standalone basic-text fixture paints fallback glyphs beyond its fontless reference", async ({ page }) => {
  test.setTimeout(90000);
  await page.goto(`${baseUrl}/test-geode.html`, { waitUntil: "domcontentloaded" });
  const render = page.locator("#render-btn");
  await expect(render).toBeEnabled({ timeout: 45000 });
  await page.locator("#w").fill("128");
  await page.locator("#h").fill("64");
  const prefix = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"128\" height=\"64\">"
    + "<rect width=\"1\" height=\"1\" fill=\"blue\"/>";
  async function capture(source: string) {
    await page.locator("#svg-input").fill(source);
    await render.click();
    await expect(page.locator("#status")).toContainText("Rendered 128x64 via Geode", {
      timeout: 45000,
    });
    return await page.evaluate(() => {
      const image = (window as Window & {
        __donnerLastRender?: { width: number; height: number; rgba: Uint8ClampedArray };
      }).__donnerLastRender;
      if (!image) throw new Error("Missing standalone renderer pixels");
      let red = 0;
      for (let index = 0; index < image.rgba.length; index += 4) {
        if (
          image.rgba[index] > 128 && image.rgba[index + 1] < 64
          && image.rgba[index + 2] < 64 && image.rgba[index + 3] > 128
        ) ++red;
      }
      return { width: image.width, height: image.height, red };
    });
  }
  const fontless = await capture(prefix + "</svg>");
  const text = await capture(
    prefix
      + "<text x=\"4\" y=\"42\" font-family=\"UninstalledFixtureFamily\" font-size=\"32\" fill=\"red\">Hi</text></svg>",
  );
  expect(fontless).toEqual({ width: 128, height: 64, red: 0 });
  expect(text.width).toBe(128);
  expect(text.height).toBe(64);
  expect(text.red).toBeGreaterThan(20);
  const outputs = process.env.TEST_UNDECLARED_OUTPUTS_DIR;
  expect(outputs, "Bazel must retain the text comparison receipt").toBeTruthy();
  writeFileSync(
    join(outputs!, "standalone_text_resource_comparison.json"),
    JSON.stringify({ fontless, text }) + "\n",
  );
});
