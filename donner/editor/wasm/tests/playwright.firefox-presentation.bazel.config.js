const baseConfig = require("./playwright.bazel.config.js");
const compatibilityConfig = require("./playwright.compatibility.config.js");
const { firefoxAquaLauncherPath } = require("./firefox-aqua-setup.js");

module.exports = {
  ...baseConfig,
  ...compatibilityConfig,
  globalSetup: "./firefox-aqua-setup.js",
  use: {},
  projects: compatibilityConfig.projects
    .filter((project) => project.name === "firefox-geode-resize")
    .map((project) => ({
      ...project,
      use: {
        ...project.use,
        headless: false,
        launchOptions: {
          ...project.use.launchOptions,
          executablePath: firefoxAquaLauncherPath(process.env),
          timeout: 15000,
        },
      },
    })),
};
