const path = require("node:path");

module.exports = {
  testDir: __dirname,
  testMatch: "font-reference.spec.ts",
  timeout: 10000,
  retries: 0,
  workers: 1,
  reporter: "list",
  outputDir: path.join(process.env.TEST_UNDECLARED_OUTPUTS_DIR, "playwright"),
  use: { headless: true, viewport: { width: 500, height: 500 }, deviceScaleFactor: 1 },
  projects: [
    { name: "chromium", use: { browserName: "chromium", channel: "chromium" } },
    { name: "firefox", use: { browserName: "firefox", headless: false, launchOptions: { timeout: 15000 } } },
  ],
};
