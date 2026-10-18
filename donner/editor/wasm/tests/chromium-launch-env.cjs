const { mkdirSync } = require("node:fs");
const path = require("node:path");

function chromiumLaunchEnvironment(environment = process.env, temporary = environment.TEST_TMPDIR) {
  if (!temporary) return { ...environment };
  if (!path.isAbsolute(temporary)) throw new Error("TEST_TMPDIR must be absolute");
  // Crashpad ignores Chromium's per-launch user-data-dir.
  const directory = path.join(temporary, "chromium-crashpad");
  mkdirSync(directory, { recursive: true, mode: 0o700 });
  return { ...environment, BREAKPAD_DUMP_LOCATION: directory };
}

function launchChromiumForTest(browserType, options = {}) {
  const environment = options.env ?? process.env;
  return browserType.launch({
    ...options,
    env: chromiumLaunchEnvironment(environment, environment.TEST_TMPDIR ?? process.env.TEST_TMPDIR),
  });
}

module.exports = { chromiumLaunchEnvironment, launchChromiumForTest };
