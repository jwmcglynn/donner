const { execFileSync } = require("node:child_process");
const { writeFileSync } = require("node:fs");
const path = require("node:path");

function requireEnvironment(environment, name) {
  const value = environment[name];
  if (!value) throw new Error(`${name} must be supplied by the Bazel test target`);
  return value;
}

function firefoxExecutablePath(environment) {
  return path.join(
    path.resolve(requireEnvironment(environment, "PLAYWRIGHT_BROWSERS_PATH")),
    "firefox-1538/firefox/Nightly.app/Contents/MacOS/firefox",
  );
}

function firefoxAquaLauncherPath(environment) {
  return path.join(
    path.resolve(requireEnvironment(environment, "TEST_TMPDIR")),
    "firefox-aqua.sh",
  );
}

function firefoxAquaLauncherScript(
  environment,
  uid,
  executable = firefoxExecutablePath(environment),
) {
  const quote = (value) => `'${value.replaceAll("'", `'"'"'`)}'`;
  return `#!/bin/sh\nexec /bin/launchctl asuser ${uid} ${quote(executable)} "$@"\n`;
}

function writeFirefoxAquaLauncher(
  environment,
  uid,
  executable = firefoxExecutablePath(environment),
) {
  writeFileSync(
    firefoxAquaLauncherPath(environment),
    firefoxAquaLauncherScript(environment, uid, executable),
    { mode: 0o700 },
  );
}

module.exports = function setup() {
  const uid = process.getuid();
  execFileSync("/bin/launchctl", ["print", `gui/${uid}`], { stdio: "ignore", timeout: 1000 });
  writeFirefoxAquaLauncher(process.env, uid);
};
module.exports.firefoxAquaLauncherPath = firefoxAquaLauncherPath;
module.exports.firefoxAquaLauncherScript = firefoxAquaLauncherScript;
module.exports.firefoxExecutablePath = firefoxExecutablePath;
module.exports.writeFirefoxAquaLauncher = writeFirefoxAquaLauncher;
