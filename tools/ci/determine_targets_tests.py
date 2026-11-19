"""Runs the CI target selection scripts against recording stand-ins for their tools.

Each case runs tools/ci/determine_targets.sh or tools/ci/fetch_bazel_diff.sh as its workflow step
does, with git, java (bazel-diff), bazelisk, curl, sha256sum and sleep replaced by a stand-in that
records its call and answers from the case's configuration, then checks the step outputs the
script wrote to $GITHUB_OUTPUT.
"""

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from python.runfiles import runfiles
from tools import test_timing
from tools.test_executable_fixtures import ExecutableFixtureDirectory, write_python_stand_in

_TOOLS = ("bazelisk", "curl", "git", "java", "sha256sum", "sleep")
# The host commands the scripts run besides the stand-ins and the shell's builtins. The scripts
# see these and the stand-ins only, so a command missing here fails on every host, as it would on
# a remote executor whose PATH has no /usr/bin.
_HOST_TOOLS = ("cp", "dirname", "grep", "mkdir", "mktemp", "rm")
_REFERENCE = "//donner/svg/renderer/tests:resvg_test_suite_wgpu_reference_linux"
# Query expressions, by a fragment each one contains.
_REFERENCE_QUERY = "labels(dep, tests(//tools/ci:linux_wgpu_resvg_reference))"
_MANUAL_QUERY = '"manual", set('
_MANIFEST_QUERY = "tests(//...)) except"
_BROWSER_QUERY = '"launches-browser", set('
# The Metal suite intersection comes first: its expression also names the suites.
_METAL_INTERSECTION_QUERY = ") intersect tests(//tools/ci:metal_full_validation"
_METAL_SUITE_QUERY = "tests(//tools/ci:metal_full_validation"
# What the selection publishes when bazel-diff proves that no target is affected.
_NO_AFFECTED_TARGETS = "//tools/ci:no_affected_targets"
_JAR_URL = "https://github.com/Tinder/bazel-diff/releases/download/v0.0.0/bazel-diff_deploy.jar"

# One stand-in for every tool. It records its name and arguments, then answers from the JSON
# configuration in STUB_CONFIG:
#   git: "merge_base" (null fails), "changed" (diff output), "head" (rev-parse HEAD); worktree
#     add creates its directory.
#   java: "affected" (get-impacted-targets output, which fails when "impacted_fail" is set);
#     generate-hashes writes "{}" unless "hashes_fail" is set; with "query_error" it reports a
#     query error and, unless given --no-keep_going, still writes partial hashes and exits 0.
#     Like the real bazel-diff on this workspace, generate-hashes without
#     --excludeExternalTargets also queries //external:all-targets, which fails because
#     WORKSPACE is disabled: fatal with --no-keep_going, ignored otherwise.
#   bazelisk: "queries", a list of [fragment, output, status]; the first fragment the query
#     expression contains answers it.
#   curl: "downloads", one [body, status] per call, in order.
#   sha256sum: checks `--check --status` input against the file's real digest.
#   sleep: records only.
_RECORDER = r'''
import hashlib, json, os, sys
from pathlib import Path

tool, args = sys.argv[1], sys.argv[2:]
with open(os.environ["CAPTURE"], "a", encoding="utf-8") as capture:
    capture.write(json.dumps([tool, *args]) + "\n")
config = json.loads(Path(os.environ["STUB_CONFIG"]).read_text(encoding="utf-8"))


def answer(output, status=0):
    sys.stdout.write(output)
    sys.exit(status)


if tool == "git":
    with open(os.environ["CAPTURE"] + ".cwd", "a", encoding="utf-8") as cwd:
        cwd.write(os.getcwd() + "\n")
    if args[:1] == ["merge-base"]:
        if config.get("merge_base") is None:
            answer("", 1)
        answer(config["merge_base"] + "\n")
    if args[:2] == ["diff", "--name-only"]:
        answer(config.get("changed", ""))
    if args[:2] == ["rev-parse", "--verify"]:
        tree = config.get("trees", {}).get(args[2])
        answer(tree + "\n" if tree else "", 0 if tree else 1)
    if args == ["rev-parse", "HEAD"]:
        answer(config.get("head", "merge0") + "\n")
    if args[:2] == ["worktree", "add"]:
        Path(args[-2]).mkdir(parents=True)
    answer("")
if tool == "java":
    output = Path(args[-1])
    if "get-impacted-targets" in args and config.get("impacted_fail"):
        answer("", 1)
    if "get-impacted-targets" in args:
        output.write_text(config.get("affected", ""), encoding="utf-8")
        answer("")
    if "--excludeExternalTargets" not in args:
        # bazel-diff skips //external only when `bazel mod graph` succeeds, and it fails here.
        sys.stderr.write("ERROR: no such package 'external': //external package is not "
                         "available since the WORKSPACE file is disabled\n")
        if "--no-keep_going" in args:
            answer("", 1)
    if config.get("query_error"):
        # bazel-diff's default --keep_going hashes what the query returned and exits 0.
        sys.stderr.write("ERROR: no such package '//broken': BUILD file not found\n")
        if "--no-keep_going" in args:
            answer("", 1)
        output.write_text('{"partial": 1}', encoding="utf-8")
        answer("")
    if config.get("hashes_fail"):
        output.write_text("partial", encoding="utf-8")
        answer("", 1)
    output.write_text("{}", encoding="utf-8")
    answer("")
if tool == "bazelisk":
    for fragment, output, status in config.get("queries", []):
        if fragment in args[-1]:
            answer(output, status)
    answer("")
if tool == "curl":
    counter = Path(os.environ["CAPTURE"] + ".curl")
    seen = int(counter.read_text()) if counter.exists() else 0
    counter.write_text(str(seen + 1))
    body, status = config["downloads"][seen]
    if status == 0:
        Path(args[args.index("-o") + 1]).write_bytes(body.encode())
    answer("", status)
if tool == "sha256sum":
    digest, path = sys.stdin.read().rstrip("\n").split("  ", 1)
    answer("", 0 if hashlib.sha256(Path(path).read_bytes()).hexdigest() == digest else 1)
answer("")
'''


