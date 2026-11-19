"""Keeps CI lane membership in Bazel metadata instead of workflow YAML."""

from pathlib import Path
import json
import re
import unittest

from python.runfiles import runfiles
from tools import bazelrc_configs
from tools.ci import lane_view
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


def lane_commands(lane, *commands, expand=True):
    """Every Bazel command a lane's phases run, from their dry runs, without wrappers and, unless
    `expand` is false, with the repository's .bazelrc configs expanded; with `commands`, only
    those Bazel commands (build, test)."""
    configs = expand and bazelrc_configs.parse(
        Path(runfiles.Create().Rlocation("donner/.bazelrc")).read_text(encoding="utf-8")) or None
    found = [argv[argv.index("bazelisk"):] for phase in lanes.LANES[lane].phases
             for argv in lane_view.phase_commands(lane, phase, configs) if "bazelisk" in argv]
    return [argv for argv in found if _command(argv) != "query"
            and (not commands or _command(argv) in commands)]


def _command(argv):
    return next(word for word in argv[1:] if not word.startswith("--"))


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

    def test_mixed_darwin_routes_preserve_native_metal_and_geode(self):
        resolver = runfiles.Create()
        source = Path(resolver.Rlocation("donner/.github/workflows/main.yml")).read_text()
        native = source.split("      - name: Test Linux and macOS with native platform transitions", 1)[1]
        native = native.split("      - name:", 1)[0]
        self.assertIn("if: needs.gatekeeper.outputs.unified_macos == 'true'", native)
        self.assertIn("RUN_METAL: ${{ needs.determine-targets.outputs.metal_affected == 'true' || "
                      "needs.determine-targets.outputs.fallback == 'true' }}", native)
        self.assertIn('if [[ "$RUN_METAL" == true ]]; then', native)
        self.assertIn("suites+=(//tools/ci:unified_metal_{full_validation,preflight,offline_compiler,native})", native)
        self.assertIn("suites+=(//tools/ci:unified_editor_geode)", native)
        self.assertIn('verify_unified_tests.py --ordinary "$ordinary" --linux "$linux" "$bep" "${suites[@]}"', native)
        self.assertNotIn("runs-on: [self-hosted, macOS", source)

    def test_worker_qualification_uses_only_its_own_four_run_roots(self):
        resolver = runfiles.Create()
        source = Path(resolver.Rlocation("donner/.github/workflows/main.yml")).read_text()
        self.assertIn("WORKER_QUALIFICATION: ${{ contains(github.event.pull_request.labels.*.name, "
                      "'ci:vm-qualification') }}", source)
        block = source.split('if [[ "$WORKER_QUALIFICATION" == true ]]; then', 1)[1].split("\n          fi", 1)[0]
        self.assertIn("suites+=(//tools/ci:unified_worker_qualification)", block)
        self.assertIn("options+=(--runs_per_test='.*:ci_darwin_qualify_.*@4')", block)
        suites = Path(resolver.Rlocation("donner/tools/ci/BUILD.bazel")).read_text()
        original = suites.split('name = "macos_vm_qualification",', 1)[1].split("\n)", 1)[0]
        mixed = suites.split('name = "unified_worker_qualification",', 1)[1].split("\n)", 1)[0]
        labels = re.findall(r'"(//[^"]+)"', original)
        self.assertEqual(len(labels), 4)
        self.assertEqual({label.replace(":", ":ci_darwin_qualify_") for label in labels},
                         set(re.findall(r'"(//[^"]+)"', mixed)))

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
