const baseConfig = require("./playwright.bazel.config.js");
const firefoxConfig = require("./playwright.composited-firefox.config.js");
const { firefoxLaunchEnvironment } = require("./firefox-launch-env.cjs");

module.exports = {
  ...baseConfig,
  ...firefoxConfig,
  // The base Chromium config excludes these specs and supplies Chromium launch options.
  testIgnore: [],
  use: {},
  globalTimeout: 840000,
  projects: firefoxConfig.projects.map((project) => ({
    ...project,
    use: {
      ...project.use,
      headless: false,
      trace: {
        mode: "retain-on-failure",
        screenshots: false,
        snapshots: false,
        sources: false,
      },
      launchOptions: {
        ...project.use.launchOptions,
        env: firefoxLaunchEnvironment(project.use.launchOptions?.env ?? process.env),
        timeout: 15000,
      },
    },
  })),
};
