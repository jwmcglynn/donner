import { parse } from "espree";
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import test from "node:test";

const filenames = [
  "browser-file-dialogs.js",
  "catalog-fonts.js",
  "enable-threads.js",
  "packaged-resource-loader.js",
];

function scripts(variable) {
  const paths = process.env[variable]?.trim().split(/\s+/) ?? [];
  const result = new Map(paths.map((file) => [path.basename(file), file]));
  assert.equal(paths.length, filenames.length, `${variable}: file count`);
  assert.deepEqual(
    [...result.keys()].sort(),
    [...filenames].sort(),
    `${variable}: file identities`,
  );
  return result;
}

function withoutFormatting(node) {
  if (node === null || typeof node !== "object" || node instanceof RegExp) {
    return node;
  }
  if (Array.isArray(node)) {
    return node.map(withoutFormatting);
  }
  return Object.fromEntries(
    Object.entries(node)
      .filter(([key]) =>
        !["start", "end", "loc", "range"].includes(key)
        && !(key === "raw" && node.type === "Literal")
      )
      .map(([key, value]) => [key, withoutFormatting(value)]),
  );
}

function program(source) {
  return withoutFormatting(parse(source, { ecmaVersion: "latest", sourceType: "script" }));
}

const originals = scripts("ORIGINAL_SCRIPTS");
const minified = scripts("MINIFIED_SCRIPTS");
for (const filename of filenames) {
  test(`${filename}: smaller shipping script preserves the complete program`, () => {
    const before = fs.readFileSync(originals.get(filename), "utf8");
    const after = fs.readFileSync(minified.get(filename), "utf8");
    assert.deepEqual(program(after), program(before), "classic-script AST");
    assert.ok(Buffer.byteLength(after) < Buffer.byteLength(before), "shipping script is smaller");
  });
}

test("program comparison retains literals, regex flags, directives and raw template text", () => {
  for (
    const [before, after] of [
      ["const value = \"one\";", "const value = \"two\";"],
      ["const value = /abc/g;", "const value = /abc/i;"],
      ["\"use strict\";", "\"use asm\";"],
      ["tag`\\n`;", "tag`\n`;"],
      ["const value = 1n;", "const value = 2n;"],
    ]
  ) {
    assert.notDeepEqual(program(before), program(after));
  }
});

test("program comparison preserves distinct raw spellings with the same cooked template value", () => {
  const escaped = program("tag`\\n`;").body[0].expression.quasi.quasis[0].value;
  const literal = program("tag`\n`;").body[0].expression.quasi.quasis[0].value;
  assert.equal(escaped.cooked, literal.cooked);
  assert.notEqual(escaped.raw, literal.raw);
});
