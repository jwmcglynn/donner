// Runs the owner abort-site probe under Node and prints the abort records its main thread kept.
//
// The module's error console is stderr. The editor's abort hook forwards each pthread record to
// the main thread, which keeps it as the editor page does; at exit each kept record is written to
// stdout as `RECORD <json>`.
//
// Usage: node owner-abort-site-probe-driver.mjs <probe.js> <mode>
// The runtime passes these arguments to main(), which reads the mode from the last one.

import { createRequire } from "node:module";

process.on("exit", () => {
  for (const record of globalThis.__donnerAbortRecords ?? []) {
    process.stdout.write(`RECORD ${JSON.stringify(record)}\n`);
  }
});
createRequire(import.meta.url)(process.argv[2]);
