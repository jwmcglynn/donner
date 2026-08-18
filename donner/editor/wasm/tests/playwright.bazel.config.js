const { createHash } = require("node:crypto");
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

const testIdentity = process.env.TEST_TMPDIR || process.cwd();
const portSeed = createHash("sha256").update(testIdentity).digest().readUInt16BE(0);
const port = 20000 + (portSeed % 20000);
const baseUrl = `http://127.0.0.1:${port}`;
const server = path.resolve(requireEnvironment("DONNER_WASM_TEST_SERVER"));
const packageDirectory = path.resolve(requireEnvironment("DONNER_WASM_PACKAGE_DIR"));
const outputRoot = process.env.TEST_UNDECLARED_OUTPUTS_DIR || requireEnvironment("TEST_TMPDIR");

process.env.DONNER_WASM_BASE_URL = baseUrl;

module.exports = {
  ...baseConfig,
  forbidOnly: true,
  outputDir: path.join(outputRoot, "playwright"),
  reporter: "list",
  retries: 0,
  workers: 1,
  webServer: {
    command: [
      shellQuote(process.execPath),
      shellQuote(server),
      `--port ${port}`,
      `--dir ${shellQuote(packageDirectory)}`,
    ].join(" "),
    reuseExistingServer: false,
    timeout: 30000,
    url: `${baseUrl}/index.html`,
  },
};
