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
 * The launches to probe: one per distinct browser and launch configuration among the projects.
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
      headless: use.headless ?? true,
      // Never shorter than the launch a case would get, and never shorter than the cold-launch
      // allowance every Bazel lane shares.
      timeout: Math.max(launchOptions.timeout ?? 0, kBrowserLaunchTimeoutMs),
    };
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

/**
 * Launch and close each probed browser, or throw one error that names what failed.
 *
 * @param {object} config The resolved Playwright config.
 * @param {Record<string, { launch: (options: object) => Promise<{ close: () => Promise<void> }> }>}
 *     browserTypes Playwright's browser types, by browser name.
 */
async function probeBrowserLaunch(config, browserTypes = require("@playwright/test")) {
  for (const { browserName, projects, options } of launchProbes(config)) {
    const browserType = browserTypes[browserName];
    if (!browserType) throw new Error(`no Playwright browser type named ${browserName}`);
    let browser;
    try {
      browser = await browserType.launch(options);
    } catch (error) {
      throw new Error(
        `${browserName} did not launch (launch timeout ${options.timeout} ms) for project `
          + `${projects.join(", ")}, so no case ran: ${firstLine(error)}`,
      );
    }
    await browser.close();
  }
}

module.exports = async function globalSetup(config) {
  await probeBrowserLaunch(config);
};
module.exports.launchProbes = launchProbes;
module.exports.probeBrowserLaunch = probeBrowserLaunch;
