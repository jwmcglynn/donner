/**
 * Report-only runs of quarantined browser cases.
 *
 * A quarantined case is skipped on pull request and push CI so main stays green while its issue
 * is open, but a skipped case stops producing evidence. The scheduled Editor WASM run lifts the
 * quarantine for one report-only lane: the cases run, each outcome is recorded in the job summary
 * and the evidence is uploaded, and a failure never fails the job.
 *
 * This module is loaded both by Playwright specs (as CommonJS) and by node:test, so it must not
 * use `import.meta`; the summary command lives in quarantine-report-summary.mjs.
 */

/** Selects whether quarantined cases are skipped ("skip", the default) or run ("report"). */
export const kQuarantineReportEnv = "DONNER_BROWSER_QUARANTINE_REPORT";

/**
 * Read the quarantine mode.
 *
 * @param {Record<string, string | undefined>} env Process environment.
 * @returns {"skip" | "report"}
 */
export function quarantineMode(env = process.env) {
  const value = env[kQuarantineReportEnv];
  if (value === undefined || value === "" || value === "skip") return "skip";
  if (value === "report") return "report";
  throw new Error(`${kQuarantineReportEnv} must be "skip" or "report", got "${value}"`);
}

/**
 * Whether a quarantined case should still be skipped in this run.
 *
 * @param {Record<string, string | undefined>} env Process environment.
 * @returns {boolean}
 */
export function quarantineStillSkips(env = process.env) {
  return quarantineMode(env) === "skip";
}

function collectTests(suite, out) {
  for (const spec of suite.specs ?? []) {
    for (const entry of spec.tests ?? []) {
      out.push({ title: spec.title, file: spec.file ?? suite.file ?? "", entry });
    }
  }
  for (const child of suite.suites ?? []) collectTests(child, out);
  return out;
}

function firstLine(text) {
  return String(text ?? "").split("\n").find((line) => line.trim() !== "")?.trim() ?? "";
}

function markdownCell(text) {
  return text.replaceAll("|", "\\|").replaceAll("`", "'");
}

/**
 * Summarize a Playwright JSON report of the report-only lane as Markdown for the job summary.
 *
 * Every case is listed with its outcome: "passed", "failed (reported)" with the first line of its
 * first error and the names of its attachments, "skipped", or another Playwright status verbatim.
 *
 * @param {object | null} report Parsed Playwright JSON report, or null when the lane wrote none.
 * @returns {string}
 */
export function quarantineReportSummary(report) {
  const heading = "### Quarantined cases (report only)\n\n";
  if (report === null || report === undefined) {
    return `${heading}The report-only lane wrote no results.\n`;
  }
  const tests = [];
  for (const suite of report.suites ?? []) collectTests(suite, tests);
  if (tests.length === 0) return `${heading}The report-only lane ran no cases.\n`;
  const rows = tests.map(({ title, file, entry }) => {
    const results = entry.results ?? [];
    const last = results.at(-1);
    const status = last?.status ?? entry.status ?? "not run";
    let outcome = status;
    let detail = "";
    if (status === "passed") {
      outcome = "passed";
    } else if (status === "skipped") {
      outcome = "skipped";
    } else if (last !== undefined) {
      outcome = `${status} (reported)`;
      const error = (last.errors ?? [])[0] ?? last.error;
      const attachments = (last.attachments ?? []).map((attachment) => attachment.name);
      detail = [
        firstLine(error?.message),
        attachments.length > 0 ? `evidence: ${attachments.join(", ")}` : "",
      ].filter((part) => part !== "").join("; ");
    }
    return `| ${markdownCell(title)} | ${markdownCell(file)} | ${outcome} | ${markdownCell(detail)} |`;
  });
  return `${heading}| Case | Spec | Outcome | Detail |\n| --- | --- | --- | --- |\n${rows.join("\n")}\n`;
}
