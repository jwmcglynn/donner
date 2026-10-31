"""Keeps CI lane membership in Bazel metadata instead of workflow YAML."""

from pathlib import Path
import json
import re
import unittest

from python.runfiles import runfiles


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


class WorkflowTargetsTest(unittest.TestCase):
    maxDiff = None

    def test_workflow_lane_membership_comes_from_bazel_metadata(self):
        resolver = runfiles.Create()
        directory = Path(resolver.Rlocation("donner/.github/workflows/main.yml")).parent
        paths = sorted(directory.glob("*.yml")) + sorted(directory.glob("*.yaml"))
        self.assertGreater(len(paths), 0, "workflow runfiles are missing")
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
        for name in ("macos-self-hosted", "macos-hosted-parity"):
            with self.subTest(job=name):
                self.assertIn("runs-on: [self-hosted, macOS-vm, ARM64]", jobs[name])
                self.assertIn("--//build_defs:metal_shader_validation=off", jobs[name])
                self.assertIn("needs.gatekeeper.outputs.use_self_hosted_macos == 'true'", jobs[name])
        native = jobs["macos-metal"]
        self.assertIn("runs-on: [self-hosted, macOS, ARM64]", native)
        self.assertIn("needs.gatekeeper.outputs.use_self_hosted_macos == 'true'", native)
        for target in ("metal_full_validation", "metal_preflight", "metal_native",
                       "metal_offline_compiler"):
            self.assertIn("//tools/ci:" + target, native)
        self.assertIn("--test_env=MTL_SHADER_VALIDATION_TEXTURE_USAGE=1", native)
        self.assertNotIn("metal_shader_validation=off", native)
        self.assertNotIn("continue-on-error:", native)
        self.assertIn("macos-metal", jobs["coverage-contract"].split("steps:", 1)[0])
        fast = jobs["macos-self-hosted"]
        for flag in ("--jobs=4", "--local_resources=cpu=4", "--local_resources=memory=4096"):
            for command in re.findall(r"(?m)^.*bazelisk .* (?:test|build) .*", fast):
                self.assertIn(flag, command)
        manifest = json.loads(Path(resolver.Rlocation(
            "donner/tools/ci/coverage_contract.json")).read_text())
        for kind in ("shader-validation", "shader-memory-canary", "native-metal"):
            self.assertEqual(manifest["lanes"]["macos"]["Test"][kind]["counterparts"][0]["job"],
                             "macos-metal")

    def test_vm_qualification_is_an_explicit_trusted_six_job_matrix(self):
        source = Path(runfiles.Create().Rlocation("donner/.github/workflows/main.yml")).read_text()
        qualification = source.split("  macos-vm-qualification:\n", 1)[1].split("\n  macos-metal:", 1)[0]
        self.assertIn("runs-on: [self-hosted, macOS-vm-qualification, ARM64]", qualification)
        self.assertIn("needs.gatekeeper.outputs.use_self_hosted_macos == 'true'", qualification)
        self.assertIn("contains(github.event.pull_request.labels.*.name, 'ci:vm-qualification')",
                      qualification)
        self.assertIn("instance: [1, 2, 3, 4, 5, 6]", qualification)
        self.assertIn("fail-fast: false", qualification)
        self.assertIn("--nocache_test_results", qualification)
        self.assertIn("--noskip_incompatible_explicit_targets", qualification)
        self.assertIn("//tools/ci:macos_vm_qualification", qualification)
        self.assertIn('--build_event_json_file="$RUNNER_TEMP/coverage-qualification.bep.json"', qualification)
        self.assertIn("vm-qualification-evidence-", qualification)
        self.assertIn('step.get("executed", 0) != 4', qualification)

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
