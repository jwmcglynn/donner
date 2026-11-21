"""Tests for tools/ci/linux_gpu_lane.py: the Linux GPU lane's selection and its execution check."""

import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest

import linux_gpu_lane as lane

DEVICE = "//donner/svg/renderer/geode:geode_device_tests"
GOLDEN = "//donner/svg/renderer/tests:renderer_geode_golden_tests"
GPU = [{"name": "ISA", "value": "aarch64"}, {"name": "OSFamily", "value": "linux"},
       {"name": "container-image", "value": "none"}, {"name": "gpu", "value": "present"}]


def _rule(label, rule_class=lane.WRAPPER_RULE, tags=(lane.GPU_TAG,), env=None, dep=None,
          selected_env=None):
    attributes = []
    if selected_env is not None:
        # The query's shape of a select()-valued string dict: one selector of branches.
        attributes.append({"name": "env", "type": "STRING_DICT", "selectorList": {
            "type": "STRING_DICT", "elements": [{"entries": [
                {"label": "//conditions:default" if index else "//:opt", "stringDictValue": [
                    {"key": key, "value": value} for key, value in branch.items()]}
                for index, branch in enumerate(selected_env)]}]}})
    if tags:
        attributes.append({"name": "tags", "stringListValue": list(tags)})
    if env:
        attributes.append({"name": "env", "stringDictValue": [
            {"key": key, "value": value} for key, value in env.items()]})
    if dep:
        attributes.append({"name": "dep", "stringValue": dep})
    return json.dumps({"type": "RULE", "rule": {"name": label, "ruleClass": rule_class,
                                                "attribute": attributes}})


def _wrapped(label, **implementation):
    """A wrapper and its implementation, whose attributes (such as env) are `implementation`."""
    return [_rule(label, dep=label + "_impl"),
            _rule(label + "_impl", rule_class="cc_test", tags=(), **implementation)]


def _spawn(label, mnemonic="TestRunner", runner="remote", cache_hit=False, platform=None):
    return {"targetLabel": label, "mnemonic": mnemonic, "runner": runner, "cacheHit": cache_hit,
            "platform": {"properties": GPU if platform is None else platform}}


def _result(label, shard=1, strategy="remote", cached=False):
    identity = {"label": label, "run": 1, "shard": shard, "attempt": 1}
    return {"id": {"testResult": identity},
            "testResult": {"status": "PASSED", "cachedLocally": cached,
                           "executionInfo": {"strategy": strategy}}}


def _summary(label, shards=None):
    summary = {"overallStatus": "PASSED", "runCount": 1}
    if shards:
        summary["shardCount"] = shards
    return {"id": {"testSummary": {"label": label}}, "testSummary": summary}


FINISHED = {"id": {"buildFinished": {}}, "finished": {"overallSuccess": True}}