def _hashing(runner_temp, workspace, output):
    """The bazel-diff call that hashes `workspace` into `output`, as every hashing runs it."""
    return ["java", "-jar", str(runner_temp / "bazel-diff.jar"), "generate-hashes",
            "--includeTargetType", "--no-keep_going", "--excludeExternalTargets",
            "-w", str(workspace), "-b", "bazelisk", str(output)]


class _ScriptTest(unittest.TestCase):
    script_name = None

    @classmethod
    def setUpClass(cls):
        cls.script = Path(runfiles.Create().Rlocation("donner/tools/ci/" + cls.script_name))
        cls.executables = ExecutableFixtureDirectory()
        cls.addClassCleanup(cls.executables.cleanup)
        recorder = cls.executables.path / "record.py"
        recorder.write_text(_RECORDER, encoding="utf-8")
        cls.bin = cls.executables.path / "bin"
        cls.bin.mkdir()
        for tool in _TOOLS:
            write_python_stand_in(cls.bin / tool, recorder, tool)
        shutil.copy(cls.executables.write_python3(), cls.bin / "python3")
        cls.host = cls.executables.path / "host"
        cls.host.mkdir()
        for tool in _HOST_TOOLS:
            found = shutil.which(tool)
            if found is None:
                raise unittest.SkipTest("host command %s is unavailable" % tool)
            os.symlink(found, cls.host / tool)
        # The macOS image's /bin/bash is 3.2; the selection runs on Linux bash, and both work.
        cls.bash = "/bin/bash" if os.path.exists("/bin/bash") else shutil.which("bash")

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="target selection ")
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)
        self.workspace = self.work / "workspace"
        self.workspace.mkdir()
        self.home = self.work / "home"
        self.runner_temp = self.work / "runner-temp"
        self.runner_temp.mkdir()
        self.capture = self.work / "calls.jsonl"
        self.output = self.work / "github-output"

    def run_script(self, *args, config=None, event=None, cwd=None, **env):
        self.capture.write_text("", encoding="utf-8")
        self.output.write_text("", encoding="utf-8")
        config_path = self.work / "config.json"
        config_path.write_text(json.dumps(config or {}), encoding="utf-8")
        event_path = self.work / "event.json"
        event_path.write_text(json.dumps(event or {}), encoding="utf-8")
        environ = {
            "PATH": os.pathsep.join((str(self.bin), str(self.host))),
            "CAPTURE": str(self.capture), "STUB_CONFIG": str(config_path),
            "HOME": str(self.home), "RUNNER_TEMP": str(self.runner_temp),
            "GITHUB_WORKSPACE": str(self.workspace), "GITHUB_OUTPUT": str(self.output),
            "GITHUB_EVENT_PATH": str(event_path), "GITHUB_EVENT_NAME": "pull_request",
            "GITHUB_REF": "refs/pull/7/merge", "GITHUB_SHA": "sha0",
        }
        environ.update(env)
        result = subprocess.run([self.bash, str(self.script), *args],
                                cwd=self.workspace if cwd is None else cwd,
                                env=environ, capture_output=True, text=True, check=False,
                                timeout=test_timing.STALL_GUARD_SECONDS)
        calls = [json.loads(line) for line in self.capture.read_text().splitlines()]
        return result, calls, self.output.read_text(encoding="utf-8")

    def assert_outputs(self, outputs, *lines):
        self.assertEqual(outputs, "".join(line + "\n" for line in lines))


