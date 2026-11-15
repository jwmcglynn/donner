// `node latency-gate-summary.mjs [--require-all] <test.log>...` prints the interaction latency
// gates and completion checks recorded in the test logs as Markdown tables. Each responsiveness
// project is its own test, so the summary reads all of their logs. A missing log means that
// project's test did not run: the summary names it on its own line, and with --require-all also
// exits nonzero, so a passing test step cannot hide a project that never ran.
//
// This command lives apart from latency-gates.mjs because the responsiveness spec imports that
// module, which must therefore stay loadable as CommonJS.

import fs from "node:fs";
import path from "node:path";
import { latencyGateSummary } from "./latency-gates.mjs";

const args = process.argv.slice(2);
const requireAll = args[0] === "--require-all";
const files = requireAll ? args.slice(1) : args;
const missing = files.filter((file) => !fs.existsSync(file));
const logs = files
  .filter((file) => fs.existsSync(file))
  .map((file) => fs.readFileSync(file, "utf8"));
let summary = latencyGateSummary(logs.join("\n"));
for (const file of missing) {
  // A Bazel test log is <testlogs>/<package>/<target>/test.log, so its directory names the target.
  summary += `Missing test log for ${path.basename(path.dirname(file))}: that test did not run.\n`;
}
process.stdout.write(summary);
if (requireAll && missing.length > 0) process.exitCode = 1;
