const { mkdirSync } = require("node:fs");
const path = require("node:path");
const baseConfig = require("./playwright.config.js");

function requireEnvironment(name) {
  const value = process.env[name];
  if (!value) {
    throw new Error(`${name} must be supplied by the Bazel test target`);
  }
  return value;
}

function shellQuote(value) {
  return `'${value.replaceAll("'", `'"'"'`)}'`;
}

const temporaryDirectory = path.resolve(requireEnvironment("TEST_TMPDIR"));
// Chromium's crash database ignores its per-launch user-data-dir.
const crashDirectory = path.join(temporaryDirectory, "chromium-crashpad");
mkdirSync(crashDirectory, { recursive: true });

const server = path.resolve(requireEnvironment("DONNER_WASM_TEST_SERVER"));
const packageDirectory = path.resolve(requireEnvironment("DONNER_WASM_PACKAGE_DIR"));
const outputRoot = process.env.TEST_UNDECLARED_OUTPUTS_DIR || requireEnvironment("TEST_TMPDIR");

module.exports = {
  ...baseConfig,
  forbidOnly: true,
  outputDir: path.join(outputRoot, "playwright"),
  reporter: "list",
  retries: 0,
  workers: 1,
  use: {
    ...baseConfig.use,
    launchOptions: {
      ...baseConfig.use.launchOptions,
      env: {
        ...(baseConfig.use.launchOptions.env ?? process.env),
        BREAKPAD_DUMP_LOCATION: crashDirectory,
      },
    },
  },
  webServer: {
    command: [
      shellQuote(process.execPath),
      shellQuote(server),
      "--port 0",
      `--dir ${shellQuote(packageDirectory)}`,
    ].join(" "),
    timeout: 30000,
    // Playwright captures the bound port before loading test modules and
    // exports this named group to their worker processes.
    wait: {
      stdout: /DONNER_WASM_BASE_URL=(?<DONNER_WASM_BASE_URL>http:\/\/127\.0\.0\.1:\d+)/,
    },
  },
};
