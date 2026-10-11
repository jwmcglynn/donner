const fs = require("node:fs");
const path = require("node:path");
const { execFileSync } = require("node:child_process");
const prepare = require("./prepare-browser-archives.js");
const { writeFirefoxAquaLauncher } = require("./firefox-aqua-setup.js");
const { writeHeartbeat } = require("./browser-heartbeat.cjs");
module.exports = async function setup(config) {
  const heartbeat = process.env.DONNER_WATCHDOG_HEARTBEAT;
  if (!heartbeat) throw new Error("browser timing tests require their external watchdog");
  const beat = () => {
    if (!fs.existsSync(`${heartbeat}.app-active`)) {
      let prior;
      try {
        prior = JSON.parse(fs.readFileSync(heartbeat, "utf8"));
      } catch {}
      writeHeartbeat(heartbeat, {
        atMs: Date.now(),
        phase: prior?.phase === "teardown" ? "teardown" : "launch",
        phaseStartedAtMs: prior?.phaseStartedAtMs,
        sessionId: prior?.sessionId,
      });
    }
  };
  beat();
  const timer = setInterval(beat, 1000);
  timer.unref();
  await prepare(config);
  if (process.env.DONNER_FIREFOX_GUI_BOOTSTRAP === "1") {
    const uid = process.getuid();
    execFileSync("/bin/launchctl", ["print", `gui/${uid}`], { stdio: "ignore", timeout: 1000 });
    writeFirefoxAquaLauncher(process.env, uid, prepare.browserExecutablePaths(process.env).firefox);
  }

  // An unref timer keeps teardown responsive to the watchdog without retaining the driver.
  return () => {};
};
