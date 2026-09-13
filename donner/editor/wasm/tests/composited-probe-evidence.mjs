import { writeFile } from "node:fs/promises";

/**
 * Stop sampling and preserve the collected window before any assertion.
 * @param {import("@playwright/test").Page} page
 * @param {import("@playwright/test").TestInfo} testInfo
 * @param {unknown} gesture
 * @returns {Promise<import("./composited-probe.ts").CompositedProbeResult>}
 */
export async function stopCompositedProbe(page, testInfo, gesture) {
  const result = await page.evaluate(() => {
    const probe = window.__donnerCompositedProbe;
    if (probe === undefined) {
      throw new Error("composited probe: install before stop");
    }
    probe.stop();
    return {
      samples: probe.samples.slice(),
      drawFailures: probe.drawFailures,
      frames: probe.frames,
      readbackRetries: probe.readbackRetries,
      readbackRescues: probe.readbackRescues,
    };
  });
  const evidencePath = testInfo.outputPath("composited-probe.json");
  await writeFile(evidencePath, JSON.stringify({ gesture, result }), "utf8");
  await testInfo.attach("composited-probe", {
    path: evidencePath,
    contentType: "application/json",
  });
  return result;
}
