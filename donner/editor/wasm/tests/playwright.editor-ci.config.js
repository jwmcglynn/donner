const base = require("./playwright.bazel.config.js");
const { firefoxLaunchOptions } = require("./firefox-launch-env.cjs");
const { browserExecutablePaths } = require("./prepare-browser-archives.js");

const name = process.env.DONNER_EDITOR_BROWSER_PROJECT;
function sourceConfig() {
  if (name === "composited-chromium") return require("./playwright.composited-chromium.bazel.config.js");
  if (name === "firefox-composited-invariants") return require("./playwright.composited-firefox.bazel.config.js");
  if (name === "chromium-default") return base;
  return require("./playwright.compatibility.config.js");
}

const config = sourceConfig();
function selectedProject() {
  if (!config.projects) return { name: "", use: config.use };
  const selected = config.projects.find((project) => project.name === name);
  if (!selected) throw new Error("Unknown editor browser project: " + name);
  if (selected.use.browserName !== "firefox" || selected.use.launchOptions?.env) return selected;
  return {
    ...selected,
    use: {
      ...selected.use,
      launchOptions: {
        ...selected.use.launchOptions,
        ...firefoxLaunchOptions(process.env),
        firefoxUserPrefs: { "dom.webgpu.enabled": true },
      },
    },
  };
}

const project = selectedProject();
const engine = project.use.browserName || "chromium";
const launchOptions = project.use.launchOptions || {};
module.exports = {
  ...base,
  ...config,
  testMatch: "**/*.spec.ts",
  testIgnore: [],
  metadata: { pinnedBrowsers: [engine] },
  globalSetup: require.resolve("./prepare-browser-archives.js"),
  use: {},
  projects: [{
    ...project,
    use: {
      ...project.use,
      headless: false,
      launchOptions: {
        ...launchOptions,
        executablePath: browserExecutablePaths(process.env)[engine],
      },
    },
  }],
  reporter: [["list"], [require.resolve("./editor-browser-coverage.cjs")]],
};
