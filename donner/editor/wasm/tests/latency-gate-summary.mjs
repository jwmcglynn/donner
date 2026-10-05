// `node latency-gate-summary.mjs <test.log>` prints the interaction latency gates and completion
// checks recorded in a test log as Markdown tables, or a notice when the log is missing because the
// test did not run.
//
// This command lives apart from latency-gates.mjs because the responsiveness spec imports that
// module, which must therefore stay loadable as CommonJS.

import fs from "node:fs";
import { latencyGateSummary } from "./latency-gates.mjs";

const file = process.argv[2];
process.stdout.write(
  latencyGateSummary(file && fs.existsSync(file) ? fs.readFileSync(file, "utf8") : ""),
);
