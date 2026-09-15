import assert from "node:assert/strict";
import {
  accessSync,
  constants,
  existsSync,
  lstatSync,
  mkdtempSync,
  realpathSync,
  statSync,
} from "node:fs";
import { createRequire } from "node:module";
import path from "node:path";
import test from "node:test";

const require = createRequire(import.meta.url);
const { browserExecutablePaths, prepareBrowserArchives } = require("./prepare-browser-archives.js");

function environment() {
  assert.ok(process.env.TEST_TMPDIR, "Bazel must provide the test-owned directory");
  return {
    ...process.env,
    TEST_TMPDIR: mkdtempSync(path.join(process.env.TEST_TMPDIR, "archive-setup-")),
  };
}

test("archive setup requires its owned directory before writing", () => {
  assert.throws(() => prepareBrowserArchives({}), /TEST_TMPDIR must be supplied/);
});

test("both pinned archives are required before extraction starts", () => {
  const env = environment();
  delete env.DONNER_FIREFOX_ARCHIVE;
  assert.throws(() => prepareBrowserArchives(env), /DONNER_FIREFOX_ARCHIVE must be supplied/);
  assert.equal(existsSync(path.join(env.TEST_TMPDIR, "perf-browsers")), false);
});

test("pinned macOS archives preserve runnable executables and native framework symlinks", () => {
  const env = environment();
  prepareBrowserArchives(env);
  const executables = browserExecutablePaths(env);
  for (const executable of Object.values(executables)) {
    assert.ok(statSync(executable).isFile());
    accessSync(executable, constants.X_OK);
  }
  const current = path.join(
    path.dirname(executables.chromium),
    "../Frameworks/Google Chrome for Testing Framework.framework/Versions/Current",
  );
  assert.ok(lstatSync(current).isSymbolicLink(), "native framework layout must stay intact");
  const locale = path.join(current, "Resources/ca.lproj/locale.pak");
  assert.ok(statSync(locale).size > 0, "the previously missing framework resource must resolve");
  assert.ok(realpathSync(current).startsWith(realpathSync(env.TEST_TMPDIR) + path.sep));
  assert.throws(() => prepareBrowserArchives(env), /EEXIST/, "a stale runtime must not be reused");
});
