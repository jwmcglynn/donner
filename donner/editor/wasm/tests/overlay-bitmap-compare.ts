import { spawnSync } from "node:child_process";
import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
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

/** Compare browser PNGs through Donner's native bitmap_golden_compare executable. */
export function compareOverlayBitmap(
  captured: Buffer,
  committedGolden: string,
  outputDir: string,
): OverlayBitmapComparison {
  const executable = process.env.DONNER_BROWSER_GOLDEN_COMPARE;
  if (!executable) {
    throw new Error("DONNER_BROWSER_GOLDEN_COMPARE is required for overlay pixels");
  }
  mkdirSync(outputDir, { recursive: true });
  const actual = join(outputDir, "actual-input.png");
  const expected = join(outputDir, "expected.png");
  const diff = join(outputDir, "diff_expected.png");
  writeFileSync(actual, normalizeOverlayGeneration(captured));
  writeFileSync(expected, normalizeOverlayGeneration(readFileSync(committedGolden)));

  const env = {
    ...process.env,
    DONNER_ACTUAL_PNG: "actual-input.png",
    DONNER_GOLDEN_PNG: "expected.png",
    TEST_UNDECLARED_OUTPUTS_DIR: outputDir,
  };
  delete env.UPDATE_GOLDEN_IMAGES_DIR;
  const result = spawnSync(resolve(executable), [], {
    cwd: outputDir,
    env,
    encoding: "utf8",
    timeout: 10_000,
    maxBuffer: 1024 * 1024,
  });
  if (result.error || result.signal || (result.status !== 0 && result.status !== 1)) {
    throw new Error("overlay bitmap comparator could not run: "
      + String(result.error ?? result.signal ?? result.status));
  }
  return {
    matched: result.status === 0,
    outputDir,
    actual,
    expected,
    diff,
    failureActual: join(outputDir, "actual_expected.png"),
    failureExpected: join(outputDir, "expected_expected.png"),
    detail: (result.stdout + result.stderr).trim(),
  };
}
