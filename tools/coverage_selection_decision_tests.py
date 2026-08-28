"""Replays the coverage lane's three target-selection failures.

The coverage lane picks a PR target subset and then decides whether that subset
is worth instrumenting. Three production failures came from taking that decision
at three different pipeline stages, each about a DIFFERENT set than the one
`bazel coverage` would actually run:

  1. A subset whose surviving tests were only a py_test passed an
     instrumentability check taken on the PRE-narrowing set, where a
     manual-tagged cc_test made it look instrumentable. The coverage command's
     own --test_tag_filters then removed that cc_test.
  2. A subset of macOS-only Metal cc_tests passed a host-compatibility recheck
     scoped to cc_binary-only sets, on the assumption that a cc_test is always
     host code.
  3. A subset of two macOS-only playwright wrapper tests passed BOTH earlier
     fixes: the kind query reported the unknown kind `js_test` (fail-open) plus
     a host-compatible `directory_path` helper that is not a test, the
     host-compatibility cquery ran on the pre-narrowing set where that helper
     kept the verdict positive, and the later surviving-set recheck re-asked
     only the KIND question and discarded the host-compatibility answer.

All three end the same way: coverage runs, every target is dropped or produces
no data, and the lane dies on an empty/absent report that no rerun can clear.

These tests exercise the CONSOLIDATED decision as it exists in coverage.yml --
the shell function is extracted from the workflow and run against the real
classifier, with `bazelisk cquery` stubbed to return each incident's recorded
compatibility answer. They also pin the shape that makes a fourth sibling
impossible: exactly one classifier call, exactly one cquery, both after every
narrowing step.
"""

import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import textwrap
import unittest

from python.runfiles import runfiles

_BEGIN = "# BEGIN coverage final-list decision"
_END = "# END coverage final-list decision"

_STUB_BAZELISK = """#!/bin/bash
# Minimal `bazelisk` stand-in: the decision block only ever runs one cquery.
if [[ "${1:-}" == "cquery" ]]; then
  if [[ "${STUB_CQUERY_FAIL:-}" == "1" ]]; then
    echo "stub: cquery failed" >&2
    exit 1
  fi
  cat "$STUB_CQUERY_OUTPUT"
  exit 0
fi
echo "stub: unexpected bazelisk invocation: $*" >&2
exit 1
"""


def _runfile(path):
    resolver = runfiles.Create()
    return resolver.Rlocation("donner/%s" % path)


def _read(path):
    with open(_runfile(path), encoding="utf-8") as handle:
        return handle.read()


class CoverageSelectionDecisionTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.workflow = _read(".github/workflows/coverage.yml")
        cls.decision = cls._extract_decision(cls.workflow)
        cls.classifier = _runfile("tools/coverage_instrumentable_targets.py")

    @staticmethod
    def _extract_decision(workflow):
        match = re.search(
            r"^[ \t]*%s[^\n]*\n(?P<body>.*?)^[ \t]*%s[ \t]*$"
            % (re.escape(_BEGIN), re.escape(_END)),
            workflow,
            re.MULTILINE | re.DOTALL,
        )
        assert match is not None, "decision block markers not found in coverage.yml"
        return textwrap.dedent(match.group("body"))

    def _decide(self, label_kinds, final_targets, host_compat, cquery_fails=False):
        """Run the extracted decision block and return its verdict.

        Args:
            label_kinds: `bazel query --output label_kind` lines for the
                alias-resolved affected set.
            final_targets: The final coverage target list, one label per entry.
            host_compat: Lines the host-compatibility cquery reports.
            cquery_fails: Make the stubbed cquery exit nonzero instead.

        Returns:
            The verdict word ("skip" or "run").
        """
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / "tools").mkdir()
            shutil.copy(self.classifier, root / "tools")
            bin_dir = root / "bin"
            bin_dir.mkdir()
            stub = bin_dir / "bazelisk"
            stub.write_text(_STUB_BAZELISK, encoding="utf-8")
            stub.chmod(0o755)

            scratch = root / "scratch"
            scratch.mkdir()
            diagnostics = root / "diagnostics"
            diagnostics.mkdir()

            kinds_file = scratch / "affected-label-kind.txt"
            kinds_file.write_text(
                "".join(f"{line}\n" for line in label_kinds), encoding="utf-8"
            )
            final_file = scratch / "final-targets.txt"
            final_file.write_text(
                "".join(f"{label}\n" for label in final_targets), encoding="utf-8"
            )
            compat_file = root / "cquery-output.txt"
            compat_file.write_text(
                "".join(f"{line}\n" for line in host_compat), encoding="utf-8"
            )

            fixture = root / "decide.sh"
            fixture.write_text(
                "#!/bin/bash\nset -euo pipefail\n"
                + self.decision
                + '\ncoverage_final_decision "$1" "$2" "$3" "$4" "$5"\n',
                encoding="utf-8",
            )
            fixture.chmod(0o755)

            env = os.environ.copy()
            env["PATH"] = "%s:%s" % (bin_dir, env["PATH"])
            env["STUB_CQUERY_OUTPUT"] = str(compat_file)
            if cquery_fails:
                env["STUB_CQUERY_FAIL"] = "1"

            result = subprocess.run(
                [
                    str(fixture),
                    str(final_file),
                    str(kinds_file),
                    str(scratch),
                    str(diagnostics),
                    str(root),
                ],
                cwd=str(root),
                check=False,
                capture_output=True,
                text=True,
                env=env,
                timeout=30,
            )
            self.assertEqual(0, result.returncode, result.stderr)
            return result.stdout.strip()

    # ---- the three incidents -------------------------------------------

    def test_incident_one_py_test_only_survivor_skips(self):
        """The manual-tagged cc_test is not in the final list, so it cannot vote."""
        verdict = self._decide(
            label_kinds=[
                "cc_test rule //donner/svg:renderer_manual_test",
                "py_test rule //tools:filter_coverage_tests",
            ],
            final_targets=["//tools:filter_coverage_tests"],
            host_compat=["@@//tools:filter_coverage_tests HOST_COMPATIBLE"],
        )
        self.assertEqual("skip", verdict)

    def test_incident_two_macos_only_cc_tests_skip(self):
        """A cc_test is not automatically host code; the cquery decides."""
        labels = [
            "//donner/svg/renderer/metal:metal_device_tests",
            "//donner/svg/renderer/metal:metal_pipeline_tests",
            "//donner/svg/renderer/metal:metal_surface_tests",
        ]
        verdict = self._decide(
            label_kinds=["cc_test rule %s" % label for label in labels],
            final_targets=labels,
            host_compat=["@@%s HOST_INCOMPATIBLE" % label for label in labels],
        )
        self.assertEqual("skip", verdict)

    def test_incident_three_playwright_wrappers_skip(self):
        """The exact set from the run that sailed through both earlier fixes.

        The `__entry_point` helper is host-compatible and is NOT a test, so it
        never reaches the final list; the two `js_test` wrappers are unknown by
        kind (fail-open) but the cquery proves both host-incompatible.
        """
        tests = [
            "//donner/editor/wasm/tests:browser_presentation_regression_test",
            "//donner/editor/wasm/tests:chromium_remote_smoke",
        ]
        verdict = self._decide(
            label_kinds=[
                "js_test rule %s" % tests[0],
                "directory_path rule //donner/editor/wasm/tests:"
                "browser_presentation_regression_test__entry_point",
                "js_test rule %s" % tests[1],
            ],
            final_targets=tests,
            host_compat=["@@%s HOST_INCOMPATIBLE" % label for label in tests],
        )
        self.assertEqual("skip", verdict)

    def test_incident_three_pre_narrowing_helper_no_longer_votes(self):
        """The direct cause: judged on the wider set, the same inputs say run.

        This is the asymmetry the consolidation removes. Keeping it as an
        executable statement means a future refactor that quietly widens the
        decided-on set fails here instead of in CI.
        """
        entry_point = (
            "//donner/editor/wasm/tests:"
            "browser_presentation_regression_test__entry_point"
        )
        tests = [
            "//donner/editor/wasm/tests:browser_presentation_regression_test",
            "//donner/editor/wasm/tests:chromium_remote_smoke",
        ]
        verdict = self._decide(
            label_kinds=[
                "js_test rule %s" % tests[0],
                "directory_path rule %s" % entry_point,
                "js_test rule %s" % tests[1],
            ],
            final_targets=tests + [entry_point],
            host_compat=[
                "@@%s HOST_INCOMPATIBLE" % tests[0],
                "@@%s HOST_COMPATIBLE" % entry_point,
                "@@%s HOST_INCOMPATIBLE" % tests[1],
            ],
        )
        self.assertEqual("run", verdict)

    # ---- positive control and fail-open paths ---------------------------

    def test_mixed_set_with_host_compatible_cc_test_runs(self):
        """A real C++ test in the final list must still produce coverage."""
        verdict = self._decide(
            label_kinds=[
                "cc_test rule //donner/base:string_utils_tests",
                "js_test rule //donner/editor/wasm/tests:chromium_remote_smoke",
                "py_test rule //tools:filter_coverage_tests",
            ],
            final_targets=[
                "//donner/base:string_utils_tests",
                "//donner/editor/wasm/tests:chromium_remote_smoke",
                "//tools:filter_coverage_tests",
            ],
            host_compat=[
                "@@//donner/base:string_utils_tests HOST_COMPATIBLE",
                "@@//donner/editor/wasm/tests:chromium_remote_smoke HOST_INCOMPATIBLE",
                "@@//tools:filter_coverage_tests HOST_COMPATIBLE",
            ],
        )
        self.assertEqual("run", verdict)

    def test_cquery_failure_keeps_the_coverage_run(self):
        verdict = self._decide(
            label_kinds=["py_test rule //tools:filter_coverage_tests"],
            final_targets=["//tools:filter_coverage_tests"],
            host_compat=[],
            cquery_fails=True,
        )
        self.assertEqual("run", verdict)

    def test_final_label_without_a_kind_line_keeps_the_coverage_run(self):
        """`tests()` can name a test_suite member the kind query never saw."""
        verdict = self._decide(
            label_kinds=["py_test rule //tools:filter_coverage_tests"],
            final_targets=[
                "//tools:filter_coverage_tests",
                "//donner/svg:suite_member_tests",
            ],
            host_compat=[
                "@@//tools:filter_coverage_tests HOST_COMPATIBLE",
                "@@//donner/svg:suite_member_tests HOST_COMPATIBLE",
            ],
        )
        self.assertEqual("run", verdict)

    # ---- workflow shape: one decision, after every narrowing step -------

    def test_the_workflow_classifies_exactly_once(self):
        """Three calls was the bug. Any number but one invites a fourth."""
        self.assertEqual(
            1,
            self.workflow.count("python3 tools/coverage_instrumentable_targets.py"),
            "coverage.yml classifies instrumentability more than once; the "
            "second call is how the sets drifted apart",
        )
        self.assertEqual(
            1,
            self.workflow.count("bazelisk cquery"),
            "coverage.yml runs more than one host-compatibility cquery",
        )

    def test_the_decision_names_the_final_list_and_the_cquery_answer(self):
        """Both criteria, one call, on the narrowed list."""
        self.assertIn('--restrict-to-file "$final_file"', self.decision)
        self.assertIn('--host-incompatible-file "$incompatible_file"', self.decision)
        # The retired two-phase handshake must not come back.
        self.assertNotIn("--instrumentable-out", self.workflow)

    def test_the_decision_runs_after_every_narrowing_step(self):
        """Order is the invariant: narrow, then decide, then emit."""
        test_query = self.workflow.index(
            'affected_tests_file="$work_dir/affected-tests.txt"'
        )
        variant_trim = self.workflow.index(
            "# Representative-variant trim", test_query
        )
        final_file = self.workflow.index(
            'final_file="$work_dir/final-targets.txt"', variant_trim
        )
        decide = self.workflow.index("coverage_final_decision \\", final_file)
        emit = self.workflow.index(
            'emit_targets false bazel_diff "$affected"', decide
        )
        self.assertLess(test_query, variant_trim)
        self.assertLess(variant_trim, final_file)
        self.assertLess(final_file, decide)
        self.assertLess(decide, emit)

    def test_a_skip_uses_the_established_reason(self):
        start = self.workflow.index('if [[ "$final_decision" == "skip" ]]')
        skip_block = self.workflow[start : self.workflow.index("\n          fi", start)]
        self.assertIn('emit_targets true no_instrumentable_targets ""', skip_block)


if __name__ == "__main__":
    unittest.main()
