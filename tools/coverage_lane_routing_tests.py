"""Pins coverage.yml's target-selection routing against a silent-skip regression.

The determination step decides WHAT to cover; the coverage jobs decide WHETHER
to run. Those used to be two independent expressions, and they drifted: a
`build_defs/*` change escalated to `fallback=true, reason=infra_change`, meaning
"cover everything", while every job's `if:` read that same state as "skip". The
composition produced zero coverage on exactly the changes most able to move
coverage, and it looked green because nothing ran.

These tests hold the routing to one shape:
  * the emitting side publishes a single `runs_coverage` decision,
  * every job gates on that output and re-derives nothing,
  * every reason the workflow can emit is classified by the routing table,
  * and a reason may resolve to "do not run" only if it is on an explicit,
    justified list.
"""

import re
import unittest

from python.runfiles import runfiles

# Reasons that produce no coverage run ANYWHERE, whatever the trigger. Both
# emit an EMPTY target set: there is genuinely nothing to measure, and running
# anyway builds an empty report and trips the report guards.
_NEVER_RUN_REASONS = frozenset(
    {
        # A genuine subset containing nothing instrumentable.
        "no_instrumentable_targets",
        # A genuine subset with instrumentable code but no test target to
        # produce profile data from. `bazel coverage` treats an empty test set
        # as an error, so running anyway is a guaranteed red with no report.
        "no_test_targets",
    }
)

# Reasons that resolve to a FULL target set. On a pull request these skip: the
# full run is the ~21-minute re-run this workflow exists to avoid, and the
# main-line baseline already covers those lines. Off a pull request they RUN,
# because off a pull request a full run IS the baseline. Adding to this list is
# a deliberate coverage-scope decision, which is the point: `build_defs/*` and
# `.bazelrc` reached this state by accident.
_FULL_FALLBACK_REASONS = frozenset(
    {
        # A root Bazel pin change: bazel-diff hashes the graph these files
        # define, so it cannot model them.
        "infra_change",
        # No PR context to diff against.
        "missing_pr_sha",
        "no_changed_files",
        # bazel-diff itself could not produce a subset. Fail open to the full
        # set.
        "head_checkout_failed",
        "base_checkout_failed",
        "base_hash_failed",
        "head_hash_failed",
        "impacted_targets_failed",
        "no_affected_targets",
        # The nightly full baseline, and the operator asking for that same
        # full picture now.
        "nightly_baseline",
        "manual_full",
        # A main push with no nightly baseline to diff against (none has run
        # yet, the run list was unreachable, its commit is not in this
        # checkout, or this commit IS the baseline). Running full re-cuts the
        # baseline on the spot rather than reporting a subset against a
        # baseline that may not exist.
        "no_nightly_baseline",
    }
)

# Reasons that produce an incremental coverage run on every trigger.
_INCREMENTAL_RUN_REASONS = frozenset(
    {"bazel_diff", "coverage_smoke", "full_test_label"}
)

_PR_SKIP_REASONS = _NEVER_RUN_REASONS | _FULL_FALLBACK_REASONS
_PR_RUN_REASONS = _INCREMENTAL_RUN_REASONS


def _workflow_text():
    resolver = runfiles.Create()
    path = resolver.Rlocation("donner/.github/workflows/coverage.yml")
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def _emitted_reasons(text):
    """Every reason literal passed to emit_targets, plus its fallback value."""
    emitted = set()
    for fallback, reason in re.findall(
        r"^\s*emit_targets\s+(true|false)\s+(\"?\$?\{?[A-Za-z_][A-Za-z0-9_]*\}?\"?)",
        text,
        re.MULTILINE,
    ):
        emitted.add((fallback, reason.strip('"')))
    return emitted


class CoverageLaneRoutingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = _workflow_text()

    def test_every_emitted_reason_is_classified(self):
        """No reason may exist that the routing table has never heard of.

        An unclassified reason is what a silent skip looks like before anyone
        notices: the step emits a target set, and the gate answers "no" for a
        reason nobody wrote down.
        """
        unknown = []
        for fallback, reason in _emitted_reasons(self.text):
            if reason.startswith("$"):
                # Indirect: `emit_targets true "$fallback_reason" ...`. The
                # literal reasons it can carry are asserted separately below.
                continue
            if reason not in _PR_SKIP_REASONS | _PR_RUN_REASONS:
                unknown.append((fallback, reason))
        self.assertEqual(
            [],
            sorted(unknown),
            "coverage.yml emits a fallback reason that tools/"
            "coverage_lane_routing_tests.py does not classify. Decide "
            "explicitly whether it should produce PR coverage and add it to "
            "_PR_RUN_REASONS or _PR_SKIP_REASONS.",
        )

    def test_indirect_reason_variable_only_carries_infra_change(self):
        """`$fallback_reason` must not become an untracked back door."""
        assignments = set(
            re.findall(r'fallback_reason="([a-z_]+)"', self.text)
        )
        assignments.discard("")
        self.assertEqual(
            {"infra_change"},
            assignments,
            "a new value is assigned to fallback_reason; classify it in the "
            "routing table above",
        )

    def test_routing_is_published_as_a_single_output(self):
        self.assertIn("runs_coverage: ${{ steps.determine.outputs.runs_coverage }}", self.text)
        self.assertIn("coverage_lane_runs()", self.text)
        self.assertIn('echo "runs_coverage=$runs"', self.text)

    def test_no_job_re_derives_the_routing_decision(self):
        """Job gates consume `runs_coverage`; they never restate the rule.

        Restating it is precisely how the two sides drifted apart.
        """
        gates = re.findall(r"^\s*if: \$\{\{(.*)\}\}\s*$", self.text, re.MULTILINE)
        offenders = [
            gate.strip()
            for gate in gates
            if "outputs.fallback_reason" in gate or "outputs.fallback " in gate
        ]
        self.assertEqual(
            [],
            offenders,
            "a job gate inspects fallback/fallback_reason directly instead of "
            "needs.determine-targets.outputs.runs_coverage",
        )

    def _job_body(self, job):
        """The YAML block for one job, up to the next job key at the same indent."""
        marker = "\n  %s:\n" % job
        self.assertIn(marker, self.text, "job %s not found" % job)
        rest = self.text.split(marker, 1)[1]
        end = re.search(r"^  [A-Za-z0-9_-]+:\s*$", rest, re.MULTILINE)
        return rest[: end.start()] if end else rest

    def test_every_coverage_job_gates_on_runs_coverage(self):
        """All three lanes, including the hosted one.

        `build` is the hosted lane used when trusted self-hosted routing is not
        available. It must consume the same coverage decision as the two
        self-hosted jobs so every selected lane measures the same target set.
        """
        for job in ("build", "coverage-self-hosted", "coverage-self-hosted-turnstile"):
            self.assertIn(
                "outputs.runs_coverage == 'true'",
                self._job_body(job),
                "job %s does not gate on the single routing output" % job,
            )

    def test_runner_gate_selects_exactly_one_coverage_lane(self):
        """The trusted-runner gate, not change size, selects the coverage lane.

        Hosted and self-hosted conditions must remain exact complements so a
        coverage decision cannot fall through both lane families.
        """
        hosted = self._job_body("build")
        self.assertIn("outputs.use_self_hosted_linux != 'true'", hosted)
        for job in ("coverage-self-hosted", "coverage-self-hosted-turnstile"):
            self.assertIn(
                "outputs.use_self_hosted_linux == 'true'",
                self._job_body(job),
            )

    def test_nightly_cuts_the_full_baseline(self):
        """The full run has exactly one scheduled owner.

        If the schedule trigger disappears, every main push still runs
        incrementally against a baseline that stops being refreshed, and the
        carried-forward numbers quietly age out. That failure is invisible in
        CI, so it is pinned here.
        """
        self.assertRegex(self.text, r"schedule:\n(?:\s*#.*\n)*\s*- cron: ")
        self.assertIn('if [[ "$EVENT_NAME" == "schedule" ]]; then', self.text)
        self.assertRegex(
            self.text,
            r'emit_targets true nightly_baseline "\$FULL_COVERAGE_TARGETS"',
        )

    def test_main_push_diffs_against_the_last_successful_nightly(self):
        """And nothing else: a wrong base silently changes what is measured."""
        self.assertIn("actions/workflows/coverage.yml/runs?event=schedule", self.text)
        self.assertIn("status=success", self.text)
        self.assertIn("branch=main", self.text)
        self.assertIn(".workflow_runs[0].head_sha", self.text)

    def test_main_push_without_a_baseline_runs_full(self):
        """Fail closed toward MORE coverage, never toward a stale baseline."""
        self.assertRegex(
            self.text,
            r'emit_targets true no_nightly_baseline "\$FULL_COVERAGE_TARGETS"',
        )
        # Every guard that clears the base SHA must land on that reason.
        self.assertIn('if [[ -z "$BASE_SHA" ]]; then', self.text)
        self.assertIn('if [[ "$BASE_SHA" == "$HEAD_SHA" ]]; then', self.text)

    def test_full_fallbacks_still_run_off_a_pull_request(self):
        """A main push whose diff cannot be scoped must not silently skip.

        The old gate answered "run" for every non-PR event, so this property
        held by accident. It is now a written branch, which means it can be
        deleted by accident, which is why it is asserted.
        """
        gate = self.text.split("coverage_lane_runs()", 1)[1].split("\n          }", 1)[0]
        self.assertIn('if [[ "$EVENT_NAME" == "pull_request" ]]; then', gate)
        self.assertRegex(gate, r'"\$EVENT_NAME" == "pull_request" \]\]; then\n\s*echo false')

    def test_nothing_to_measure_skips_on_every_trigger(self):
        """The empty-report guards are not PR-only.

        A main push can reach an affected set with no instrumentable code or no
        tests just as easily as a PR can, and coverage would die on the same
        guard.
        """
        gate = self.text.split("coverage_lane_runs()", 1)[1].split("\n          }", 1)[0]
        for reason in sorted(_NEVER_RUN_REASONS):
            self.assertIn(reason, gate)
        self.assertNotIn('if [[ "$EVENT_NAME" != "pull_request" ]]; then', gate)

    def test_variant_trim_stays_off_the_main_line(self):
        """Main incremental runs must measure what the nightly measured.

        Dropping variant wrappers on main would report backend-gated lines as
        uncovered on every merge that touches them, so main's graph would
        oscillate with the trim rather than with the code.
        """
        # The window between writing the untrimmed set and invoking the trim
        # tool is where the main line must leave.
        between = self.text.split('"$work_dir/affected-pretrim.txt"', 1)[1]
        between = between.split("python3 tools/coverage_variant_trim.py", 1)[0]
        self.assertIn('if [[ "$EVENT_NAME" != "pull_request" ]]; then', between)
        self.assertIn("emit_targets false bazel_diff", between)

    def test_build_defs_and_bazelrc_do_not_escalate_to_a_full_fallback(self):
        """The regression itself.

        A `build_defs/*` or `.bazelrc` change must reach bazel-diff and produce
        a real affected set. Escalating it to a full fallback made the coverage
        jobs skip, so a rules.bzl change - which can silently change what every
        variant target even is - got no coverage at all.
        """
        escalation = re.search(
            r"MODULE\.bazel\|[^)]*\)\n\s*echo \"Root Bazel pin change", self.text
        )
        self.assertIsNotNone(
            escalation,
            "the root-pin escalation case is not in its expected shape",
        )
        case_line = escalation.group(0)
        self.assertNotIn("build_defs/*", case_line)
        self.assertNotIn(".bazelrc", case_line)


if __name__ == "__main__":
    unittest.main()
