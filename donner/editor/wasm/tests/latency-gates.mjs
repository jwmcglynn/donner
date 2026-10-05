// Interaction latency gates of the browser responsiveness lane.
//
// Every run records each gate's measured value. A miss fails the run unless the run sets
// DONNER_BROWSER_LATENCY_GATES=report, which only the GitHub-hosted macOS Perf job does: the gates
// miss their targets on that runner (#1666), so that job reports them without failing.

/** Environment variable that selects whether a gate miss fails the run. */
export const kLatencyGatesEnv = "DONNER_BROWSER_LATENCY_GATES";

/** The gates, their limits in milliseconds and the failure message an enforced miss reports. */
export const kLatencyGates = {
  settledClick: { limitMs: 100, message: "settled click feedback must meet100ms gate" },
  inputP95: { limitMs: 50, message: "input-to-completed-frame p95 must meet 50 ms gate" },
  inputStall: { limitMs: 250, message: "no input can stall above 250 ms" },
  completedStall: { limitMs: 250, message: "completed interaction cannot stall above 250 ms" },
};

/** Line prefix of a recorded gate result in the test log. */
export const kLatencyGateRecordPrefix = "latency-gate ";

/**
 * Whether a gate miss fails the run ("enforce", the default) or is only recorded ("report").
 * @param {Record<string, string | undefined>} env Environment to read.
 * @returns {"enforce" | "report"}
 */
export function latencyGateMode(env = process.env) {
  const value = env[kLatencyGatesEnv];
  if (value === undefined || value === "" || value === "enforce") return "enforce";
  if (value === "report") return "report";
  throw new Error(`${kLatencyGatesEnv} must be "enforce" or "report", got "${value}"`);
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
 * Markdown table of the gate results recorded in a test log.
 * @param {string} log Test log text.
 * @returns {string}
 */
export function latencyGateSummary(log) {
  const results = log.split("\n").flatMap((line) => {
    const start = line.indexOf(kLatencyGateRecordPrefix);
    if (start < 0) return [];
    try {
      return [JSON.parse(line.slice(start + kLatencyGateRecordPrefix.length))];
    } catch {
      return [];
    }
  });
  if (results.length === 0) return "No latency gate results were recorded.\n";
  const rows = results.map((result) => {
    const value = typeof result.valueMs === "number" ? result.valueMs.toFixed(1) : "missing";
    const outcome = result.met ? "met" : result.mode === "report" ? "missed (reported)" : "missed";
    const cells = [result.browser, result.phase, result.gate, value, result.limitMs, outcome];
    return `| ${cells.join(" | ")} |`;
  });
  return [
    "### Interaction latency gates",
    "",
    "| Browser | Phase | Gate | Measured ms | Limit ms | Result |",
    "| --- | --- | --- | --- | --- | --- |",
    ...rows,
    "",
  ].join("\n");
}