def _pull_request(labels=(), base="base0", head="head0"):
    return {"pull_request": {"base": {"sha": base}, "head": {"sha": head},
                             "labels": [{"name": label} for label in labels]}}


def _queries(**overrides):
    """bazelisk answers for an affected set that reaches every optional lane."""
    queries = {
        _REFERENCE_QUERY: (_REFERENCE + "\n", 0),
        _MANUAL_QUERY: ("", 0),
        _MANIFEST_QUERY: ("//w:smoke\n\n//w:regression\n", 0),
        _BROWSER_QUERY: ("//w:smoke\n", 0),
        _METAL_INTERSECTION_QUERY: ("", 0),
        _METAL_SUITE_QUERY: ("//donner/gpu/metal/tests:metal_tests\n", 0),
    }
    queries.update(overrides)
    return [[fragment, output, status] for fragment, (output, status) in queries.items()]


class DetermineTest(_ScriptTest):
    script_name = "determine_targets.sh"

    def determine(self, changed="donner/base/Utils.h\n", affected="//a:b\n", labels=(),
                  queries=None, **config):
        config = {"merge_base": "fork0", "changed": changed, "affected": affected,
                  "queries": _queries() if queries is None else queries, **config}
        return self.run_script("determine", config=config, event=_pull_request(labels))

    def queried(self, calls):
        return [call[-1] for call in calls if call[0] == "bazelisk"]

    def test_a_push_keeps_full_coverage_without_bazel_diff(self):
        result, calls, outputs = self.run_script("determine", GITHUB_EVENT_NAME="push",
                                                 GITHUB_REF="refs/heads/feature")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_outputs(outputs, "fallback=true", "affected=", "full_test=false")
        self.assertIn("Non-PR event; keeping full //... coverage.", result.stdout)
        self.assertEqual(calls, [])

    def test_a_main_push_prewarms_the_base_hashes_once(self):
        cached = self.home / ".cache/bazel-diff-base-hashes/sha0.json"
        result, calls, outputs = self.run_script("determine", GITHUB_EVENT_NAME="push",
                                                 GITHUB_REF="refs/heads/main")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_outputs(outputs, "fallback=true", "affected=", "full_test=false")
        self.assertIn("Pre-warming bazel-diff hash cache for sha0.", result.stdout)
        self.assertEqual(calls, [_hashing(self.runner_temp, self.workspace, cached)])
        self.assertEqual(cached.read_text(), "{}")
        result, calls, _ = self.run_script("determine", GITHUB_EVENT_NAME="push",
                                           GITHUB_REF="refs/heads/main")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(calls, [])

    def test_a_failed_prewarm_fails_the_job_and_removes_its_partial_file(self):
        # Every pull request against this commit would fail the same way, so main turns red.
        for config in ({"hashes_fail": True}, {"query_error": True}):
            with self.subTest(config=config):
                result, _, _ = self.run_script("determine", config=config,
                                               GITHUB_EVENT_NAME="workflow_dispatch",
                                               GITHUB_REF="refs/heads/main")
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn("::error::bazel-diff could not hash sha0", result.stdout)
                self.assertEqual(list((self.home / ".cache/bazel-diff-base-hashes").iterdir()),
                                 [])

    def test_a_pull_request_without_its_revisions_falls_back(self):
        result, calls, outputs = self.run_script("determine", event=_pull_request(head=""))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_outputs(outputs, "fallback=true", "affected=", "full_test=false",
                            "wgpu_reference_unknown=true")
        self.assertIn("Missing PR base/head SHA", result.stdout)
        self.assertEqual(calls, [])

    def test_the_full_test_label_forces_full_coverage(self):
        result, calls, outputs = self.determine(labels=("bug", "ci:full-test"))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_outputs(outputs, "fallback=true", "affected=", "full_test=true")
        self.assertEqual(calls, [])

    def test_no_changed_files_falls_back(self):
        result, calls, outputs = self.determine(changed="")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_outputs(outputs, "full_test=false", "fallback=true", "affected=",
                            "wgpu_reference_unknown=true")
        self.assertEqual(calls, [["git", "rev-parse", "--verify", "base0^{tree}"],
                                 ["git", "merge-base", "base0", "head0"],
                                 ["git", "diff", "--name-only", "fork0", "head0"]])

    def test_identical_tree_selection_and_setup(self):
        trees = {"base0^{tree}": "abc", "head0^{tree}": "abc", "HEAD^{tree}": "abc"}
        for changed_tree, labels, noop in (
                ({}, (), True), ({"HEAD^{tree}": "other"}, (), False),
                ({"head0^{tree}": "other"}, (), False), ({"HEAD^{tree}": None}, (), False),
                ({}, ("ci:full-test",), False)):
            with self.subTest(changed_tree=changed_tree, labels=labels):
                selected = {**trees, **changed_tree}
                result, _, meta = self.run_script("pr-meta", event=_pull_request(labels),
                                                  config={"trees": selected})
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual("identical_tree=true" in meta, noop)
                result, calls, outputs = self.determine(changed="", trees=selected, labels=labels)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual("affected=" + _NO_AFFECTED_TARGETS in outputs, noop)
                self.assertEqual("fallback=true" in outputs, not noop)
                self.assertFalse(any(row[0] in ("java", "bazelisk") for row in calls))

    def test_a_failed_merge_base_diffs_from_the_base_tip(self):
        result, calls, _ = self.determine(changed="", merge_base=None)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("git merge-base failed; using base tip base0", result.stdout)
        self.assertIn(["git", "diff", "--name-only", "base0", "head0"], calls)

    def test_infra_and_workflow_logic_changes_fall_back_after_a_hashing_self_check(self):
        for changed, wgpu in ((".bazelversion\n", "false"), (".github/actions/x/action.yml\n",
                                                            "false"),
                              ("MODULE.bazel\n", "true"), (".github/workflows/main.yml\n", "true"),
                              (".github/workflows/pr_base_change.yml\n", "false"),
                              ("build_defs/rules.bzl\n", "true"),
                              ("build_defs/BUILD.bazel\n", "true"),
                              ("tools/ci/determine_targets.sh\n", "true"),
                              ("tools/ci/fetch_bazel_diff.sh\n", "true"),
                              ("tools/ci/bazel_watchdog.sh\n", "true")):
            with self.subTest(changed=changed):
                result, calls, outputs = self.determine(changed="docs/a.md\n" + changed)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assert_outputs(outputs, "full_test=false",
                                    "wgpu_reference_changed=" + wgpu, "fallback=true",
                                    "affected=")
                self.assertIn("Infra change requires full //... coverage: " + changed.strip(),
                              result.stdout)
                # The checkout is hashed once, the hashes are discarded and nothing is compared.
                java = [call for call in calls if call[0] == "java"]
                self.assertEqual(len(java), 1, java)
                self.assertEqual(java[0], _hashing(self.runner_temp, self.workspace.resolve(),
                                                   java[0][-1]))
                self.assertFalse(Path(java[0][-1]).exists())
                self.assertNotIn(["git", "worktree", "add"], [call[:3] for call in calls])

    def test_a_broken_hashing_fails_a_change_that_selects_full_coverage(self):
        # A change to the selection itself selects full coverage, so it must not skip the
        # hashing that every later pull request runs.
        for config in ({"hashes_fail": True}, {"query_error": True}):
            with self.subTest(config=config):
                result, _, outputs = self.determine(
                    changed="tools/ci/determine_targets.sh\n", **config)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn("::error::bazel-diff could not hash this revision", result.stdout)
                self.assertNotIn("fallback=", outputs)

    def test_every_hashing_is_strict_and_skips_the_external_package(self):
        result, calls, _ = self.determine()
        self.assertEqual(result.returncode, 0, result.stderr)
        hashing = [call for call in calls if "generate-hashes" in call]
        self.assertEqual([Path(call[-1]).name for call in hashing],
                         ["base-hashes.json", "head-hashes.json"])
        for call in hashing:
            self.assertEqual(call, _hashing(self.runner_temp, call[call.index("-w") + 1],
                                            call[-1]))

    def test_graph_tracked_build_defs_and_other_workflows_select_incrementally(self):
        # A lint script under build_defs is an ordinary source of its own tests, and another
        # workflow cannot change what this one runs; bazel-diff attributes both.
        for changed in ("build_defs/check_banned_patterns.py\n",
                        "build_defs/testdata/fixture.txt\n",
                        ".github/workflows/cmake.yml\n", ".github/workflows/editor_wasm.yml\n",
                        ".github/workflows/bcr_preflight.yml\n", ".github/workflows/lint.yml\n",
                        ".github/workflows/coverage.yml\n",
                        ".github/workflows/deploy_docs.yaml\n"):
            with self.subTest(changed=changed):
                result, calls, outputs = self.determine(changed=changed)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertNotIn("Infra change", result.stdout)
                self.assertIn("get-impacted-targets", [arg for call in calls for arg in call])
                self.assertTrue(outputs.endswith("fallback=false\naffected=//a:b\n"), outputs)

    def test_no_affected_targets_selects_nothing_when_bazel_diff_sees_every_change(self):
        for changed in ("package.json\n", "donner/base/Utils.h\ndocs/a.md\n",
                        ".github/workflows/lint.yml\n"):
            with self.subTest(changed=changed):
                result, calls, outputs = self.determine(changed=changed, affected="\n")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assert_outputs(outputs, "full_test=false", "wgpu_reference_changed=false",
                                    "browser_affected=false", "metal_affected=false",
                                    "fallback=false", "affected=" + _NO_AFFECTED_TARGETS)
                self.assertIn("nothing to build or test", result.stdout)
                self.assertEqual(self.queried(calls), [])

    def test_no_affected_targets_falls_back_for_inputs_bazel_diff_cannot_see(self):
        # Each reaches Bazel outside the hashed build graph: module extension manifests, build
        # and rule definitions, and source trees of local repositories under third_party.
        for changed in ("donner/editor/wasm/tests/pnpm-lock.yaml\n",
                        "donner/editor/wasm/tests/package.json\n", "MODULE.bazel.lock\n",
                        ".bazelignore\n", "donner/base/BUILD.bazel\n", "donner/BUILD\n",
                        "tools/defs.bzl\n", "third_party/BUILD.stb\n",
                        "third_party/entt/src/entt.hpp\n", "package.json\nexamples/x.BUILD\n",
                        "tools/ci_metal_toolchain.py\n", "tools/ci_macos_xcode.txt\n"):
            with self.subTest(changed=changed):
                result, _, outputs = self.determine(changed=changed, affected="\n")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertTrue(outputs.endswith("fallback=true\naffected=\n"), outputs)
                self.assertIn("bazel-diff produced no affected targets", result.stdout)

    def test_a_failed_graph_diff_fails_instead_of_selecting_nothing(self):
        for config in ({"hashes_fail": True}, {"impacted_fail": True}, {"query_error": True}):
            with self.subTest(config=config):
                result, _, outputs = self.determine(affected="\n", **config)
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn("fallback=false", outputs)
                self.assertNotIn(_NO_AFFECTED_TARGETS, outputs)

    def test_a_reference_suite_without_tests_fails(self):
        result, _, _ = self.determine(queries=_queries(**{_REFERENCE_QUERY: ("", 0)}))
        self.assertEqual(result.returncode, 1)
        self.assertIn("::error::Linux WebGPU resvg suite has no test target.", result.stdout)

    def test_only_manual_affected_targets_fall_back(self):
        result, _, outputs = self.determine(
            affected="//a:manual\n", queries=_queries(**{_MANUAL_QUERY: ("//a:manual\n", 0)}))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_outputs(outputs, "full_test=false", "wgpu_reference_changed=false",
                            "metal_affected=false", "fallback=true", "affected=")
        self.assertIn("Filtered 1 manual-tagged affected target(s).", result.stdout)
        self.assertIn("Only manual-tagged affected targets detected", result.stdout)

    def test_unlisted_browser_tests_for_changed_manifests_fall_back(self):
        for listing in (("", 0), ("//w:smoke\n", 3)):
            with self.subTest(listing=listing):
                result, _, outputs = self.determine(
                    changed="donner/editor/wasm/tests/package-lock.json\n",
                    queries=_queries(**{_MANIFEST_QUERY: listing}))
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assert_outputs(outputs, "full_test=false", "wgpu_reference_changed=false",
                                    "metal_affected=false", "fallback=true", "affected=")
                self.assertIn("Could not list browser tests", result.stdout)

    def test_a_selection_longer_than_a_lane_can_receive_falls_back(self):
        # 98304 bytes of labels is the bound: one byte more selects //... before any query.
        labels = ["//p:%s" % ("t" * 59)] * 1536
        for extra, falls_back in ((0, False), (1, False), (2, True)):
            selection = labels[:-1] + [labels[-1] + "x" * extra]
            with self.subTest(bytes=len(" ".join(selection))):
                self.assertEqual(len(" ".join(selection)), 98303 + extra)
                result, calls, outputs = self.determine(affected="\n".join(selection))
                self.assertEqual(result.returncode, 0, result.stderr)
                if falls_back:
                    self.assert_outputs(outputs, "full_test=false",
                                        "wgpu_reference_changed=false", "fallback=true",
                                        "affected=", "wgpu_reference_unknown=true")
                    self.assertIn("98305 bytes, more than the 98304 a CI lane", result.stdout)
                    self.assertEqual([], self.queried(calls))
                else:
                    self.assertIn("affected=" + " ".join(selection) + "\n", outputs)
                    self.assertNotIn("more than the", result.stdout)

    def test_browser_tests_added_past_the_bound_fall_back(self):
        label = "//p:%s" % ("t" * 59)
        manifest = "\n".join("//w:%s%d" % ("b" * 60, index) for index in range(20)) + "\n"
        result, calls, outputs = self.determine(
            changed="donner/editor/wasm/tests/package-lock.json\n",
            affected="\n".join([label] * 1520),
            queries=_queries(**{_MANIFEST_QUERY: (manifest, 0)}))
        self.assertEqual(result.returncode, 0, result.stderr)
        # 1520 labels fit (97279 bytes); the 20 browser tests take the list past the bound.
        self.assertTrue(outputs.endswith("fallback=true\naffected=\n"), outputs[-200:])
        self.assertIn("more than the 98304 a CI lane", result.stdout)
        self.assertFalse([query for query in self.queried(calls)
                          if 'attr("tags", "launches-browser", set(' in query])

    def test_an_affected_set_publishes_the_selection_and_lane_hints(self):
        affected = "//a:b\n" + _REFERENCE + "\n//a:manual\n@npm//:pkg+x\n"
        result, calls, outputs = self.determine(
            changed="donner/svg/renderer/Renderer.cc\ndonner/editor/wasm/tests/package.json\n",
            affected=affected, queries=_queries(**{_MANUAL_QUERY: ("//a:manual\n", 0)}))
        self.assertEqual(result.returncode, 0, result.stderr)
        selection = "//a:b %s @npm//:pkg+x //w:smoke //w:regression" % _REFERENCE
        self.assert_outputs(outputs, "full_test=false", "wgpu_reference_changed=true",
                            "wgpu_reference_affected=true", "metal_affected=false",
                            "browser_affected=true", "fallback=false", "affected=" + selection)
        queries = self.queried(calls)
        self.assertIn('attr("tags", "manual", set("//a:b" "%s" "//a:manual" "@npm//:pkg+x"))'
                      % _REFERENCE, queries)
        self.assertIn('attr("tags", "launches-browser", set(%s))' % selection, queries)
        # The Metal suites are intersected with the selection before manual targets drop out.
        before_manual = " ".join('"%s"' % label for label in affected.split())
        self.assertIn('set(%s) intersect tests(//tools/ci:metal_full_validation' % before_manual,
                      "\n".join(queries))
        # Every query runs in the merge commit's worktree, and bazel-diff hashes both sides.
        heads = [call for call in calls if call[:3] == ["git", "worktree", "add"]]
        self.assertEqual([call[-1] for call in heads], ["merge0", "base0"])
        hashed = [call[call.index("-w") + 1] for call in calls if call[0] == "java"]
        self.assertEqual([Path(path).name for path in hashed], ["base", "head", "head"])
        cached = self.home / ".cache/bazel-diff-base-hashes/base0.json"
        self.assertEqual(cached.read_text(), "{}")
        # The worktrees and their scratch directory are removed on exit.
        self.assertEqual([Path(call[-1]).name for call in calls
                          if call[:3] == ["git", "worktree", "remove"]], ["base", "head"])
        self.assertFalse(Path(heads[0][-2]).parent.exists())

    def test_cached_base_hashes_skip_the_base_worktree(self):
        cached = self.home / ".cache/bazel-diff-base-hashes/base0.json"
        cached.parent.mkdir(parents=True)
        cached.write_text('{"cached": 1}')
        result, calls, outputs = self.determine()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Using cached bazel-diff base hashes for base0.", result.stdout)
        self.assertEqual([call[-1] for call in calls if call[:3] == ["git", "worktree", "add"]],
                         ["merge0"])
        self.assertTrue(outputs.endswith("fallback=false\naffected=//a:b\n"), outputs)

    def test_lane_hints_follow_their_queries(self):
        hints = ("wgpu_reference_affected", "metal_affected", "browser_affected")
        cases = (
            ("none reached", {_REFERENCE_QUERY: ("//other:test\n", 0), _BROWSER_QUERY: ("", 0)},
             ("metal_affected=false", "browser_affected=false")),
            ("Metal suite reached",
             {_METAL_INTERSECTION_QUERY: ("//donner/gpu/metal/tests:metal_tests\n", 0)},
             ("wgpu_reference_affected=true", "metal_affected=true", "browser_affected=true")),
            # A failed or empty query selects its lane.
            ("queries failed", {_BROWSER_QUERY: ("", 1), _METAL_SUITE_QUERY: ("", 1)},
             ("wgpu_reference_affected=true", "metal_affected=true", "browser_affected=true")),
            ("no Metal suite tests", {_METAL_SUITE_QUERY: ("", 0)},
             ("wgpu_reference_affected=true", "metal_affected=true", "browser_affected=true")),
        )
        for name, overrides, expected in cases:
            with self.subTest(name):
                result, _, outputs = self.determine(affected=_REFERENCE + "\n",
                                                    queries=_queries(**overrides))
                self.assertEqual(result.returncode, 0, result.stderr)
                lines = [line for line in outputs.splitlines() if line.split("=")[0] in hints]
                self.assertEqual(tuple(lines), expected)
                self.assertTrue(outputs.endswith("fallback=false\naffected=%s\n" % _REFERENCE),
                                outputs)

    def test_metal_job_inputs_outside_bazel_select_the_metal_job(self):
        for changed in ("tools/ci_metal_toolchain.py\n", "third_party/BUILD.wgpu_native\n",
                        "MODULE.bazel.lock\n"):
            with self.subTest(changed=changed):
                result, calls, outputs = self.determine(changed=changed)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("metal_affected=true\n", outputs)
                self.assertNotIn(_METAL_SUITE_QUERY, "\n".join(self.queried(calls)))

    def test_pr_meta_names_the_revision_whose_hashes_are_cached(self):
        for name, env, expected in (
                ("pull request", {}, "base_sha=base0"),
                ("main push", {"GITHUB_EVENT_NAME": "push", "GITHUB_REF": "refs/heads/main"},
                 "base_sha=sha0"),
                ("other push", {"GITHUB_EVENT_NAME": "push", "GITHUB_REF": "refs/heads/x"},
                 "base_sha=")):
            with self.subTest(name):
                result, calls, outputs = self.run_script("pr-meta", event=_pull_request(), **env)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assert_outputs(outputs, expected)
                self.assertEqual(calls, [["git", "rev-parse", "--verify", "base0^{tree}"]]
                                 if name == "pull request" else [])

    def test_the_wgpu_reference_runs_when_any_signal_selects_it(self):
        for affected, changed, unknown, full_test, expected in (
            ("true", "false", "", "false", "true"),
            ("", "true", "", "false", "true"),
            ("", "false", "true", "false", "true"),
            ("", "false", "", "true", "true"),
            ("", "false", "", "false", "false"),
        ):
            with self.subTest(affected=affected, changed=changed, unknown=unknown,
                              full_test=full_test):
                result, _, outputs = self.run_script(
                    "select-wgpu-reference", WGPU_REFERENCE_AFFECTED=affected,
                    WGPU_REFERENCE_CHANGED=changed, WGPU_REFERENCE_UNKNOWN=unknown,
                    FULL_TEST=full_test)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assert_outputs(outputs, "run=" + expected)

    def test_bazel_run_selects_in_the_checkout(self):
        elsewhere = self.work / "runfiles"
        elsewhere.mkdir()
        cwd = Path(str(self.capture) + ".cwd")
        cwd.write_text("")
        for env, directory in (({}, self.workspace),
                               ({"BUILD_WORKSPACE_DIRECTORY": str(self.workspace)}, elsewhere)):
            with self.subTest(env=env):
                result, _, _ = self.run_script("determine", config={"changed": ""},
                                               event=_pull_request(), cwd=directory, **env)
                self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(set(cwd.read_text().splitlines()), {str(self.workspace.resolve())})

    def test_runs_outside_a_workflow_step(self):
        # Unset `determine` outputs read as empty, as a skipped step's outputs do.
        result, _, outputs = self.run_script("select-wgpu-reference")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_outputs(outputs, "run=false")
        result, calls, _ = self.run_script("pr-meta", GITHUB_OUTPUT="")
        self.assertEqual(result.returncode, 2)
        self.assertIn("set it.", result.stderr)
        self.assertEqual(calls, [])

    def test_rejects_an_unknown_subcommand(self):
        for args in ((), ("select",), ("determine", "extra")):
            with self.subTest(args=args):
                result, calls, outputs = self.run_script(*args)
                self.assertEqual(result.returncode, 2)
                self.assertIn("usage: tools/ci/determine_targets.sh", result.stderr)
                self.assertEqual((calls, outputs), ([], ""))


