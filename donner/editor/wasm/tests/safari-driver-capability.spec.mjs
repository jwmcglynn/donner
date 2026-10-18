import assert from "node:assert/strict";
import { execFileSync, spawn } from "node:child_process";
import { once } from "node:events";
import { writeFile } from "node:fs/promises";
import { createServer } from "node:http";
import path from "node:path";
import test from "node:test";
import { observeProcessExit, stopOwnedProcess } from "./owned-process-lifecycle.mjs";

async function freePort() {
  const server = createServer();
  server.listen(0, "127.0.0.1");
  await once(server, "listening");
  const port = server.address().port;
  await new Promise((resolve) => server.close(resolve));
  return port;
}

async function request(base, method, endpoint, body, timeoutMs = 15000) {
  const response = await fetch(base + endpoint, {
    method,
    headers: { "content-type": "application/json" },
    body: body === undefined ? undefined : JSON.stringify(body),
    signal: AbortSignal.timeout(timeoutMs),
  });
  const payload = await response.json();
  if (!response.ok || payload.value?.error) {
    throw new Error(payload.value?.message || `SafariDriver status ${response.status}`);
  }
  return payload.value;
}

async function ready(base, child) {
  const deadline = Date.now() + 8000;
  while (Date.now() < deadline) {
    assert.equal(child.exitCode, null, "owned SafariDriver exited before readiness");
    try {
      const state = await request(base, "GET", "/status", undefined, 1000);
      if (state.ready) return;
    } catch {
      // Retry only the bounded startup readiness probe.
    }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error("SafariDriver did not become ready within eight seconds");
}

test("installed Safari permits an isolated visible WebGPU automation session", async () => {
  assert.equal(process.platform, "darwin");
  const output = process.env.TEST_UNDECLARED_OUTPUTS_DIR;
  assert.ok(output, "run this capability probe as its Bazel-owned remote test");
  const result = { passed: false, permissionsChanged: false };
  const cleanupErrors = [];
  let pageServer;
  let child;
  let exited;
  let base;
  let session;
  let failure;
  let driverLog = "";
  try {
    result.driverVersion = execFileSync("/usr/bin/safaridriver", ["--version"], {
      timeout: 5000,
      encoding: "utf8",
    }).trim();
    pageServer = createServer((_request, response) => {
      response.writeHead(200, {
        "content-type": "text/html",
        "cross-origin-opener-policy": "same-origin",
        "cross-origin-embedder-policy": "require-corp",
        "cache-control": "no-store",
      });
      response.end("<!doctype html><title>Donner Safari capability probe</title>");
    });
    pageServer.listen(0, "127.0.0.1");
    await once(pageServer, "listening");
    const driverPort = await freePort();
    base = `http://127.0.0.1:${driverPort}`;
    child = spawn("/usr/bin/safaridriver", ["-p", String(driverPort)], {
      stdio: ["ignore", "pipe", "pipe"],
    });
    const appendLog = (chunk) => {
      driverLog = (driverLog + String(chunk)).slice(-32768);
    };
    exited = observeProcessExit(child, (error) => appendLog(`driver error: ${error.message}\n`));
    child.stdout.on("data", appendLog);
    child.stderr.on("data", appendLog);
    await once(child, "spawn");
    await ready(base, child);
    const created = await request(base, "POST", "/session", {
      capabilities: { alwaysMatch: { browserName: "safari" } },
    });
    session = created.sessionId;
    result.browser = {
      name: created.capabilities.browserName,
      version: created.capabilities.browserVersion,
      platform: created.capabilities.platformName,
    };
    await request(base, "POST", `/session/${session}/url`, {
      url: `http://127.0.0.1:${pageServer.address().port}/`,
    });
    result.capability = await request(base, "POST", `/session/${session}/execute/async`, {
      script: `
        const done = arguments[arguments.length - 1];
        (async () => {
          const adapter = await navigator.gpu?.requestAdapter();
          const first = await new Promise(requestAnimationFrame);
          const second = await new Promise(requestAnimationFrame);
          done({isolated: crossOriginIsolated, visibility: document.visibilityState,
                webgpu: !!adapter, frameIntervalMs: second - first});
        })().catch(error => done({error: String(error)}));`,
      args: [],
    });
    assert.equal(result.capability.error, undefined);
    assert.equal(result.capability.isolated, true);
    assert.equal(result.capability.visibility, "visible");
    assert.equal(result.capability.webgpu, true);
    assert.ok(Number.isFinite(result.capability.frameIntervalMs));
    assert.ok(result.capability.frameIntervalMs > 0);
    result.passed = true;
  } catch (error) {
    failure = error;
    result.error = String(error.message || error);
  } finally {
    if (session) {
      await request(base, "DELETE", `/session/${session}`, undefined, 5000).catch((error) => {
        cleanupErrors.push(`session: ${error.message || error}`);
      });
    }
    if (child) {
      await stopOwnedProcess(child, exited).catch((error) => {
        cleanupErrors.push(`driver: ${error.message || error}`);
      });
    }
    if (pageServer?.listening) {
      pageServer.closeAllConnections();
      await new Promise((resolve) => pageServer.close(resolve));
    }
    if (cleanupErrors.length) {
      result.passed = false;
      result.cleanupErrors = cleanupErrors;
    }
    await writeFile(path.join(output, "safaridriver.log"), driverLog);
    await writeFile(path.join(output, "safari-capability.json"), JSON.stringify(result, null, 2));
  }
  if (failure) throw failure;
  assert.deepEqual(cleanupErrors, [], "owned Safari session and driver must close successfully");
});
