// Report-only lane for the Firefox cases quarantined by #1634.
//
// It runs the quarantined cases with the Firefox Geode resize project's browser settings, writes
// a JSON report for the job summary, and is only ever invoked with
// DONNER_BROWSER_QUARANTINE_REPORT=report by tools/run-browser-ci.sh on the scheduled Editor WASM
// run. Its exit code never fails that job.
const { defineConfig } = require("@playwright/test");
const compatibility = require("./playwright.compatibility.config.js");

const firefox = compatibility.projects.find((project) => project.name === "firefox-geode-resize");

/** The cases quarantined in Firefox by #1634; each still runs in Chromium where it applies. */
const kQuarantinedFirefoxCases = [
  /Geode Wasm View overlays render tile metadata and sparse Slug triangle edges/,
  /Firefox keeps Basic Shapes resize pixels and outline synchronized/,
  /Firefox keeps the dragged shape and its selection outline in every drag frame/,
];

/**
 * Cap on the whole lane, so the nightly job grows by at most this much even when every case runs
 * to its own timeout. Failure evidence is bounded inside it (failure-canvas-evidence.ts).
 */
const kReportLaneBudgetMs = 6 * 60_000;

module.exports = defineConfig({
  ...compatibility,
  globalTimeout: kReportLaneBudgetMs,
  reporter: [
    ["list"],
    ["json", { outputFile: process.env.DONNER_QUARANTINE_REPORT_JSON ?? "quarantine-report.json" }],
  ],
  projects: [
    {
      ...firefox,
      name: "firefox-quarantine-report",
      grep: kQuarantinedFirefoxCases,
    },
  ],
});