class FetchBazelDiffTest(_ScriptTest):
    script_name = "fetch_bazel_diff.sh"
    jar = "bazel-diff release"

    def fetch(self, *downloads):
        return self.run_script(config={"downloads": [list(item) for item in downloads]},
                               BAZEL_DIFF_TAG="v0.0.0",
                               BAZEL_DIFF_SHA256=hashlib.sha256(self.jar.encode()).hexdigest())

    def test_a_verified_download_succeeds_at_once(self):
        result, calls, _ = self.fetch((self.jar, 0))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(calls[0], ["curl", "-fsSL", "--retry", "4", "--retry-delay", "2",
                                    "--retry-all-errors", "--connect-timeout", "15",
                                    "--max-time", "300", "-o",
                                    str(self.runner_temp / "bazel-diff.jar"), _JAR_URL])
        self.assertEqual([call[0] for call in calls], ["curl", "sha256sum"])
        self.assertEqual((self.runner_temp / "bazel-diff.jar").read_text(), self.jar)

    def test_a_mismatched_or_failed_download_is_retried_with_backoff(self):
        result, calls, _ = self.fetch(("tampered", 0), ("", 22), (self.jar, 0))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("bazel-diff digest mismatch for v0.0.0; refusing artifact.", result.stderr)
        self.assertEqual([call for call in calls if call[0] == "sleep"],
                         [["sleep", "2"], ["sleep", "4"]])
        self.assertEqual((self.runner_temp / "bazel-diff.jar").read_text(), self.jar)

    def test_five_failed_attempts_fail_and_leave_no_artifact(self):
        result, calls, _ = self.fetch(*[("tampered", 0)] * 5)
        self.assertEqual(result.returncode, 1)
        self.assertIn("error: bazel-diff download failed after 5 attempts.", result.stderr)
        self.assertEqual([call for call in calls if call[0] == "sleep"],
                         [["sleep", "2"], ["sleep", "4"], ["sleep", "8"], ["sleep", "16"]])
        self.assertFalse((self.runner_temp / "bazel-diff.jar").exists())

    def test_rejects_arguments(self):
        result, calls, _ = self.run_script("extra")
        self.assertEqual(result.returncode, 2)
        self.assertEqual(calls, [])


if __name__ == "__main__":
    unittest.main()
