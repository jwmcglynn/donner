// Interaction latency gates and tracked completion checks of the browser responsiveness lane.
//
// Every run records each gate's measured value and each completion check's result. A gate miss
// fails the run unless the run sets DONNER_BROWSER_LATENCY_GATES=report, and a completion check
// miss fails it unless the run sets DONNER_BROWSER_COMPLETION_CHECKS=report and the miss is in a
// browser and phase that the check's report scope covers. Only the GitHub-hosted macOS Perf job
// sets either: the gates miss their targets on that runner (#1666), and the completion checks fail
// there (#1675, #1676, #2106).

import { kNominalCompletedResultHandoffLimitMs } from "./completed-result-handoff.mjs";

/** Environment variable that selects whether a gate miss fails the run. */
export const kLatencyGatesEnv = "DONNER_BROWSER_LATENCY_GATES";

/** The gates, their limits in milliseconds and the failure message an enforced miss reports. */
export const kLatencyGates = {
  settledClick: { limitMs: 100, message: "settled click feedback must meet100ms gate" },
  inputP95: { limitMs: 50, message: "input-to-completed-frame p95 must meet 50 ms gate" },
  inputStall: { limitMs: 250, message: "no input can stall above 250 ms" },
  completedStall: { limitMs: 250, message: "completed interaction cannot stall above 250 ms" },
  completedResultHandoff: {
    limitMs: kNominalCompletedResultHandoffLimitMs,
    message: "nominal carousel completed-result handoff must meet its 1000 ms bound",
  },
};

/** Line prefix of a recorded gate result in the test log. */
export const kLatencyGateRecordPrefix = "latency-gate ";

/** Environment variable that selects whether a completion check miss fails the run. */
export const kCompletionChecksEnv = "DONNER_BROWSER_COMPLETION_CHECKS";

/**
 * Report scope of every check of how much of the masked crown drag (the `geode-crown` phase)
 * completes while it is held. On the hosted macOS runner the Chromium drag-start result of that
 * drag alone can take longer than the 1.2 s hold, so the hold often completes no other result
 * (#2106), and Firefox renders held results but presents few of them (#2106, jointly with #1676).
 * The checks share this one scope in all four browser projects, so the job never enforces one view
 * of that slowness while it reports another.
 *
 * The trade-off: on the hosted job a held drag that makes no progress before its release is
 * reported, not failed, and cannot be told apart from this slowness until #2106 is fixed. The
 * hosted job still fails the drag on the held results' shape, a release that stalls past its
 * guard, a GPU tile readback, and, in Chromium, an input the stream never completed.
 *
 * Restore enforcement, by removing this scope from its checks, once the #2106 fix has landed and
 * five consecutive scheduled hosted Perf runs meet these checks in every project.
 */
const kMaskedCrownHeldDragProgress = Object.freeze({
  browsers: Object.freeze(["chromium", "firefox"]),
  phases: Object.freeze(["geode-crown"]),
  issue: "#2106",
});

/**
 * Hard checks of this lane: what each observes and, for a check the hosted macOS Perf job only
 * reports, its report scope: the browsers and, when `phases` is set, the phases whose misses the
 * scope's issue tracks. That job enforces the check in every other browser and phase, and every
 * run that does not set report mode enforces it everywhere; no CI job other than the hosted Perf
 * job runs the browser responsiveness targets, so those are manual runs. Checks that observe the
 * same thing share one scope, which browser_matrix_tests verifies, so the same slowness cannot be
 * reported through one check and enforced through another. The fix restores enforcement: remove a
 * scope once its issue is fixed.
 */
export const kCompletionChecks = {
  coldZoomPresents: {
    observes: "cold zoom presentation",
    report: Object.freeze({ browsers: Object.freeze(["chromium"]), issue: "#1675" }),
  },
  dragInputsComplete: {
    observes: "drag input completion",
    report: Object.freeze({ browsers: Object.freeze(["firefox"]), issue: "#1676" }),
  },
  heldDragPositions: { observes: "held drag progress", report: kMaskedCrownHeldDragProgress },
  heldDragFastPathResults: {
    observes: "held drag progress",
    report: kMaskedCrownHeldDragProgress,
  },
  // A slow-path or unpatched held result is a fast-path defect, never slowness: always enforced.
  heldMaskedDragDamagePatch: { observes: "held masked-child result shape" },
};

/**
 * Whether the report scope of a check covers a miss in this browser and phase.
 * @param {keyof typeof kCompletionChecks} check Check that missed.
 * @param {string} browserName Playwright browser name, such as "chromium".
 * @param {string} phase Measured phase of the miss.
 * @returns {boolean}
 */
export function completionCheckTracks(check, browserName, phase) {
  const { report } = kCompletionChecks[check];
  return report !== undefined && report.browsers.includes(browserName)
    && (report.phases === undefined || report.phases.includes(phase));
}

/** Line prefix of a recorded completion check result in the test log. */
export const kCompletionCheckRecordPrefix = "completion-check ";

/**
 * Whether a miss fails the run ("enforce", the default) or is only recorded ("report").
 * @param {string} variable Environment variable that selects the mode.
 * @param {Record<string, string | undefined>} env Environment to read.
 * @returns {"enforce" | "report"}
 */
