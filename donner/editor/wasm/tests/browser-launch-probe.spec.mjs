import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { createRequire } from "node:module";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const require = createRequire(import.meta.url);
const testDirectory = path.dirname(fileURLToPath(import.meta.url));
const { launchProbes, probeBrowserLaunch } = require("./browser-launch-probe.js");
const { kBrowserLaunchTimeoutMs, kStallGuardMs } = require("./test-timing.cjs");

function fakeBrowserTypes(outcomes = {}) {
  const launches = [];
  const closed = [];
  const browserType = (name) => ({
    launch: async (options) => {
      launches.push([name, options]);
      if (outcomes[name] instanceof Error) throw outcomes[name];
      return { close: async () => closed.push(name) };
    },
  });
  return {
    launches,
    closed,
    types: { chromium: browserType("chromium"), firefox: browserType("firefox") },
  };
}

const kConfig = {
  projects: [
    {
      name: "chromium-animation",
      use: {
        channel: "chromium",
        headless: true,
        launchOptions: { args: ["--enable-unsafe-webgpu"], env: { BREAKPAD_DUMP_LOCATION: "x" } },
      },
    },
    {
      name: "firefox-animation",
      use: { browserName: "firefox", headless: false, launchOptions: { timeout: 30000 } },
    },
  ],
};

test("the launch allowance is a stall guard, not a latency budget", () => {
  assert.equal(kBrowserLaunchTimeoutMs, 30_000);
  assert.ok(kBrowserLaunchTimeoutMs >= kStallGuardMs, "a cold launch keeps the full stall guard");
});

test("each project's browser is launched once with that project's launch options", async () => {
  const { launches, closed, types } = fakeBrowserTypes();
  await probeBrowserLaunch(kConfig, types);
  assert.deepEqual(launches, [
    ["chromium", {
      args: ["--enable-unsafe-webgpu"],
      env: { BREAKPAD_DUMP_LOCATION: "x" },
      channel: "chromium",
      headless: true,
      timeout: kBrowserLaunchTimeoutMs,
    }],
    ["firefox", { timeout: kBrowserLaunchTimeoutMs, headless: false }],
  ]);
  assert.deepEqual(closed, ["chromium", "firefox"], "a healthy probe browser is closed again");
});

test("projects that launch the same browser the same way share one probe", () => {
  const sameLaunch = { use: { channel: "chromium", headless: true } };
  const probes = launchProbes({
    projects: [
      { name: "a", ...sameLaunch },
      { name: "b", ...sameLaunch },
      { name: "c", use: { channel: "chromium", headless: false } },
    ],
  });
  assert.deepEqual(probes.map((probe) => probe.projects), [["a", "b"], ["c"]]);
});

test("a probe never shortens the launch a case would get", () => {
  const [probe] = launchProbes({
    projects: [{ name: "slow", use: { launchOptions: { timeout: kBrowserLaunchTimeoutMs * 3 } } }],
  });
  assert.equal(probe.options.timeout, kBrowserLaunchTimeoutMs * 3);
});

test("a browser that cannot launch fails the lane once, naming the browser and its error", async () => {
  const { launches, types } = fakeBrowserTypes({
    chromium: new Error("browserType.launch: Timeout 30000ms exceeded.\nCall log:\n  - launching"),
  });
  await assert.rejects(probeBrowserLaunch(kConfig, types), {
    message: `chromium did not launch (launch timeout ${kBrowserLaunchTimeoutMs} ms) for project `
      + "chromium-animation, so no case ran: browserType.launch: Timeout 30000ms exceeded.",
  });
  assert.equal(launches.length, 1, "the first failed launch ends setup");
});

test("an unknown browser name is an error, not a skipped probe", async () => {
  const { types } = fakeBrowserTypes();
  await assert.rejects(
    probeBrowserLaunch({ projects: [{ name: "w", use: { browserName: "webkit" } }] }, types),
    /no Playwright browser type named webkit/,
  );
});

test("archive extraction composes with the probe instead of replacing it", () => {
  const source = readFileSync(path.join(testDirectory, "prepare-browser-archives.js"), "utf8");
  const setup = source.slice(source.indexOf("module.exports = async function setup(config) {"));
  assert.ok(
    setup.indexOf("prepareBrowserArchives(process.env, config?.metadata?.pinnedBrowsers);")
      < setup.indexOf("probeBrowserLaunch(config)"),
    "the pinned browsers are extracted before their launch is probed",
  );
  assert.ok(setup.indexOf("probeBrowserLaunch(config)") > 0);
});
