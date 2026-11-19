import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import {
  chmodSync,
  lstatSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  readdirSync,
  rmSync,
  symlinkSync,
  writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { test } from "node:test";
import { pathToFileURL } from "node:url";

const helper = resolve(process.env.TEST_SRCDIR ?? process.cwd(), process.argv[2]);
const { normalizePermissions, prepareOutputDirectory } = await import(pathToFileURL(helper));

function fixture(run) {
  const root = mkdtempSync(join(tmpdir(), "npm-permission-test-"));
  try {
    run(root);
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
}

const mode = (path) => lstatSync(path).mode & 0o777;

test("file-like directory modes become traversable without changing bytes", () =>
  fixture((root) => {
    const lib = join(root, "lib");
    mkdirSync(lib);
    const script = join(lib, "entry.js");
    writeFileSync(script, "export const value = 7;\n");
    chmodSync(script, 0o200);
    chmodSync(lib, 0o644);
    normalizePermissions(root);
    assert.equal(mode(lib), 0o755);
    assert.equal(mode(script), 0o644);
    assert.equal(readFileSync(script, "utf8"), "export const value = 7;\n");
  }));

test("existing executable bits survive normalization", () =>
  fixture((root) => {
    const script = join(root, "bin");
    writeFileSync(script, "#!/usr/bin/env node\n");
    chmodSync(script, 0o700);
    normalizePermissions(root);
    assert.equal(mode(script), 0o744);
  }));

test("symlinks remain unchanged and external targets are never opened", () =>
  fixture((root) => {
    const packageRoot = join(root, "package");
    mkdirSync(packageRoot);
    const external = join(root, "external");
    writeFileSync(external, "external sentinel");
    chmodSync(external, 0o200);
    symlinkSync(external, join(packageRoot, "outside"));
    symlinkSync("missing-target", join(packageRoot, "broken"));
    symlinkSync(".", join(packageRoot, "cycle"));
    normalizePermissions(packageRoot);
    assert.equal(mode(external), 0o200);
    for (const name of ["outside", "broken", "cycle"]) {
      assert.equal(lstatSync(join(packageRoot, name)).isSymbolicLink(), true);
    }
  }));

test("foreign-owned declared empty directory is replaced before extraction", () =>
  fixture((root) => {
    const output = join(root, "pkg");
    mkdirSync(output);
    chmodSync(output, 0o700);
    const before = lstatSync(output);
    prepareOutputDirectory(output, before.uid);
    assert.equal(lstatSync(output).ino, before.ino);
    prepareOutputDirectory(output, before.uid + 1);
    assert.notEqual(lstatSync(output).ino, before.ino);
    assert.deepEqual(readdirSync(root), ["pkg"]);
    normalizePermissions(output);
    assert.equal(mode(output) & 0o555, 0o555);
  }));

test("already-normalized foreign output keeps its existing inode", () =>
  fixture((root) => {
    const output = join(root, "pkg");
    mkdirSync(output);
    chmodSync(output, 0o755);
    const before = lstatSync(output);
    prepareOutputDirectory(output, before.uid + 1);
    assert.equal(lstatSync(output).ino, before.ino);
  }));

test("normalized foreign output must be empty to extract in place", () =>
  fixture((root) => {
    const output = join(root, "pkg");
    mkdirSync(output);
    chmodSync(output, 0o777);
    writeFileSync(join(output, "keep"), "preserved");
    assert.throws(() => prepareOutputDirectory(output, lstatSync(output).uid + 1), /empty/);
    assert.equal(readFileSync(join(output, "keep"), "utf8"), "preserved");
  }));

test("normalization removes special bits without dropping executable bits", () =>
  fixture((root) => {
    const file = join(root, "tool");
    writeFileSync(file, "#!/bin/sh\n");
    chmodSync(file, 0o6755);
    chmodSync(root, 0o1755);
    normalizePermissions(root);
    assert.equal(lstatSync(root).mode & 0o7777, 0o755);
    assert.equal(lstatSync(file).mode & 0o7777, 0o755);
  }));

test("adoption preserves nonempty directories and rejects symlink outputs", () =>
  fixture((root) => {
    const output = join(root, "pkg");
    mkdirSync(output);
    const sentinel = join(output, "keep");
    writeFileSync(sentinel, "preserved");
    chmodSync(output, 0o700);
    assert.throws(() => prepareOutputDirectory(output, lstatSync(output).uid + 1));
    assert.equal(readFileSync(sentinel, "utf8"), "preserved");
    assert.deepEqual(readdirSync(root), ["pkg"]);
    const link = join(root, "link");
    symlinkSync(output, link);
    assert.throws(() => prepareOutputDirectory(link), /Expected a directory/);
    assert.equal(lstatSync(link).isSymbolicLink(), true);
  }));

test("CLI extracts through both direct and symlinked input paths", () =>
  fixture((root) => {
    const linkedHelper = join(root, "helper-link.mjs");
    symlinkSync(helper, linkedHelper);
    const extractor = join(root, "extractor.mjs");
    writeFileSync(
      extractor,
      `
import { mkdirSync, writeFileSync, chmodSync } from "node:fs";
import { join } from "node:path";
const output = process.argv[2];
mkdirSync(output);
writeFileSync(join(output, "sentinel"), "package bytes");
chmodSync(output, 0o644);
`,
    );
    for (const [index, entry] of [helper, linkedHelper].entries()) {
      const output = join(root, `output-${index}`);
      execFileSync(process.execPath, [entry, process.execPath, output, extractor, output]);
      assert.equal(mode(output), 0o755);
      assert.equal(readFileSync(join(output, "sentinel"), "utf8"), "package bytes");
    }
  }));

test("CLI propagates extractor failure through symlinked inputs", () =>
  fixture((root) => {
    const linkedHelper = join(root, "helper-link.mjs");
    symlinkSync(helper, linkedHelper);
    assert.throws(
      () =>
        execFileSync(process.execPath, [
          linkedHelper,
          process.execPath,
          join(root, "output"),
          "-e",
          "process.exit(7)",
        ]),
      (error) => error.status === 7,
    );
  }));
