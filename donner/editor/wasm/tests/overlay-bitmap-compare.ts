import { spawnSync } from "node:child_process";
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { join, resolve } from "node:path";

import { normalizeOverlayGeneration } from "./png-crop";

export interface OverlayBitmapComparison {
  matched: boolean;
  outputDir: string;
  actual: string;
  expected: string;
  diff: string;
  failureActual: string;
  failureExpected: string;
  detail: string;
}

function readComparisonFailureCount(reportPath: string): number {
  // Exit zero is insufficient when an inherited GTest filter or shard silently runs no cases.
  const report = JSON.parse(readFileSync(reportPath, "utf8"));
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
    || onlyCase?.classname !== "StandaloneGeodeBrowserPngCompare"
    || onlyCase?.name !== "CanvasMatchesGolden"
    || onlyCase?.status !== "RUN" || onlyCase?.result !== "COMPLETED"
    || report.failures !== (caseFailures > 0 ? 1 : 0)
  ) {
    throw new Error("overlay bitmap comparator did not complete exactly one valid comparison");
  }
  return caseFailures;
}

/** Compare browser PNGs through Donner's native bitmap_golden_compare executable. */
export function compareOverlayBitmap(
  captured: Buffer,
  committedGolden: string,
  outputDir: string,
  inheritedEnvironment: NodeJS.ProcessEnv = process.env,
): OverlayBitmapComparison {
  const executable = inheritedEnvironment.DONNER_BROWSER_GOLDEN_COMPARE;
  if (!executable) {
    throw new Error("DONNER_BROWSER_GOLDEN_COMPARE is required for overlay pixels");
  }
  mkdirSync(outputDir, { recursive: true });
  const actual = join(outputDir, "actual-input.png");
  const expected = join(outputDir, "expected.png");
  const diff = join(outputDir, "diff_expected.png");
  const failureActual = join(outputDir, "actual_expected.png");
  const failureExpected = join(outputDir, "expected_expected.png");
  const reportPath = join(outputDir, "gtest-result.json");
  const sideBySide = join(outputDir, "side_by_side_expected.png");
  writeFileSync(actual, normalizeOverlayGeneration(captured));
  writeFileSync(expected, normalizeOverlayGeneration(readFileSync(committedGolden)));
  for (const path of [reportPath, failureActual, failureExpected, diff, sideBySide]) {
    rmSync(path, { force: true });
  }

  const env: NodeJS.ProcessEnv = {
    ...inheritedEnvironment,
    DONNER_ACTUAL_PNG: "actual-input.png",
    DONNER_GOLDEN_PNG: "expected.png",
    DONNER_BROWSER_COMPARE_MODE: "overlay",
    TEST_UNDECLARED_OUTPUTS_DIR: outputDir,
  };
  for (const name of Object.keys(env)) {
    if (name.startsWith("GTEST_") || name.startsWith("TESTBRIDGE_")) delete env[name];
  }
  delete env.UPDATE_GOLDEN_IMAGES_DIR;
  const result = spawnSync(resolve(executable), [
    "--gtest_filter=StandaloneGeodeBrowserPngCompare.CanvasMatchesGolden",
    "--gtest_repeat=1",
    "--gtest_list_tests=0",
    "--gtest_shuffle=0",
    "--gtest_output=json:gtest-result.json",
  ], {
    cwd: outputDir,
    env,
    encoding: "utf8",
    timeout: 10_000,
    maxBuffer: 1024 * 1024,
  });
  if (result.error || result.signal || (result.status !== 0 && result.status !== 1)) {
    throw new Error(
      "overlay bitmap comparator could not run: "
        + String(result.error ?? result.signal ?? result.status),
    );
  }

  const caseFailures = readComparisonFailureCount(reportPath);
  if (
    (result.status === 0 && caseFailures !== 0)
    || (result.status === 1 && (caseFailures < 1
      || ![failureActual, failureExpected, diff].every(existsSync)))
  ) {
    throw new Error("overlay bitmap comparator did not complete exactly one valid comparison");
  }
  return {
    matched: result.status === 0,
    outputDir,
    actual,
    expected,
    diff,
    failureActual,
    failureExpected,
    detail: (result.stdout + result.stderr).trim(),
  };
}
