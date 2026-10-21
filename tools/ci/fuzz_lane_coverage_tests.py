"""Checks the rules that decide whether the Fuzz workflow soaks every fuzzer.

The cases run the real .bazelrc and Fuzz workflow through the checker against a stand-in graph
with the soak shapes `donner_cc_fuzzer` produces, so a workflow or lane change that would leave a
fuzzer unsoaked fails here, without a Bazel query.
"""

import unittest

from python.runfiles import runfiles
from tools.ci import fuzz_lane_coverage as coverage

_SUITE = "//tools/ci:image_loader_production_soak"
_PRODUCTION_SOAK = "//donner/svg/resources:image_loader_production_fuzzer_soak"


def _read(path):
    with open(runfiles.Create().Rlocation("donner/" + path), encoding="utf-8") as handle:
        return handle.read()


def _graph():
    """A stand-in graph: one fuzzer of each tag shape `donner_cc_fuzzer` generates today."""
    soaks = {
        "//donner/base:path_fuzzer_soak": {"fuzz_target", "fuzz_soak"},
        "//donner/svg/text:font_decoder_fuzzer_soak": {"fuzz_target", "fuzz_soak",
                                                       "fuzz_text_full"},
        "//donner/svg/renderer/geode:budget_fuzzer_soak": {"fuzz_target", "fuzz_soak",
                                                           "fuzz_geode", "exclusive-if-local"},
        "//donner/svg/renderer/tests:text_geode_fuzzer_soak": {"fuzz_target", "fuzz_soak",
                                                               "fuzz_geode", "fuzz_text_full"},
        _PRODUCTION_SOAK: {"fuzz_target", "fuzz_soak", "manual"},
    }
    linked = set()
    for soak in soaks:
        fuzzer = soak[:-len("_soak")]
        linked.update({fuzzer, fuzzer + "_bin"})
    # A full-text-only fuzzer links its implementation targets instead.
    linked.update({"//donner/svg/text:font_decoder_fuzzer_impl",
                   "//donner/svg/text:font_decoder_fuzzer_bin_impl"})
    return soaks, linked


class FuzzLaneCoverageTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.bazelrc = _read(".bazelrc")
        cls.workflow = _read(".github/workflows/fuzz.yml")

    def _problems(self, soaks=None, linked=None, workflow=None, bazelrc=None):
        default_soaks, default_linked = _graph()
        soaks = {label: frozenset(tags) for label, tags in (soaks or default_soaks).items()}
        configs = coverage.bazelrc_configs(bazelrc or self.bazelrc)
        steps = [{"filters": coverage.tag_filters(coverage.lane_flags(configs, lane)),
                  "patterns": patterns}
                 for lane, patterns in coverage.workflow_steps(workflow or self.workflow)]
        expanded = {_SUITE: {_PRODUCTION_SOAK}}
        return coverage.problems(soaks, default_linked if linked is None else linked, steps,
                                 expanded)

    def test_reads_every_phase_of_the_workflow(self):
        lanes = [lane for lane, _ in coverage.workflow_steps(self.workflow)]
        self.assertEqual(lanes, ["fuzz-lane", "fuzz-lane-text-full", "fuzz-lane-production-soak",
                                 "fuzz-lane-geode", "fuzz-lane", "fuzz-lane-text-full",
                                 "fuzz-lane-production-soak"])
        self.assertIn(("fuzz-lane-production-soak", [_SUITE]),
                      coverage.workflow_steps(self.workflow))

    def test_current_workflow_soaks_every_fuzzer_shape(self):
        self.assertEqual(self._problems(), [])

    def test_fuzzer_without_a_soak_fails(self):
        soaks, linked = _graph()
        self.assertEqual(
            self._problems(linked=linked | {"//donner/base:raw_fuzzer"}),
            ["//donner/base:raw_fuzzer links libFuzzer but has no soak target; define it with "
             "donner_cc_fuzzer"])
        del soaks["//donner/base:path_fuzzer_soak"]
        problems = self._problems(soaks=soaks, linked=linked)
        self.assertEqual(len(problems), 2, problems)
        self.assertTrue(all("//donner/base:path_fuzzer" in problem for problem in problems))

    def test_manual_soak_outside_a_named_suite_fails(self):
        soaks, _ = _graph()
        soaks["//donner/base:quiet_fuzzer_soak"] = {"fuzz_target", "fuzz_soak", "manual"}
        problems = self._problems(soaks=soaks)
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("//donner/base:quiet_fuzzer_soak is not run by any Fuzz workflow step",
                      problems[0])

    def test_soak_without_a_selected_tag_fails(self):
        soaks, _ = _graph()
        soaks["//donner/base:untagged_fuzzer_soak"] = {"fuzz_soak"}
        problems = self._problems(soaks=soaks)
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("//donner/base:untagged_fuzzer_soak is not run", problems[0])

    def test_variant_tag_without_its_lane_fails(self):
        soaks, _ = _graph()
        soaks["//donner/base:wasm_fuzzer_soak"] = {"fuzz_target", "fuzz_soak", "fuzz_wasm"}
        self.assertEqual(self._problems(soaks=soaks), [
            "//donner/base:wasm_fuzzer_soak carries fuzz_wasm, but no Fuzz workflow step that "
            "runs it selects that tag"])

    def test_dropping_a_phase_fails(self):
        cases = {
            "      - name: Test Geode render fuzzers\n":
                ["//donner/svg/renderer/geode:budget_fuzzer_soak carries fuzz_geode",
                 "//donner/svg/renderer/tests:text_geode_fuzzer_soak carries fuzz_geode"],
            "      - name: Soak image loader at production decode budget\n":
                [_PRODUCTION_SOAK + " is not run"],
        }
        for step, expected in cases.items():
            with self.subTest(step=step.strip()):
                workflow = self.workflow
                while step in workflow:
                    before, after = workflow.split(step, 1)
                    workflow = before + after.split("\n\n", 1)[1]
                self.assertNotEqual(workflow, self.workflow)
                problems = self._problems(workflow=workflow)
                self.assertEqual(len(problems), len(expected), problems)
                for problem, prefix in zip(problems, expected):
                    self.assertTrue(problem.startswith(prefix), problem)

    def test_lane_filter_that_excludes_soaks_fails(self):
        bazelrc = self.bazelrc.replace("test:fuzz-lane --test_tag_filters=fuzz_target\n",
                                       "test:fuzz-lane --test_tag_filters=fuzz_target,-fuzz_soak\n")
        self.assertNotEqual(bazelrc, self.bazelrc)
        problems = self._problems(bazelrc=bazelrc)
        self.assertIn("//donner/base:path_fuzzer_soak is not run by any Fuzz workflow step "
                      "(tags: fuzz_soak, fuzz_target)", problems)

    def test_patterns(self):
        step = {"filters": (frozenset({"fuzz_target"}), frozenset()),
                "patterns": ["//donner/...", "-//donner/svg/..."]}
        tags = frozenset({"fuzz_target"})
        self.assertTrue(coverage.step_runs(step, "//donner/base:a_soak", tags, {}))
        self.assertTrue(coverage.step_runs(step, "//donner:a_soak", tags, {}))
        self.assertFalse(coverage.step_runs(step, "//donner/svg/text:a_soak", tags, {}))
        self.assertFalse(coverage.step_runs(step, "//donnerx:a_soak", tags, {}))
        self.assertFalse(coverage.step_runs(step, "//donner/base:a_soak",
                                            tags | {"manual"}, {}))
        self.assertTrue(coverage.step_runs(
            {**step, "patterns": ["//donner/base:all"]}, "//donner/base:a_soak", tags, {}))
        self.assertFalse(coverage.step_runs(
            {**step, "patterns": ["//donner/base:all"]}, "//donner/base/x:a_soak", tags, {}))


if __name__ == "__main__":
    unittest.main()
