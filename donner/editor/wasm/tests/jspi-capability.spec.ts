import { expect, type Page, test } from "@playwright/test";

const baseUrl = process.env.DONNER_WASM_BASE_URL || "http://127.0.0.1:8000";

test.beforeEach(async ({ request }) => {
  const response = await request.get(`${baseUrl}/index.html`);
  expect(response.ok(), "the package must serve its index").toBe(true);
  expect(await response.text(), "the test must use the opt-in package").toContain(
    '<script src="editor-jspi.js"></script>',
  );
});

type ProbeTestWindow = Window & { __donnerProbeWorkerConfigured?: boolean };

async function configureProbeWorker(page: Page, setup: string): Promise<() => Promise<boolean>> {
  await page.addInitScript((workerSetup) => {
    const BrowserWorker = Worker;
    window.Worker = class extends BrowserWorker {
      constructor(url: string | URL, options?: WorkerOptions) {
        const probe = new URL(url, location.href);
        if (!probe.pathname.endsWith("/editor-jspi.js")) {
          super(url, options);
          return;
        }
        // Use a real worker and its unchanged packaged probe after applying the test override.
        const source = workerSetup + "\nimportScripts(" + JSON.stringify(probe.href) + ");";
        const blob = URL.createObjectURL(new Blob([source], { type: "text/javascript" }));
        super(blob, options);
        const revoke = () => URL.revokeObjectURL(blob);
        this.addEventListener("message", revoke, { once: true });
        this.addEventListener("error", revoke, { once: true });
        (window as ProbeTestWindow).__donnerProbeWorkerConfigured = true;
      }
    };
  }, setup);
  return () => page.evaluate(() => (window as ProbeTestWindow).__donnerProbeWorkerConfigured === true);
}

async function expectUnsupportedBeforeModuleRequest(page: Page): Promise<void> {
  const editorRequests: string[] = [];
  page.on("request", (request) => {
    if (/\/editor\.(?:js|wasm)$/.test(new URL(request.url()).pathname)) {
      editorRequests.push(request.url());
    }
  });

  await page.goto(`${baseUrl}/index.html`, { waitUntil: "networkidle" });
  await expect(page.locator("#capability-error")).toBeVisible();
  await expect(page.locator("#capability-error-detail")).toContainText("standard editor package");
  expect(editorRequests).toHaveLength(0);
}

test("JSPI package refuses a page without suspension before requesting module bytes", async ({ page }) => {
  await page.addInitScript(() => {
    Object.defineProperty(WebAssembly, "Suspending", { value: undefined, configurable: true });
  });
  await expectUnsupportedBeforeModuleRequest(page);
});

test("JSPI package refuses a worker without suspension before requesting module bytes", async ({ page }) => {
  const workerConfigured = await configureProbeWorker(
    page,
    'Object.defineProperty(WebAssembly, "promising", { value: undefined });',
  );
  await expectUnsupportedBeforeModuleRequest(page);
  expect(await workerConfigured(), "the page passed the first check and reached the worker").toBe(true);
});

test("JSPI preflight does not depend on an animation frame in its probe worker", async ({ page }) => {
  const workerConfigured = await configureProbeWorker(
    page,
    "globalThis.requestAnimationFrame = () => 0;",
  );
  await page.goto(`${baseUrl}/index.html`, { waitUntil: "domcontentloaded" });
  await expect.poll(
    () => page.evaluate(() =>
      (window as Window & { __donnerFirstFramePresented?: boolean }).__donnerFirstFramePresented
    ),
    { timeout: 30000 },
  ).toBe(true);
  expect(await workerConfigured(), "the probe worker received the paused rAF override").toBe(true);
});
