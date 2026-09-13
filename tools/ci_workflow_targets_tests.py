"""Keeps CI lane membership in Bazel metadata instead of workflow YAML."""

from pathlib import Path
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
