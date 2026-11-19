const { execFileSync } = require("node:child_process");
const { accessSync, constants, mkdirSync, statSync } = require("node:fs");
const path = require("node:path");

function requireEnvironment(environment, name) {
  const value = environment[name];
  if (!value) throw new Error(`${name} must be supplied by the Bazel test target`);
  return value;
}

function browserRoot(environment) {
  return path.join(path.resolve(requireEnvironment(environment, "TEST_TMPDIR")), "perf-browsers");
}

function browserExecutablePaths(environment) {
  const root = browserRoot(environment);
  return {
    webkit: path.join(root, "webkit/pw_run.sh"),
    chromium: path.join(
      root,
      "chromium/chrome-mac-arm64/Google Chrome for Testing.app/Contents/MacOS/Google Chrome for Testing",
    ),
    firefox: path.join(root, "firefox/firefox/Nightly.app/Contents/MacOS/firefox"),
  };
}

// Each pinned browser and the variable its Bazel target names its archive with.
const kArchiveVariables = {
  webkit: "DONNER_WEBKIT_ARCHIVE",
  chromium: "DONNER_CHROMIUM_ARCHIVE",
  firefox: "DONNER_FIREFOX_ARCHIVE",
};

/**
 * Extract the pinned browser archives into a fresh directory under TEST_TMPDIR.
 *
 * @param {Record<string, string | undefined>} environment The test environment.
 * @param {string[]} browsers The pinned browsers the caller launches; every one by default.
 */
function prepareBrowserArchives(environment, browsers = ["chromium", "firefox"]) {
  const root = browserRoot(environment);
  const archives = browsers.map((name) => {
    if (!(name in kArchiveVariables)) throw new Error(`no pinned browser archive named ${name}`);
    return [name, path.resolve(requireEnvironment(environment, kArchiveVariables[name]))];
  });
  for (const [name, archive] of archives) {
    if (!statSync(archive).isFile()) throw new Error(`${name} archive must be a file`);
  }
  // A fresh test-owned directory keeps native app symlinks out of Bazel tree artifacts.
  mkdirSync(root, { mode: 0o700 });
  const executables = browserExecutablePaths(environment);
  for (const [name, archive] of archives) {
    try {
      execFileSync("/usr/bin/ditto", ["-x", "-k", archive, path.join(root, name)], {
        timeout: 30000,
        stdio: "pipe",
      });
    } catch (error) {
      throw new Error(`Could not extract pinned ${name} archive (${error.code ?? error.status})`);
    }
    accessSync(executables[name], constants.X_OK);
  }
}

// As a Playwright global setup: extract the browsers the config lists in `metadata.pinnedBrowsers`
// (every pinned browser when it lists none), then launch each project's browser once so a
// browser that cannot start fails the lane before its first case.
module.exports = async function setup(config) {
  prepareBrowserArchives(process.env, config?.metadata?.pinnedBrowsers);
  await require("./browser-launch-probe.js").probeBrowserLaunch(config);
};
module.exports.browserExecutablePaths = browserExecutablePaths;
module.exports.prepareBrowserArchives = prepareBrowserArchives;
