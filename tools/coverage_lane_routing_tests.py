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

# Reasons that intentionally produce no coverage run on a pull request. Adding
# to this list is a deliberate coverage-scope decision, which is the point:
# `build_defs/*` and `.bazelrc` reached this state by accident.
_PR_SKIP_REASONS = frozenset(
    {
        # A root Bazel pin change: bazel-diff hashes the graph these files
        # define, so it cannot model them. The main-push baseline covers it.
        "infra_change",
        # No PR context to diff against.
        "non_pr",
        "missing_pr_sha",
        "no_changed_files",
        # bazel-diff itself could not produce a subset. Fail open to the full
        # set, which on a PR means the main-push baseline covers it.
        "head_checkout_failed",
        "base_checkout_failed",
        "base_hash_failed",
        "head_hash_failed",
        "impacted_targets_failed",
        "no_affected_targets",
        # A genuine subset containing nothing instrumentable. There is no
        # coverage to measure, and running anyway trips the empty-report guard.
        "no_instrumentable_targets",
    }
)

# Reasons that DO produce a PR coverage run.
_PR_RUN_REASONS = frozenset({"bazel_diff", "coverage_smoke", "full_test_label"})


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
