import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import net from "node:net";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const serverPath = fileURLToPath(new URL("./remote-webserver.mjs", import.meta.url));
const readyPattern = /DONNER_WASM_BASE_URL=(http:\/\/127\.0\.0\.1:\d+)/;

function packageDirectory(t, index) {
  const directory = mkdtempSync(path.join(os.tmpdir(), "remote-webserver-test-"));
  writeFileSync(path.join(directory, "index.html"), `package ${index}`);
  t.after(() => rmSync(directory, { recursive: true, force: true }));
  return directory;
}

function waitForReady(child) {
  return new Promise((resolve, reject) => {
    let output = "";
    const timeout = setTimeout(() => reject(new Error("server did not report its port")), 5_000);
    child.stdout.on("data", (chunk) => {
      output += chunk.toString();
      const match = readyPattern.exec(output);
      if (match) {
        clearTimeout(timeout);
        resolve(match[1]);
      }
    });
    child.once("error", reject);
    child.once("exit", (code) => reject(new Error(`server exited before ready: ${code}`)));
  });
}

function startServer(t, directory) {
  const child = spawn(process.execPath, [serverPath, "--port", "0", "--dir", directory], {
    stdio: ["pipe", "pipe", "pipe"],
  });
  t.after(() => {
    child.stdin.end();
    child.kill("SIGKILL");
  });
  return { child, url: waitForReady(child) };
}

async function assertPortCloses(url) {
  const { port } = new URL(url);
  const deadline = Date.now() + 3_000;
  for (;;) {
    const open = await new Promise((resolve) => {
      const socket = net.connect(Number(port), "127.0.0.1");
      socket.once("connect", () => {
        socket.destroy();
        resolve(true);
      });
      socket.once("error", () => resolve(false));
    });
    if (!open) {
      return;
    }
    if (Date.now() >= deadline) {
      throw new Error(`server still accepts connections after its parent exited: ${url}`);
    }
    await new Promise((resolve) => setTimeout(resolve, 25));
  }
}

test("an occupied port does not conflict with a runtime-bound server", async (t) => {
  const occupied = net.createServer();
  await new Promise((resolve) => occupied.listen(0, "127.0.0.1", resolve));
  t.after(() => occupied.close());
  const { url } = startServer(t, packageDirectory(t, "A"));
  const boundUrl = await url;
  assert.notEqual(Number(new URL(boundUrl).port), occupied.address().port);
  assert.equal(await (await fetch(`${boundUrl}/index.html`)).text(), "package A");
});

test("two browser servers use distinct ports and packages", async (t) => {
  const first = startServer(t, packageDirectory(t, "first"));
  const second = startServer(t, packageDirectory(t, "second"));
  const [firstUrl, secondUrl] = await Promise.all([first.url, second.url]);
  assert.notEqual(firstUrl, secondUrl);
  assert.equal(await (await fetch(`${firstUrl}/index.html`)).text(), "package first");
  assert.equal(await (await fetch(`${secondUrl}/index.html`)).text(), "package second");
});

test("SIGKILL of the server owner closes its listening port", async (t) => {
  const directory = packageDirectory(t, "orphan");
  const parentCode = `
    const { spawn } = require("node:child_process");
    const child = spawn(process.execPath, [${JSON.stringify(serverPath)}, "--port", "0", "--dir", ${
    JSON.stringify(directory)
  }], {
      stdio: ["pipe", "pipe", "pipe"],
    });
    child.stdout.pipe(process.stdout);
    child.stderr.pipe(process.stderr);
  `;
  const parent = spawn(process.execPath, ["-e", parentCode], {
    stdio: ["ignore", "pipe", "pipe"],
  });
  t.after(() => parent.kill("SIGKILL"));
  const url = await waitForReady(parent);
  assert.equal(await (await fetch(`${url}/index.html`)).text(), "package orphan");
  parent.kill("SIGKILL");
  await assertPortCloses(url);
});
