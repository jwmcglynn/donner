const base = require("./playwright.bazel.config.js");
const { devices } = require("@playwright/test");

module.exports = {
  ...base,
  use: {},
  projects: [
    { name: "chromium-animation", use: base.use },
    {
      name: "firefox-animation",
      timeout: 90000,
      use: {
        ...devices["Desktop Firefox"],
        browserName: "firefox",
        headless: false,
        ignoreHTTPSErrors: true,
        launchOptions: { firefoxUserPrefs: { "dom.webgpu.enabled": true } },
      },
    },
  ],
};