class SelectTest(unittest.TestCase):
    def test_selects_scoped_gpu_wrappers_that_leave_the_driver_to_the_executor(self):
        lines = [
            _rule(DEVICE, dep=DEVICE + "_impl"),
            _rule(DEVICE + "_impl", rule_class="cc_test", tags=()),
            _rule(GOLDEN),
            # Out of scope: GPU wrappers in suites the lane has not admitted on hardware.
            _rule("//donner/editor/tests:rnr_replay_tests_geode"),
            _rule("//donner/svg/renderer/tests:renderer_geode_tests"),
            # Not GPU-opening: a Geode variant without the GPU tag.
            _rule("//donner/svg/renderer/geode:geo_encoder_tests", tags=("small",)),
            # Every excluded tag: opt-in, wall-clock, soak, remote twin and client-only tests.
            *[_rule("//donner/svg/renderer/geode:geode_%s_tests" % tag.replace("-", "_"),
                    tags=(lane.GPU_TAG, tag))
              for tag in ("manual", "perf", "fuzz_soak", "ci-remote-gpu", "local", "no-remote",
                          "no-remote-exec")],
            # A reviewed test that builds only for another OS.
            *[_rule(label) for label in sorted(lane.OTHER_OS_TESTS)],
            # Not the wrapper rule: its test action could leave the target platform.
            _rule("//donner/svg/renderer/geode:geode_plain_tests", rule_class="cc_test"),
            # The implementation names its own driver. The wrapper rule has no env attribute, so an
            # env on the wrapper itself (which Bazel would reject) is not consulted.
            *_wrapped("//donner/svg/renderer/geode:geode_pinned_tests",
                      env={"VK_DRIVER_FILES": "/usr/share/vulkan/icd.d/software.json"}),
        ]
        # Each driver variable excludes a test, and an env that is a select() does too when any
        # branch names one.
        for variable in ("VK_ICD_FILENAMES", "VK_DRIVER_FILES", "VK_ADD_DRIVER_FILES",
                         "VK_LOADER_DRIVERS_SELECT", "VK_LOADER_DRIVERS_DISABLE"):
            lines += _wrapped("//donner/svg/renderer/geode:geode_%s_tests" % variable.lower(),
                              env={variable: "x"})
        lines += _wrapped("//donner/svg/renderer/geode:geode_selected_env_tests",
                          selected_env=[{"OTHER": "1"}, {"VK_ICD_FILENAMES": "/a.json"}])
        lines += _wrapped("//donner/svg/renderer/geode:geode_unreadable_env_tests",
                          selected_env=[])
        self.assertEqual([DEVICE, GOLDEN], lane.select(lines))
        # An implementation env, literal or select(), with no driver variable leaves it selected.
        self.assertEqual([GOLDEN], lane.select(_wrapped(GOLDEN, env={"OTHER": "1"})))
        self.assertEqual([GOLDEN],
                         lane.select(_wrapped(GOLDEN, selected_env=[{"OTHER": "1"}, {}])))

    def test_query_intersects_the_requested_patterns_with_the_scope(self):
        text = lane.query(["//donner/..."])
        self.assertIn('set("//donner/...") intersect (', text)
        self.assertIn("//donner/svg/renderer/geode:all", text)
        self.assertIn(GOLDEN, text)
        self.assertIn("labels(dep, $tests)", text)
        with self.assertRaises(ValueError):
            lane.query([])


