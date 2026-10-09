const baseConfig = require("./playwright.bazel.config.js");
const compatibilityConfig = require("./playwright.compatibility.config.js");

module.exports = {
  ...baseConfig,
  ...compatibilityConfig,
  use: {},
  projects: compatibilityConfig.projects.filter((project) => project.use.browserName === "webkit")
    .map((project) => ({
      ...project,
      use: {
        ...project.use,
        headless: process.env.DONNER_BROWSER_HEADLESS === "1",
        launchOptions: {
          ...project.use.launchOptions,
          executablePath: require("node:path").resolve(process.env.DONNER_WEBKIT_EXECUTABLE),
          timeout: 15000,
        },
      },
    })),
};
