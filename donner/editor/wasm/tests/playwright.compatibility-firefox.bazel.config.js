const baseConfig = require("./playwright.bazel.config.js");
const compatibilityConfig = require("./playwright.compatibility.config.js");

const firefoxProject = compatibilityConfig.projects.find(
  (project) => project.name === "firefox-geode-resize",
);
if (!firefoxProject) {
  throw new Error("Firefox compatibility project is missing");
}

module.exports = {
  ...baseConfig,
  ...compatibilityConfig,
  globalTimeout: 840000,
  projects: [
    {
      ...firefoxProject,
      use: {
        ...firefoxProject.use,
        headless: false,
        trace: {
          mode: "retain-on-failure",
          screenshots: false,
          snapshots: false,
          sources: false,
        },
        launchOptions: {
          ...firefoxProject.use.launchOptions,
          timeout: 15000,
        },
      },
    },
  ],
};
