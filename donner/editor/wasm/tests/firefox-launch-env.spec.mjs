import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import {
  existsSync,
  mkdirSync,
  mkdtempSync,
  rmSync,
  statSync,
  symlinkSync,
  writeFileSync,
} from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import helpers from "./firefox-launch-env.cjs";

const kDarwin = process.platform === "darwin";

function temporary(t) {
  const directory = mkdtempSync(path.join(os.tmpdir(), "firefox-env-"));
  t.after(() => {
    // Strip ACLs from real entries only: `chmod -R -N` would fail on the dangling symlink case.
    if (kDarwin) {
      const realEntries = ["(", "-type", "d", "-o", "-type", "f", ")"];
      execFileSync("/usr/bin/find", [directory, ...realEntries, "-exec", "/bin/chmod", "-N", "{}", "+"]);
    }
    rmSync(directory, { recursive: true, force: true });
  });
  return directory;
}

/** The ACL entries of `file` itself, as listed by `ls -lde`. */
function aclOf(file) {
  return execFileSync("/bin/ls", ["-lde", file], { encoding: "utf8" })
    .split("\n")
    .slice(1)
    .filter((line) => line.trim() !== "");
}

test("macOS Firefox receives a private writable home with its standard folders", { skip: !kDarwin }, (t) => {
  const directory = temporary(t);
  const environment = { TEST_TMPDIR: directory, HOME: "/var/empty", KEEP: "value" };
  const actual = helpers.firefoxLaunchEnvironment(environment, directory);
  assert.equal(actual.KEEP, "value");
  assert.equal(path.dirname(actual.HOME), directory);
  assert.equal(actual.CFFIXED_USER_HOME, actual.HOME);
  assert.equal(statSync(actual.HOME).mode & 0o777, 0o700);
  for (const folder of ["Documents", "Downloads", "Library/Application Support", "Library/Caches"]) {
    assert.ok(statSync(path.join(actual.HOME, folder)).isDirectory(), folder);
  }
  assert.deepEqual(environment, { TEST_TMPDIR: directory, HOME: "/var/empty", KEEP: "value" });
});

test("each launch environment gets its own home", { skip: !kDarwin }, (t) => {
  const directory = temporary(t);
  const first = helpers.firefoxLaunchEnvironment({}, directory);
  const second = helpers.firefoxLaunchEnvironment({}, directory);
  assert.notEqual(first.HOME, second.HOME);
});

test("home removal strips a system deny-delete ACL first", { skip: !kDarwin }, (t) => {
  const home = path.join(temporary(t), "home");
  const documents = path.join(home, "Documents");
  mkdirSync(documents, { recursive: true });
  writeFileSync(path.join(documents, ".localized"), "");
  execFileSync("/bin/chmod", ["+a", "group:everyone deny delete", documents]);
  assert.throws(() => rmSync(home, { recursive: true }), /EACCES|EPERM|ENOTEMPTY/);
  helpers.removeFirefoxHome(home);
  assert.equal(existsSync(home), false);
});

test("home removal leaves ACLs on symlink targets outside the home", { skip: !kDarwin }, (t) => {
  const directory = temporary(t);
  const home = path.join(directory, "home");
  const outsideFile = path.join(directory, "outside-file");
  const outsideDirectory = path.join(directory, "outside-directory");
  mkdirSync(home);
  writeFileSync(outsideFile, "");
  mkdirSync(outsideDirectory);
  for (const target of [outsideFile, outsideDirectory]) {
    execFileSync("/bin/chmod", ["+a", "group:everyone deny delete", target]);
  }
  symlinkSync(outsideFile, path.join(home, "file-link"));
  symlinkSync(outsideDirectory, path.join(home, "directory-link"));
  const before = [aclOf(outsideFile), aclOf(outsideDirectory)];
  assert.equal(before[0].length, 1, before[0].join("\n"));
  assert.equal(before[1].length, 1, before[1].join("\n"));

  helpers.removeFirefoxHome(home);

  assert.equal(existsSync(home), false);
  assert.deepEqual([aclOf(outsideFile), aclOf(outsideDirectory)], before);
});

test("home removal still deletes a home that contains a dangling symlink", { skip: !kDarwin }, (t) => {
  const home = path.join(temporary(t), "home");
  const documents = path.join(home, "Documents");
  mkdirSync(documents, { recursive: true });
  execFileSync("/bin/chmod", ["+a", "group:everyone deny delete", documents]);
  symlinkSync(path.join(home, "missing"), path.join(home, "dangling-link"));

  helpers.removeFirefoxHome(home);

  assert.equal(existsSync(home), false);
});

test("non-macOS and standalone launches keep the caller environment", (t) => {
  const environment = { KEEP: "value", HOME: "/caller-home" };
  assert.deepEqual(helpers.firefoxLaunchEnvironment(environment, undefined), environment);
  if (!kDarwin) {
    assert.deepEqual(helpers.firefoxLaunchEnvironment(environment, temporary(t)), environment);
  }
});

test("a relative temporary directory fails before launch", { skip: !kDarwin }, () => {
  assert.throws(() => helpers.firefoxLaunchEnvironment({}, "relative"), /absolute/);
});
