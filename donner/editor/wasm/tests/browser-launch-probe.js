// Global setup for the Bazel browser lanes: launch each project's browser once before any case.
//
// Playwright launches a browser per worker and relaunches it after a failed case, each launch with
// its own timeout. A browser that cannot start at all (a broken archive, a host that refuses the
// GPU process, a sandbox that blocks the binary) therefore failed every case of a lane one launch
// timeout at a time, and a lane with enough cases ran into the Bazel test timeout before it said
// why. One probe launch here turns that into a single failure, before the first case, that names
// the browser and the launch error. A healthy browser is closed again at once; the cases still
// launch their own, with their own options.
const { kBrowserLaunchTimeoutMs } = require("./test-timing.cjs");

/**
 * The launch timeout a probe uses: never shorter than the launch a case gets. A project that sets
 * none keeps Playwright's default (undefined), 0 keeps no limit, and any other value is raised to
 * the cold-launch allowance every Bazel lane shares.
 *
 * @param {number | undefined} timeout The project's own `launchOptions.timeout`.
 * @returns {number | undefined}
 */
function probeLaunchTimeout(timeout) {
  if (timeout === undefined || timeout === 0) return timeout;
  return Math.max(timeout, kBrowserLaunchTimeoutMs);
}

/**
 * The launches to probe: one per distinct browser and launch configuration among the projects.
 * Every project is probed: no target filters the projects of a probed config
 * (browser-matrix.spec.mjs).
 *
 * @param {{ projects: Array<{ name: string, use: Record<string, unknown> }> }} config The
 *     resolved Playwright config that global setup receives.
 * @returns {Array<{ browserName: string, projects: string[], options: Record<string, unknown> }>}
 */
function launchProbes(config) {
  const probes = new Map();
  for (const project of config.projects) {
    const use = project.use ?? {};
    const browserName = use.browserName ?? "chromium";
    const launchOptions = use.launchOptions ?? {};
    const options = {
      ...launchOptions,
      ...(use.channel === undefined ? {} : { channel: use.channel }),
      // As Playwright resolves it for a case: the project's `headless` wins over a launch option.
      headless: use.headless ?? launchOptions.headless ?? true,
    };
    const timeout = probeLaunchTimeout(launchOptions.timeout);
    if (timeout === undefined) delete options.timeout;
    else options.timeout = timeout;
    const key = JSON.stringify([browserName, options]);
    const probe = probes.get(key) ?? { browserName, projects: [], options };
    probe.projects.push(project.name || "(default)");
    probes.set(key, probe);
  }
  return [...probes.values()];
}

function firstLine(error) {
  return String(error?.message ?? error).split("\n", 1)[0];
}

function describeTimeout(timeout) {
  if (timeout === undefined) return "Playwright's default launch timeout";
  if (timeout === 0) return "no launch timeout";
  return `launch timeout ${timeout} ms`;
}

/** Resolves to "timeout" when `work` has not settled within `timeoutMs`. */
async function bounded(work, timeoutMs) {
  let timer;
  try {
    return await Promise.race([
      work,
      new Promise((resolve) => {
        timer = setTimeout(() => resolve("timeout"), timeoutMs);
      }),
    ]);
  } finally {
    clearTimeout(timer);
  }
}

/**
 * Launch and close each probed browser, or throw one error that names what failed.
 *
 * @param {object} config The resolved Playwright config.
 * @param {Record<string, { launch: (options: object) => Promise<{ close: () => Promise<void> }> }>}
 *     browserTypes Playwright's browser types, by browser name.
 * @param {{ closeTimeoutMs?: number }} bounds How long closing a probed browser may take.
 */
async function probeBrowserLaunch(
  config,
  browserTypes = require("@playwright/test"),
  { closeTimeoutMs = kBrowserLaunchTimeoutMs } = {},
) {
  for (const { browserName, projects, options } of launchProbes(config)) {
    const browserType = browserTypes[browserName];
    if (!browserType) throw new Error(`no Playwright browser type named ${browserName}`);
    const project = `project ${projects.join(", ")}`;
    let browser;
    try {
      browser = await browserType.launch(options);
    } catch (error) {
      throw new Error(
        `${browserName} did not launch (${describeTimeout(options.timeout)}) for ${project}, `
          + `so no case ran: ${firstLine(error)}`,
      );
    }
    // A browser that launched but cannot close would hold global setup with no bound of its own.
    if (await bounded(browser.close(), closeTimeoutMs) === "timeout") {
      throw new Error(
        `${browserName} launched for ${project} but did not close within ${closeTimeoutMs} ms, `
          + "so no case ran",
      );
    }
  }
}

module.exports = async function globalSetup(config) {
  await probeBrowserLaunch(config);
};
module.exports.launchProbes = launchProbes;
module.exports.probeLaunchTimeout = probeLaunchTimeout;
module.exports.probeBrowserLaunch = probeBrowserLaunch;
