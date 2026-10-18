import assert from "node:assert/strict";
import { mkdtempSync, readFileSync, rmSync, statSync, writeFileSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import helpers from "./chromium-launch-env.cjs";

function temporary(t) {
  const directory = mkdtempSync(path.join(os.tmpdir(), "chromium-env-"));
  t.after(() => rmSync(directory, { recursive: true, force: true }));
  return directory;
}

test("Chromium receives a writable private crash directory and unchanged launch options", async (t) => {
  const directory = temporary(t);
  const environment = { TEST_TMPDIR: directory, HOME: "/unwritable-home", KEEP: "value" };
  const options = {
    channel: "chromium",
    headless: true,
    args: ["--use-gl=angle"],
    env: environment,
  };
  const result = {};
  const browserType = {
    launch: async (actual) => {
      assert.equal(actual.channel, options.channel);
      assert.equal(actual.headless, options.headless);
      assert.deepEqual(actual.args, options.args);
      assert.equal(actual.env.KEEP, "value");
      const crash = actual.env.BREAKPAD_DUMP_LOCATION;
      assert.equal(path.dirname(crash), directory);
      assert.equal(statSync(crash).mode & 0o777, 0o700);
      writeFileSync(path.join(crash, "probe"), "writable");
      assert.equal(readFileSync(path.join(crash, "probe"), "utf8"), "writable");
      return result;
    },
  };
  assert.equal(await helpers.launchChromiumForTest(browserType, options), result);
  assert.equal(environment.BREAKPAD_DUMP_LOCATION, undefined);
  assert.equal(options.env, environment);
});

test("explicit environments retain filtering", (t) => {
  const environment = { KEEP: "value" };
  const actual = helpers.chromiumLaunchEnvironment(environment, temporary(t));
  assert.equal(actual.KEEP, "value");
  assert.equal(actual.HOME, undefined);
  assert.deepEqual(environment, { KEEP: "value" });
});

test("standalone launches preserve their caller environment", () => {
  const environment = { KEEP: "value", BREAKPAD_DUMP_LOCATION: "/caller-path" };
  assert.deepEqual(helpers.chromiumLaunchEnvironment(environment), environment);
});

test("invalid or unwritable crash directories fail before launch", async (t) => {
  assert.throws(() => helpers.chromiumLaunchEnvironment({}, "relative"), /absolute/);
  const directory = temporary(t);
  writeFileSync(path.join(directory, "chromium-crashpad"), "occupied");
  let launched = false;
  assert.throws(() =>
    helpers.launchChromiumForTest({
      launch: () => {
        launched = true;
      },
    }, { env: { TEST_TMPDIR: directory } }), /EEXIST/);
  assert.equal(launched, false);
});
