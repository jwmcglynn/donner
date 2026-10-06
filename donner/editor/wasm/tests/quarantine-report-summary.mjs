// `node quarantine-report-summary.mjs <report.json>` prints the report-only lane's quarantined
// cases as a Markdown table, or a notice when the lane wrote no report.
//
// This command lives apart from quarantine-report.mjs because the Playwright specs import that
// module, which must therefore stay loadable as CommonJS.

import fs from "node:fs";
import { quarantineReportSummary } from "./quarantine-report.mjs";

const file = process.argv[2];
let report = null;
if (file && fs.existsSync(file)) {
  try {
    report = JSON.parse(fs.readFileSync(file, "utf8"));
  } catch (error) {
    process.stdout.write(`### Quarantined cases (report only)\n\nUnreadable report: ${error}\n`);
    process.exit(0);
  }
}
process.stdout.write(quarantineReportSummary(report));
