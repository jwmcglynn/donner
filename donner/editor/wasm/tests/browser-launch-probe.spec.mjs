import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { createRequire } from "node:module";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const require = createRequire(import.meta.url);
const testDirectory = path.dirname(fileURLToPath(import.meta.url));
const { launchProbes, probeBrowserLaunch, probeLaunchTimeout } = require("./browser-launch-probe.js");
const { kBrowserLaunchTimeoutMs, kStallGuardMs } = require("./test-timing.cjs");

function fakeBrowserTypes(outcomes = {}) {
  const launches = [];
  const closed = [];
  const browserType = (name) => ({
    launch: async (options) => {
      launches.push([name, options]);
      if (outcomes[name] instanceof Error) throw outcomes[name];
      if (outcomes[name] === "hangs on close") return { close: () => new Promise(() => {}) };
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
  // No timeout of its own: Playwright's default applies to the probe as to the cases.
  assert.equal(probeLaunchTimeout(undefined), undefined);
  // No limit stays no limit.
  assert.equal(probeLaunchTimeout(0), 0);
  // Any other value: the larger of the project's and the shared cold-launch allowance.
  assert.equal(probeLaunchTimeout(15000), kBrowserLaunchTimeoutMs);
  assert.equal(probeLaunchTimeout(kBrowserLaunchTimeoutMs * 3), kBrowserLaunchTimeoutMs * 3);
  const probes = launchProbes({
    projects: [
      { name: "default", use: {} },
      { name: "unbounded", use: { launchOptions: { timeout: 0 } } },
      { name: "short", use: { launchOptions: { timeout: 15000 } } },
    ],
  });
  assert.equal("timeout" in probes[0].options, false, "no timeout is passed to Playwright");
  assert.equal(probes[1].options.timeout, 0);
  assert.equal(probes[2].options.timeout, kBrowserLaunchTimeoutMs);
});

test("the project's headless setting wins over a launch option, as for a case", () => {
  const headless = (use) => launchProbes({ projects: [{ name: "p", use }] })[0].options.headless;
  assert.equal(headless({ headless: false, launchOptions: { headless: true } }), false);
  assert.equal(headless({ launchOptions: { headless: false } }), false);
  assert.equal(headless({}), true);
});

test("a probed browser that does not close fails setup within its own bound", async () => {
  const { types } = fakeBrowserTypes({ chromium: "hangs on close" });
  await assert.rejects(
    probeBrowserLaunch({ projects: [{ name: "p", use: {} }] }, types, { closeTimeoutMs: 10 }),
    { message: "chromium launched for project p but did not close within 10 ms, so no case ran" },
  );
});

test("a browser that cannot launch fails the lane once, naming the browser and its error", async () => {
  const { launches, types } = fakeBrowserTypes({
    chromium: new Error("browserType.launch: Timeout 30000ms exceeded.\nCall log:\n  - launching"),
  });
  await assert.rejects(probeBrowserLaunch(kConfig, types), {
    message: "chromium did not launch (Playwright's default launch timeout) for project "
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
  const start = source.indexOf("module.exports = async function setup(config) {");
  assert.notEqual(start, -1, "the archive setup is the async global setup");
  const setup = source.slice(start);
  const extract = setup.indexOf("prepareBrowserArchives(process.env, config?.metadata?.pinnedBrowsers);");
  const probe = setup.indexOf("probeBrowserLaunch(config)");
  assert.notEqual(extract, -1, "the setup extracts the config's pinned browsers");
  assert.notEqual(probe, -1, "the setup probes the launch");
  assert.ok(extract < probe, "the pinned browsers are extracted before their launch is probed");
});
