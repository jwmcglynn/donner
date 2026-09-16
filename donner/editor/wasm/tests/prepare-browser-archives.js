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
    chromium: path.join(
      root,
      "chromium/chrome-mac-arm64/Google Chrome for Testing.app/Contents/MacOS/Google Chrome for Testing",
    ),
    firefox: path.join(root, "firefox/firefox/Nightly.app/Contents/MacOS/firefox"),
  };
}

function prepareBrowserArchives(environment) {
  const root = browserRoot(environment);
  const archives = [
    ["chromium", requireEnvironment(environment, "DONNER_CHROMIUM_ARCHIVE")],
    ["firefox", requireEnvironment(environment, "DONNER_FIREFOX_ARCHIVE")],
  ].map(([name, archive]) => [name, path.resolve(archive)]);
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

module.exports = function setup() {
  prepareBrowserArchives(process.env);
};
module.exports.browserExecutablePaths = browserExecutablePaths;
module.exports.prepareBrowserArchives = prepareBrowserArchives;
