import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { join } from "node:path";

import { runBitmapComparator } from "./browser-bitmap-comparator";
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
  const sideBySide = join(outputDir, "side_by_side_expected.png");
  writeFileSync(actual, normalizeOverlayGeneration(captured));
  writeFileSync(expected, normalizeOverlayGeneration(readFileSync(committedGolden)));
  for (const path of [failureActual, failureExpected, diff, sideBySide]) {
    rmSync(path, { force: true });
  }

  const env: NodeJS.ProcessEnv = {
    ...inheritedEnvironment,
    TEST_UNDECLARED_OUTPUTS_DIR: outputDir,
  };
  delete env.UPDATE_GOLDEN_IMAGES_DIR;
  const { matched, detail } = runBitmapComparator({
    executable,
    comparison: {
      DONNER_ACTUAL_PNG: "actual-input.png",
      DONNER_GOLDEN_PNG: "expected.png",
      DONNER_BROWSER_COMPARE_MODE: "overlay",
    },
    reportPath: join(outputDir, "gtest-result.json"),
    cwd: outputDir,
    inheritedEnvironment: env,
  });
  if (!matched && ![failureActual, failureExpected, diff].every(existsSync)) {
    throw new Error(`overlay bitmap comparator did not write its mismatch artifacts:\n${detail}`);
  }
  return {
    matched,
    outputDir,
    actual,
    expected,
    diff,
    failureActual,
    failureExpected,
    detail,
  };
}