function reportMode(variable, env) {
  const value = env[variable];
  if (value === undefined || value === "" || value === "enforce") return "enforce";
  if (value === "report") return "report";
  throw new Error(`${variable} must be "enforce" or "report", got "${value}"`);
}

/**
 * Whether a gate miss fails the run ("enforce", the default) or is only recorded ("report").
 * @param {Record<string, string | undefined>} env Environment to read.
 * @returns {"enforce" | "report"}
 */
export function latencyGateMode(env = process.env) {
  return reportMode(kLatencyGatesEnv, env);
}

/**
 * Whether a completion check miss fails the run ("enforce", the default) or is only recorded.
 * @param {Record<string, string | undefined>} env Environment to read.
 * @returns {"enforce" | "report"}
 */
export function completionCheckMode(env = process.env) {
  return reportMode(kCompletionChecksEnv, env);
}

/**
 * Run one completion check's assertion and record whether it held. A miss rethrows the assertion's
 * error unless the run reports completion checks and the check's report scope covers the miss.
 * @param {keyof typeof kCompletionChecks} check Check to run.
 * @param {() => unknown} assertion Throws, or returns a promise that rejects, on a miss.
 * @param {{
 *   browserName: string,
 *   project: string,
 *   phase: string,
 *   detail: string,
 *   mode: "enforce" | "report",
 *   log: (line: string) => void,
 * }} options
 * @returns {Promise<{ check: string, browser: string, phase: string, detail: string,
 *   met: boolean, mode: string, error?: string }>} The recorded result.
 */
export async function runCompletionCheck(check, assertion, options) {
  if (!Object.hasOwn(kCompletionChecks, check)) {
    throw new Error(`unknown completion check "${check}"`);
  }
  let failure;
  try {
    await assertion();
  } catch (error) {
    failure = error;
  }
  const result = {
    check,
    browser: options.project,
    phase: options.phase,
    detail: options.detail,
    met: failure === undefined,
    mode: options.mode === "report"
        && completionCheckTracks(check, options.browserName, options.phase)
      ? "report"
      : "enforce",
  };
  if (failure !== undefined) {
    result.error = String(failure instanceof Error ? failure.message : failure).split("\n")[0];
  }
  options.log(`${kCompletionCheckRecordPrefix}${JSON.stringify(result)}`);
  if (failure !== undefined && result.mode === "enforce") throw failure;
  return result;
}

/**
 * Record one gate's measured value and, in "enforce" mode, check it through \p enforce.
 * @param {keyof typeof kLatencyGates} gate Gate to check.
 * @param {number | null | undefined} valueMs Measured value; a missing value misses the gate.
 * @param {{
 *   browser: string,
 *   phase: string,
 *   mode: "enforce" | "report",
 *   enforce: (valueMs: number | null | undefined, limitMs: number, message: string) => void,
 *   log: (line: string) => void,
 * }} options
 * @returns {{ gate: string, browser: string, phase: string, valueMs: number | null,
 *   limitMs: number, met: boolean, mode: string }} The recorded result.
 */
export function checkLatencyGate(gate, valueMs, options) {
  const { limitMs, message } = kLatencyGates[gate];
  const result = {
    gate,
    browser: options.browser,
    phase: options.phase,
    valueMs: typeof valueMs === "number" ? valueMs : null,
    limitMs,
    met: typeof valueMs === "number" && valueMs <= limitMs,
    mode: options.mode,
  };
  options.log(`${kLatencyGateRecordPrefix}${JSON.stringify(result)}`);
  if (options.mode === "enforce") options.enforce(valueMs, limitMs, message);
  return result;
}

/**
 * Markdown tables of the gate and completion check results recorded in a test log.
 * @param {string} log Test log text.
 * @returns {string}
 */
export function latencyGateSummary(log) {
  const records = (prefix) =>
    log.split("\n").flatMap((line) => {
      const start = line.indexOf(prefix);
      if (start < 0) return [];
      try {
        return [JSON.parse(line.slice(start + prefix.length))];
      } catch {
        return [];
      }
    });
  const outcome = (result) =>
    result.met ? "met" : result.mode === "report" ? "missed (reported)" : "missed";
  const table = (title, header, rows) => [
    `### ${title}`,
    "",
    `| ${header.join(" | ")} |`,
    `| ${header.map(() => "---").join(" | ")} |`,
    ...rows.map((cells) => `| ${cells.join(" | ")} |`),
    "",
  ];
  const gates = records(kLatencyGateRecordPrefix);
  const checks = records(kCompletionCheckRecordPrefix);
  if (gates.length === 0 && checks.length === 0) return "No latency gate results were recorded.\n";
  const lines = [];
  if (gates.length > 0) {
    lines.push(...table(
      "Interaction latency gates",
      ["Browser", "Phase", "Gate", "Measured ms", "Limit ms", "Result"],
      gates.map((result) => [
        result.browser,
        result.phase,
        result.gate,
        typeof result.valueMs === "number" ? result.valueMs.toFixed(1) : "missing",
        result.limitMs,
        outcome(result),
      ]),
    ));
  }
  if (checks.length > 0) {
    lines.push(...table(
      "Completion checks",
      ["Browser", "Phase", "Check", "Detail", "Result"],
      checks.map((result) => [
        result.browser,
        result.phase,
        result.check,
        result.detail,
        outcome(result),
      ]),
    ));
  }
  return lines.join("\n");
}
