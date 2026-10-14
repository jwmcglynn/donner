import { spawnSync } from "node:child_process";
import { readFileSync, rmSync } from "node:fs";
import { resolve } from "node:path";

const kSuite = "StandaloneGeodeBrowserPngCompare";
const kCase = "CanvasMatchesGolden";

// The comparator's inputs come only from the request.
const kComparatorInputs = [
  "DONNER_ACTUAL_PNG",
  "DONNER_BROWSER_COMPARE_MODE",
  "DONNER_EXPECTED_PNG",
  "DONNER_GOLDEN_PNG",
];
// googletest writes, creates or deletes these files for the test that owns them: the caller.
const kCallerTestFiles = [
  "TEST_PREMATURE_EXIT_FILE",
  "TEST_WARNINGS_OUTPUT_FILE",
  "XML_OUTPUT_FILE",
];

export interface BitmapComparatorRequest {
  /** The native comparator, //donner/editor/tests:standalone_geode_browser_png_compare. */
  executable: string;
  /** The comparator's DONNER_* inputs for this comparison. */
  comparison: Record<string, string>;
  /** Where the comparator writes its JSON report; any earlier report is removed first. */
  reportPath: string;
  /** Working directory that relative image paths resolve against; the caller's by default. */
  cwd?: string;
  /**
   * Environment the comparator starts from, `process.env` by default. Its googletest settings,
   * its comparator inputs and the caller's test files are removed before `comparison` applies.
   */
  inheritedEnvironment?: NodeJS.ProcessEnv;
}

export interface BitmapComparatorResult {
  /** Whether the comparison passed. */
  matched: boolean;
  /** The comparator's output, which names its actual, expected and diff PNGs on a mismatch. */
  detail: string;
}

function readComparisonFailureCount(reportPath: string, output: string): number {
  // Exit zero is insufficient: an unknown flag prints help and exits zero, and an inherited
  // filter or shard can select no case.
  let report;
  try {
    report = JSON.parse(readFileSync(reportPath, "utf8"));
  } catch (error) {
    throw new Error(`bitmap comparator wrote no readable report (${String(error)}):\n${output}`);
  }
  const suites = Array.isArray(report.testsuites) ? report.testsuites : [];
  const cases = suites.flatMap((suite: { testsuite?: unknown[] }) => suite.testsuite ?? []);
  const onlyCase = cases[0] as {
    classname?: string;
    name?: string;
    status?: string;
    result?: string;
    failures?: unknown[];
  } | undefined;
  const caseFailures = onlyCase?.failures?.length ?? 0;
  if (
    report.tests !== 1 || report.disabled !== 0 || report.errors !== 0
    || suites.length !== 1 || cases.length !== 1
    || onlyCase?.classname !== kSuite
    || onlyCase?.name !== kCase
    || onlyCase?.status !== "RUN" || onlyCase?.result !== "COMPLETED"
    || report.failures !== (caseFailures > 0 ? 1 : 0)
  ) {
    throw new Error(`bitmap comparator did not complete exactly one valid comparison:\n${output}`);
  }
  return caseFailures;
}

/**
 * Run one comparison through Donner's native pixelmatch comparator and confirm from its JSON
 * report that it completed its one comparison case. Throws, with the comparator's output, when the
 * comparator could not run or did not complete that case.
 */
export function runBitmapComparator(request: BitmapComparatorRequest): BitmapComparatorResult {
  const env: NodeJS.ProcessEnv = { ...(request.inheritedEnvironment ?? process.env) };
  for (const name of Object.keys(env)) {
    if (name.startsWith("GTEST_") || name.startsWith("TESTBRIDGE_")) delete env[name];
  }
  for (const name of [...kComparatorInputs, ...kCallerTestFiles]) delete env[name];
  const reportPath = resolve(request.reportPath);
  rmSync(reportPath, { force: true });
  const result = spawnSync(resolve(request.executable), [
    `--gtest_filter=${kSuite}.${kCase}`,
    "--gtest_fail_if_no_test_selected",
    "--gtest_repeat=1",
    "--gtest_list_tests=0",
    "--gtest_shuffle=0",
    `--gtest_output=json:${reportPath}`,
  ], {
    cwd: request.cwd,
    env: { ...env, ...request.comparison },
    encoding: "utf8",
    timeout: 10_000,
    maxBuffer: 1024 * 1024,
  });
  const output = `${result.stdout ?? ""}${result.stderr ?? ""}`.trim();
  if (result.error || result.signal || (result.status !== 0 && result.status !== 1)) {
    throw new Error(
      `bitmap comparator could not run: ${String(result.error ?? result.signal ?? result.status)}`
        + `:\n${output}`,
    );
  }
  const caseFailures = readComparisonFailureCount(reportPath, output);
  if ((result.status === 0) !== (caseFailures === 0)) {
    throw new Error(`bitmap comparator exit status disagrees with its report:\n${output}`);
  }
  return { matched: result.status === 0, detail: output };
}
