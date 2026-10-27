const path = require("node:path");
const { chromiumLaunchEnvironment } = require("./chromium-launch-env.cjs");
const { stallGuardFor } = require("./test-timing.cjs");

module.exports = {
  testMatch: 'docs-selector.spec.ts',
  timeout: 10000,
  expect: { timeout: stallGuardFor(10000) },
  globalTimeout: 120000,
  retries: 0,
  workers: 1,
  reporter: [['list']],
  outputDir: path.join(process.env.TEST_UNDECLARED_OUTPUTS_DIR || process.env.TEST_TMPDIR, 'docs-browser'),
  use: {
    browserName: 'chromium', channel: 'chromium', headless: true,
    launchOptions: {env: chromiumLaunchEnvironment(process.env, process.env.TEST_TMPDIR)},
  },
};
