import path from "node:path";
import { runBitmapComparator } from "./browser-bitmap-comparator";

/** Compare the saved pixel samples after GPU transport has completed. */
export function compareGpuTransferSamples(
  samples: Iterable<Record<string, unknown>>,
  outputDirectory: string,
  inheritedEnvironment: NodeJS.ProcessEnv = process.env,
): void {
  const comparator = inheritedEnvironment.DONNER_BROWSER_GOLDEN_COMPARE;
  if (!comparator) throw new Error("Bazel must provide the shared pixelmatch comparator");
  const comparatorEnvironment = { ...inheritedEnvironment };
  delete comparatorEnvironment.UPDATE_GOLDEN_IMAGES_DIR;
  delete comparatorEnvironment.DONNER_BROWSER_COMPARE_MODE;
  for (const sample of samples) {
    if (!sample.actualPath || !sample.expectedPath) continue;
    const { matched, detail } = runBitmapComparator({
      executable: comparator,
      comparison: {
        DONNER_ACTUAL_PNG: String(sample.actualPath),
        DONNER_GOLDEN_PNG: String(sample.expectedPath),
      },
      reportPath: path.join(outputDirectory, `${sample.sequence}-compare.json`),
      inheritedEnvironment: comparatorEnvironment,
    });
    if (!matched) throw new Error(detail);
    sample.pixelChecked = true;
  }
}
