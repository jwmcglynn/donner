import assert from "node:assert/strict";
import {
  chmodSync,
  lstatSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  rmSync,
  symlinkSync,
  writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { test } from "node:test";
import { pathToFileURL } from "node:url";

const helper = resolve(process.env.TEST_SRCDIR ?? process.cwd(), process.argv[2]);
const { normalizePermissions } = await import(pathToFileURL(helper));

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