class CheckTest(unittest.TestCase):
    selection = {DEVICE, GOLDEN}

    def run_check(self, spawns, events):
        with tempfile.TemporaryDirectory() as work:
            log = Path(work) / "exec.json"
            log.write_text("".join(json.dumps(spawn, indent=2) + "\n" for spawn in spawns))
            bep = Path(work) / "bep.json"
            bep.write_text("".join(json.dumps(event) + "\n" for event in events))
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr), contextlib.redirect_stdout(io.StringIO()):
                status = lane.check(str(log), str(bep), self.selection)
        return status, stderr.getvalue()

    def passing(self):
        spawns = [_spawn(DEVICE), _spawn(GOLDEN), _spawn(GOLDEN),
                  _spawn("//donner/base:base", mnemonic="CppCompile", runner="remote cache hit",
                         cache_hit=True, platform=GPU[:3])]
        events = [_result(DEVICE), _summary(DEVICE), _result(GOLDEN, 1), _result(GOLDEN, 2),
                  _summary(GOLDEN, shards=2), FINISHED]
        return spawns, events

    def assert_fails(self, spawns, events, reason):
        status, problems = self.run_check(spawns, events)
        self.assertEqual(1, status, problems)
        self.assertIn(reason, problems)

    def test_fresh_remote_gpu_test_runs_pass(self):
        self.assertEqual((0, ""), self.run_check(*self.passing()))

    def test_compile_executed_by_the_test_invocation_fails(self):
        spawns, events = self.passing()
        spawns.append(_spawn("//donner/base:base", mnemonic="CppCompile", platform=GPU[:3]))
        self.assert_fails(spawns, events, "CppCompile //donner/base:base executed (remote)")
        # A selected test's own link, even one that carried the GPU request, is not a test run.
        spawns, events = self.passing()
        spawns.append(_spawn(DEVICE, mnemonic="CppLink"))
        self.assert_fails(spawns, events, "CppLink %s executed (remote)" % DEVICE)

    def test_test_runner_without_the_gpu_request_fails(self):
        spawns, events = self.passing()
        spawns[0] = _spawn(DEVICE, platform=GPU[:3])
        self.assert_fails(spawns, events, "TestRunner %s ran without the GPU request" % DEVICE)

    def test_unselected_or_local_test_runner_fails(self):
        spawns, events = self.passing()
        self.assert_fails(spawns + [_spawn("//donner/base:base_tests")], events,
                          "TestRunner //donner/base:base_tests executed")
        spawns[0] = _spawn(DEVICE, runner="linux-sandbox")
        self.assert_fails(spawns, events, "ran on linux-sandbox, not remotely")

    def test_missing_result_fails(self):
        spawns, events = self.passing()
        self.assert_fails(spawns, [event for event in events if event != _result(DEVICE)],
                          "%s reported 0 of 1 run and shard results" % DEVICE)
        self.assert_fails(spawns, [event for event in events if event != _summary(DEVICE)],
                          "%s reported no result" % DEVICE)
        self.assert_fails(spawns, [event for event in events if event != _result(GOLDEN, 2)],
                          "%s reported 1 of 2 run and shard results" % GOLDEN)
        self.assert_fails(spawns, events[:-1], "no build-finished event")

    def test_skipped_or_aborted_selected_test_fails(self):
        spawns, events = self.passing()
        events = [event for event in events if event not in (_result(DEVICE), _summary(DEVICE))]
        for reason in ("SKIPPED", "ANALYSIS_FAILURE"):
            with self.subTest(reason=reason):
                aborted = {"id": {"targetCompleted": {"label": DEVICE}},
                           "aborted": {"reason": reason, "description": "build was skipped"}}
                self.assert_fails([spawn for spawn in spawns if spawn != _spawn(DEVICE)],
                                  events + [aborted],
                                  "%s was skipped or aborted instead of running" % DEVICE)

    def test_empty_or_unrelated_execution_log_fails(self):
        _, events = self.passing()
        self.assert_fails([], events, "no selected test ran on the remote executor")
        self.assert_fails([], events, "%s has 0 of 1 remote test runs with the GPU request" % DEVICE)
        other = [_spawn("//donner/base:base", mnemonic="CppCompile", runner="remote cache hit",
                        cache_hit=True)]
        self.assert_fails(other, events, "no selected test ran on the remote executor")

    def test_result_without_a_matching_gpu_run_fails(self):
        spawns, events = self.passing()
        spawns.remove(_spawn(GOLDEN))
        self.assert_fails(spawns, events, "%s has 1 of 2 remote test runs with the GPU request"
                          % GOLDEN)

    def test_failed_summary_fails(self):
        spawns, events = self.passing()
        failed = _summary(DEVICE)
        failed["testSummary"]["overallStatus"] = "FAILED"
        events[1] = failed
        self.assert_fails(spawns, events, "%s did not pass: FAILED" % DEVICE)

    def test_cached_or_unselected_result_fails(self):
        spawns, events = self.passing()
        events[0] = _result(DEVICE, cached=True)
        self.assert_fails(spawns, events, "%s reported a cached result" % DEVICE)
        spawns, events = self.passing()
        spawns[0] = _spawn(DEVICE, runner="remote cache hit", cache_hit=True)
        self.assert_fails(spawns, events, "%s reused a cached test result" % DEVICE)
        spawns, events = self.passing()
        self.assert_fails(spawns, events + [_result("//donner/base:base_tests")],
                          "//donner/base:base_tests reported a result but was not selected")


if __name__ == "__main__":
    unittest.main()
