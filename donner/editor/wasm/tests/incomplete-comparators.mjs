// Comparator stubs and caller test-protocol files for the browser-free tests of the callers of
// browser-bitmap-comparator.ts.
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";

const kCallerProtocolSentinel = "caller test protocol sentinel\n";
const kCallerProtocolFileNames = {
  XML_OUTPUT_FILE: "caller-test.xml",
  TEST_PREMATURE_EXIT_FILE: "caller-premature-exit",
  TEST_SHARD_STATUS_FILE: "caller-shard-status",
  TEST_WARNINGS_OUTPUT_FILE: "caller-warnings",
  GTEST_SHARD_STATUS_FILE: "caller-gtest-shard-status",
};

/**
 * Writes the calling test's protocol files into `directory`, each holding a sentinel. Returns the
 * settings that name them, the JSON report that an inherited `GTEST_OUTPUT` would name (left for
 * the caller to set), and every file.
 */
export function writeCallerProtocolFiles(directory) {
  const settings = Object.fromEntries(
    Object.entries(kCallerProtocolFileNames).map(([setting, name]) => [
      setting,
      path.join(directory, name),
    ]),
  );
  const report = path.join(directory, "caller-gtest-report.json");
  const files = [...Object.values(settings), report];
  for (const file of files) fs.writeFileSync(file, kCallerProtocolSentinel);
  return { settings, report, files };
}

/** Asserts that every file from writeCallerProtocolFiles still holds its sentinel. */
export function assertCallerProtocolFilesUnchanged(files) {
  for (const file of files) {
    assert.equal(
      fs.readFileSync(file, "utf8"),
      kCallerProtocolSentinel,
      `the comparator must not write ${path.basename(file)}`,
    );
  }
}

const kComparisonCase = {
  classname: "StandaloneGeodeBrowserPngCompare",
  name: "CanvasMatchesGolden",
  status: "RUN",
  result: "COMPLETED",
};

/** A report of one completed, passing comparison, as an earlier run would leave it. */
export const kCompletedReport = {
  tests: 1,
  failures: 0,
  disabled: 0,
  errors: 0,
  testsuites: [{ testsuite: [kComparisonCase] }],
};

/**
 * Writes a comparator that exits 0 without completing its comparison case. It writes every caller
 * test-protocol file whose setting reaches it, and the JSON report that an inherited GTEST_OUTPUT
 * names, so a leaked setting overwrites that caller file.
 */
function writeIncompleteComparator(directory, name, report) {
  const file = path.join(directory, `${name}.sh`);
  const lines = [
    "#!/bin/sh",
    ...Object.keys(kCallerProtocolFileNames).map((setting) =>
      `if [ -n "$${setting}" ]; then echo stub > "$${setting}"; fi`
    ),
    "case \"$GTEST_OUTPUT\" in json:*) echo stub > \"${GTEST_OUTPUT#json:}\" ;; esac",
  ];
  if (report) {
    fs.writeFileSync(`${file}.json`, JSON.stringify(report));
    lines.push(
      "for arg in \"$@\"; do",
      `  case "$arg" in --gtest_output=json:*) cp '${file}.json' "\${arg#*json:}" ;; esac`,
      "done",
    );
  }
  lines.push("exit 0", "");
  fs.writeFileSync(file, lines.join("\n"), { mode: 0o755 });
  return file;
}

/**
 * Writes into `directory` one comparator for each way of exiting 0 without completing the case,
 * with an `assert.throws` validator for the error the shared runner must raise for it.
 */
export function writeIncompleteComparators(directory) {
  const noReport = { message: /^bitmap comparator wrote no readable report \(/ };
  const incomplete = {
    message: /^bitmap comparator did not complete exactly one valid comparison:\n/,
  };
  return [
    {
      behavior: "exits without a report",
      comparator: writeIncompleteComparator(directory, "no-report", null),
      expectedError: noReport,
    },
    {
      behavior: "selects no case",
      comparator: writeIncompleteComparator(directory, "no-case", {
        tests: 0,
        failures: 0,
        disabled: 0,
        errors: 0,
        testsuites: [],
      }),
      expectedError: incomplete,
    },
    {
      behavior: "skips its case",
      comparator: writeIncompleteComparator(directory, "skipped-case", {
        ...kCompletedReport,
        testsuites: [{ testsuite: [{ ...kComparisonCase, result: "SKIPPED" }] }],
      }),
      expectedError: incomplete,
    },
  ];
}
