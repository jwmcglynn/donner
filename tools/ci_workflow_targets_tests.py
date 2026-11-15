"""Keeps CI lane membership in Bazel metadata instead of workflow YAML."""

from pathlib import Path
import json
import re
import unittest

from python.runfiles import runfiles
from tools.ci import lane as lane_entry
from tools.ci import lanes


LABEL = re.compile(r"(?<![\w:/])(?P<option>--)?(?P<label>//[\w./:+*=-]+)")


def individual_targets(source):
    """Find concrete target selections, excluding comments and build settings."""
    findings = []
    for number, line in enumerate(source.splitlines(), 1):
        if line.lstrip().startswith("#"):
            continue
        for match in LABEL.finditer(line):
            label = match.group("label")
            if match.group("option") or label.startswith("//tools/ci:"):
                continue
            if label.endswith(("/...", ":all", ":*")) or label.rstrip(".") == "//":
                continue
            findings.append((number, label))
    return findings


def lane_commands(lane, *commands):
    """The argv of every Bazel command a lane's phases run, from their dry runs; with `commands`,
    only those Bazel commands (build, test)."""
    found = []
    for phase in lanes.LANES[lane].phases:
        for action in lane_entry.dry_run(lane, phase, {})["actions"]:
            if action["action"] != "bazel":
                continue
            argv = action["argv"][action["argv"].index("bazelisk"):]
            command = next(word for word in argv[1:] if not word.startswith("--"))
            if not commands or command in commands:
                found.append(argv)
    return found


