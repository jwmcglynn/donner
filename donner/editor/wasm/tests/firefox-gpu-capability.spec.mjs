import { firefox } from "@playwright/test";
import assert from "node:assert/strict";
import { once } from "node:events";
import { writeFile } from "node:fs/promises";
import { createServer } from "node:http";
import path from "node:path";
import test from "node:test";
import { observeProcessExit, stopOwnedProcess } from "./owned-process-lifecycle.mjs";

const workerSource = `
self.onmessage = async ({data: canvas}) => {
  let device;
  try {
    const adapter = await navigator.gpu?.requestAdapter();
    if (!adapter) { postMessage({webgpu: false}); return; }
    device = await adapter.requestDevice();
    device.pushErrorScope("validation");
    const context = canvas.getContext("webgpu");
    context.configure({device, format: navigator.gpu.getPreferredCanvasFormat()});
    const encoder = device.createCommandEncoder();
    const pass = encoder.beginRenderPass({colorAttachments: [{
      view: context.getCurrentTexture().createView(),
      clearValue: {r: 1, g: 0, b: 0, a: 1}, loadOp: "clear", storeOp: "store"
    }]});
    pass.end();
    device.queue.submit([encoder.finish()]);
    await device.queue.onSubmittedWorkDone();
    const error = await device.popErrorScope();
    postMessage({webgpu: true, submitted: !error, validationError: error?.message ?? null,
      vendor: adapter.info?.vendor ?? null, architecture: adapter.info?.architecture ?? null,
      fallback: adapter.isFallbackAdapter ?? adapter.info?.isFallbackAdapter ?? null,
      workerRaf: typeof requestAnimationFrame === "function"});
    context.unconfigure();
  } catch (error) {
    postMessage({error: String(error)});
  } finally {
    device?.destroy();
  }
};`;

async function cleanupStep(name, work, errors) {
  let timer;
  try {
    await Promise.race([
      work(),
      new Promise((_, reject) => {
        timer = setTimeout(() => reject(new Error("cleanup deadline expired")), 5000);
      }),
    ]);
  } catch (error) {
    errors.push(`${name}: ${error.name}`);
  } finally {
    clearTimeout(timer);
  }
}

function failureDescription(stage, error) {
  if (String(error).includes("Host system is missing dependencies")) {
    return "Pinned Firefox requires browser runtime libraries absent from this runner";
  }
  return `${stage} failed (${error.name})`;
}

test("pinned Firefox launches and submits WebGPU work from an isolated worker", async () => {
  const output = process.env.TEST_UNDECLARED_OUTPUTS_DIR;
  assert.ok(output, "run this probe as its Bazel-owned test");
  const report = { platform: process.platform, architecture: process.arch, passed: false };
  let browser;
  let browserServer;
  let browserProcess;
  let browserExited;
  const cleanupErrors = [];
  let stage = "launch";
  let failure;
  const server = createServer((request, response) => {
    const worker = request.url === "/worker.js";
    response.writeHead(200, {
      "content-type": worker ? "text/javascript" : "text/html",
      "cross-origin-opener-policy": "same-origin",
      "cross-origin-embedder-policy": "require-corp",
      "cache-control": "no-store",
    });
    response.end(worker ? workerSource : "<!doctype html><title>Firefox GPU capability</title>");
  });
  try {
    browserServer = await firefox.launchServer({
      host: "127.0.0.1",
      headless: true,
      timeout: 15000,
      firefoxUserPrefs: { "dom.webgpu.enabled": true },
    });
    browserProcess = browserServer.process();
    browserExited = observeProcessExit(browserProcess, (error) => {
      cleanupErrors.push(`browser process: ${error.code ?? error.name}`);
    });
    browser = await firefox.connect(browserServer.wsEndpoint(), { timeout: 10000 });
    report.browserVersion = browser.version();
    server.listen(0, "127.0.0.1");
    await once(server, "listening");
    stage = "isolated worker GPU submission";
    const page = await browser.newPage();
    await page.goto(`http://127.0.0.1:${server.address().port}/`);
    report.capability = await page.evaluate(async () => {
      const canvas = document.createElement("canvas");
      canvas.width = canvas.height = 32;
      document.body.append(canvas);
      const worker = new Worker("/worker.js");
      let timer;
      try {
        const result = new Promise((resolve, reject) => {
          timer = setTimeout(() => reject(new Error("worker GPU probe timed out")), 15000);
          worker.onmessage = ({ data }) => resolve(data);
          worker.onerror = () => reject(new Error("worker GPU probe failed"));
        });
        const offscreen = canvas.transferControlToOffscreen();
        worker.postMessage(offscreen, [offscreen]);
        return { isolated: crossOriginIsolated, ...(await result) };
      } finally {
        clearTimeout(timer);
        worker.terminate();
      }
    });
    assert.equal(report.capability.isolated, true);
    assert.equal(report.capability.error, undefined);
    assert.equal(report.capability.webgpu, true);
    assert.equal(report.capability.submitted, true);
    report.passed = true;
  } catch (error) {
    report.failureStage = stage;
    report.error = failureDescription(stage, error);
    failure = error;
  } finally {
    if (browserServer) {
      await cleanupStep("browser server", () => browserServer.close(), cleanupErrors);
    }
    if (browserProcess?.exitCode === null && browserProcess?.signalCode === null) {
      await cleanupStep("forced browser termination", () => browserServer.kill(), cleanupErrors);
    }
    if (browserProcess) {
      await cleanupStep(
        "owned browser process",
        () => stopOwnedProcess(browserProcess, browserExited, 1000),
        cleanupErrors,
      );
    }
    if (browser) await cleanupStep("browser client", () => browser.close(), cleanupErrors);
    if (server.listening) {
      await cleanupStep("page server", () => {
        server.closeAllConnections();
        return new Promise((resolve) => server.close(resolve));
      }, cleanupErrors);
    }
    if (cleanupErrors.length) {
      report.passed = false;
      report.cleanupErrors = cleanupErrors;
    }
    await writeFile(path.join(output, "firefox-capability.json"), JSON.stringify(report, null, 2));
  }
  if (failure) throw new Error(report.error);
  assert.deepEqual(cleanupErrors, []);
});
