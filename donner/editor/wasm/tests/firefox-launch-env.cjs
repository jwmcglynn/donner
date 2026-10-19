const { execFileSync } = require("node:child_process");
const { existsSync, mkdirSync, mkdtempSync, rmSync } = require("node:fs");
const path = require("node:path");

// The standard per-user folders macOS creates on demand in a home directory. When the system
// creates them itself, it gives them an "everyone deny delete" ACL, which a test runner's
// recursive delete cannot remove. Creating them first keeps the home an ordinary directory tree.
const kStandardHomeFolders = ["Documents", "Downloads", "Library/Application Support", "Library/Caches"];

/**
 * Launch environment for a Bazel-owned Firefox on macOS.
 *
 * Firefox resolves its profile service under the user's home (`~/Library/Application Support`)
 * even when Playwright passes `-profile`. A test action may run as a service account whose home
 * does not exist or is not writable; Firefox then cannot start its profile service and, when
 * headed, waits on a "profile missing" dialog until the launch times out. Give every launch its
 * own writable home inside the test's temporary directory instead.
 *
 * Playwright loads its config in the runner and in every worker process, so each of those
 * processes creates (and removes on exit) its own home even if it never launches Firefox.
 *
 * @param environment The environment to extend.
 * @param temporary The test's private temporary directory, normally `TEST_TMPDIR`.
 */
function firefoxLaunchEnvironment(environment = process.env, temporary = environment.TEST_TMPDIR) {
  if (!temporary || process.platform !== "darwin") return { ...environment };
  if (!path.isAbsolute(temporary)) throw new Error("TEST_TMPDIR must be absolute");
  mkdirSync(temporary, { recursive: true });
  const home = mkdtempSync(path.join(temporary, "firefox-home-"));
  for (const folder of kStandardHomeFolders) {
    mkdirSync(path.join(home, folder), { recursive: true, mode: 0o700 });
  }
  process.once("exit", () => removeFirefoxHome(home));
  // CoreFoundation resolves the home from the account record unless CFFIXED_USER_HOME is set.
  return { ...environment, HOME: home, CFFIXED_USER_HOME: home };
}

/** Remove a launch home, stripping any ACL the system added after launch first. */
function removeFirefoxHome(home) {
  if (!existsSync(home)) return;
  try {
    execFileSync("/bin/chmod", ["-R", "-N", home], { stdio: "ignore" });
    rmSync(home, { recursive: true, force: true });
  } catch (error) {
    // Exit handlers must not throw; report the leftover so the next failure names it.
    console.error(`firefox launch home cleanup failed: ${String(error)}`);
  }
}

module.exports = { firefoxLaunchEnvironment, removeFirefoxHome };