class WorkflowTargetsTest(unittest.TestCase):
    maxDiff = None

    def test_workflow_lane_membership_comes_from_bazel_metadata(self):
        resolver = runfiles.Create()
        directory = Path(resolver.Rlocation("donner/.github/workflows/main.yml")).parent
        paths = sorted(directory.glob("*.yml")) + sorted(directory.glob("*.yaml"))
        self.assertGreater(len(paths), 0, "workflow runfiles are missing")
        # The lane table holds the commands the workflow's Bazel steps run.
        paths.append(Path(resolver.Rlocation("donner/tools/ci/lanes.py")))
        findings = [
            f"{path.name}:{line}: {label}"
            for path in paths
            for line, label in individual_targets(path.read_text(encoding="utf-8"))
        ]
        self.assertEqual(findings, [], "Move individual CI targets into //tools/ci BUILD metadata")

    def test_filtered_workflows_include_their_ci_metadata(self):
        resolver = runfiles.Create()
        directory = Path(resolver.Rlocation("donner/.github/workflows/main.yml")).parent
        for path in sorted(directory.glob("*.yml")) + sorted(directory.glob("*.yaml")):
            source = path.read_text(encoding="utf-8")
            header = source.split("\njobs:", 1)[0]
            if "//tools/ci:" in source and re.search(r"(?m)^\s+paths:\s*$", header):
                with self.subTest(workflow=path.name):
                    self.assertIn('"tools/ci/**"', header)

    def test_required_pr_workflows_admit_stacked_base_branches(self):
        resolver = runfiles.Create()
        workflows = ("main.yml", "cmake.yml", "lint.yml", "sanitizers-pr.yml")
        for workflow in workflows:
            with self.subTest(workflow=workflow):
                source = Path(resolver.Rlocation(f"donner/.github/workflows/{workflow}"))
                header = source.read_text(encoding="utf-8").split("\njobs:", 1)[0]
                trigger = re.search(
                    r"(?m)^  pull_request:[^\n]*\n(?P<body>(?:^    [^\n]*\n|^\s*\n)*)",
                    header,
                )
                self.assertIsNotNone(trigger, "required pull_request trigger is missing")
                self.assertNotRegex(
                    trigger.group("body"),
                    r"(?m)^    branches(?:-ignore)?:",
                    "Required PR checks must admit dependency branches as well as main",
                )

    def test_vm_and_native_metal_routes_preserve_capabilities(self):
        resolver = runfiles.Create()
        source = Path(resolver.Rlocation("donner/.github/workflows/main.yml")).read_text()
        jobs = {m.group(1): m.group(2) for m in re.finditer(
            r"(?m)^  ([A-Za-z0-9_-]+):\n((?:(?:    .*|[ \\t]*)\n)*)", source)}
        for name in ("macos-self-hosted",):
            with self.subTest(job=name):
                self.assertIn("runs-on: [self-hosted, macOS-vm, ARM64]", jobs[name])
                for command in lane_commands(name, "test"):
                    self.assertIn("--//build_defs:metal_shader_validation=off", command)
                self.assertIn("needs.gatekeeper.outputs.use_self_hosted_macos == 'true'", jobs[name])
        native = jobs["macos-metal"]
        self.assertIn("runs-on: [self-hosted, macOS, ARM64]", native)
        self.assertIn("needs.gatekeeper.outputs.use_self_hosted_macos == 'true'", native)
        # Only pull requests that reach the Metal suites, or fall back to //..., run it.
        self.assertIn("(needs.determine-targets.outputs.fallback == 'true' || "
                      "needs.determine-targets.outputs.metal_affected == 'true')", native)
        native_commands = lane_commands("macos-metal", "test")
        self.assertEqual([command[-1] for command in native_commands], [
            "//tools/ci:" + target for target in ("metal_full_validation", "metal_preflight",
                                                  "metal_offline_compiler", "metal_native")])
        for command in native_commands:
            self.assertIn("--test_env=MTL_SHADER_VALIDATION_TEXTURE_USAGE=1", command)
            self.assertNotIn("--//build_defs:metal_shader_validation=off", command)
        self.assertNotIn("continue-on-error:", native)
        self.assertIn("macos-metal", jobs["coverage-contract"].split("steps:", 1)[0])
        commands = lane_commands("macos-self-hosted", "test", "build")
        self.assertGreaterEqual(len(commands), 6)
        for command in commands:
            self.assertIn("--config=macos-vm", command)
            self.assertNotRegex(" ".join(command), r"--jobs|--local_resources")
        # Local execution stays bounded by the VM's CPU and memory budget. --jobs sits above the
        # CPU budget so cache lookups and downloads are not throttled to local concurrency.
        bazelrc = Path(resolver.Rlocation("donner/.bazelrc")).read_text()
        budget = re.findall(r"(?m)^build:macos-vm (.+)$", bazelrc)
        self.assertEqual(budget[1:], ["--local_resources=cpu=6",
                                      "--local_resources=memory=6144"])
        jobs_flag = re.fullmatch(r"--jobs=(\d+)", budget[0])
        self.assertIsNotNone(jobs_flag, budget)
        self.assertGreater(int(jobs_flag.group(1)), 6)
        manifest = json.loads(Path(resolver.Rlocation(
            "donner/tools/ci/coverage_contract.json")).read_text())
        for kind in ("shader-validation", "shader-memory-canary", "native-metal"):
            self.assertEqual(manifest["lanes"]["macos"]["Test"][kind]["counterparts"][0]["job"],
                             "macos-metal")

    def test_vm_qualification_is_an_explicit_trusted_four_job_matrix(self):
        source = Path(runfiles.Create().Rlocation("donner/.github/workflows/main.yml")).read_text()
        qualification = source.split("  macos-vm-qualification:\n", 1)[1].split("\n  macos-metal:", 1)[0]
        self.assertIn("runs-on: [self-hosted, macOS-vm-qualification, ARM64]", qualification)
        self.assertIn("needs.gatekeeper.outputs.use_self_hosted_macos == 'true'", qualification)
        self.assertIn("contains(github.event.pull_request.labels.*.name, 'ci:vm-qualification')",
                      qualification)
        self.assertIn("instance: [1, 2, 3, 4]", qualification)
        self.assertIn("fail-fast: false", qualification)
        [command] = lane_commands("macos-vm-qualification", "test")
        self.assertIn("--nocache_test_results", command)
        self.assertIn("--noskip_incompatible_explicit_targets", command)
        self.assertEqual("//tools/ci:macos_vm_qualification", command[-1])
        self.assertIn("--build_event_json_file=$RUNNER_TEMP/coverage-qualification.bep.json",
                      command)
        self.assertIn("vm-qualification-evidence-", qualification)
        self.assertIn("run: python3 tools/ci/lane.py phase macos-vm-qualification "
                      "require_executed", qualification)
        self.assertEqual(
            lanes.LANES["macos-vm-qualification"].phases["require_executed"].actions,
            (lanes.RequireExecuted("qualification", 4,
                                   "Expected four executed qualification tests"),))

    def test_native_suite_keeps_every_shader_validated_device_test(self):
        resolver = runfiles.Create()
        metal = Path(resolver.Rlocation("donner/donner/gpu/metal/tests/BUILD.bazel")).read_text()
        suites = Path(resolver.Rlocation("donner/tools/ci/BUILD.bazel")).read_text()
        expected = set()
        for block in re.findall(r"(?ms)^(?:donner_)?cc_test\(\n(.*?)^\)", metal):
            if ("_METAL_API_VALIDATION_ENV" in block or "_METAL_API_ONLY_VALIDATION_ENV" in block
                    or '":metal_device_gate"' in block):
                name = re.search(r'name = "([^"]+)"', block).group(1)
                if 'variants = ["geode"]' in block:
                    name += "_geode"
                expected.add("//donner/gpu/metal/tests:" + name)
        self.assertGreater(len(expected), 6)
        native = re.search(r'(?ms)test_suite\(\n    name = "metal_native",\n(.*?)^\)', suites).group(1)
        preflight = re.search(r'(?ms)test_suite\(\n    name = "metal_preflight",\n(.*?)^\)', suites).group(1)
        covered = set(re.findall(r'"(//[^"]+)"', native + preflight))
        self.assertEqual(expected - covered, set())

    def test_guard_covers_multiline_lists_queries_and_implicit_targets(self):
        source = '''targets: >-
  //donner/foo:one
  -//donner/foo:two
run: bazel build //donner/bar
run: bazel cquery 'deps(//donner/foo:three)'
'''
        self.assertEqual(individual_targets(source), [
            (2, "//donner/foo:one"), (3, "//donner/foo:two"),
            (4, "//donner/bar"), (5, "//donner/foo:three"),
        ])

    def test_guard_allows_suites_patterns_build_settings_and_urls(self):
        source = '''# Example: //path:target
bazel test //tools/ci:editor_geode //... //donner/... //donner/foo:all //donner/foo:*
bazel test --//donner/foo:feature=true
url: https://example.com/path
'''
        self.assertEqual(individual_targets(source), [])


if __name__ == "__main__":
    unittest.main()
