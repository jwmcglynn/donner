"""Pins review-driven security workflow selection and scheduling policy."""

import re
import unittest

from python.runfiles import runfiles


def _read(path):
    resolved = runfiles.Create().Rlocation("donner/%s" % path)
    with open(resolved, encoding="utf-8") as handle:
        return handle.read()


def _step_body(workflow, name):
    marker = "      - name: %s\n" % name
    if marker not in workflow:
        raise AssertionError("workflow step not found: %s" % name)
    body = workflow.split(marker, 1)[1]
    next_step = re.search(r"^      - name:", body, re.MULTILINE)
    return body[: next_step.start()] if next_step else body


class SecurityWorkflowPolicyTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.codeql = _read(".github/workflows/codeql.yml")
        cls.fuzz = _read(".github/workflows/fuzz.yml")
        cls.sanitizers = _read(".github/workflows/sanitizers.yml")

    def test_heavy_security_workflows_do_not_gate_pull_requests(self):
        self.assertNotRegex(self.codeql, r"(?m)^  pull_request:")
        self.assertNotRegex(self.fuzz, r"(?m)^  pull_request:")

    def test_codeql_builds_are_selected_by_bazel_tags(self):
        build = _step_body(self.codeql, "Build tagged C++ entry points")
        for tag in (
            "codeql_default",
            "codeql_text_full",
            "codeql_geode",
            "codeql_wasm",
            "codeql_wasm_geode",
        ):
            self.assertIn("--build_tag_filters=%s" % tag, build)
        self.assertNotRegex(build.replace("//...", ""), r"//[^\s]+")

    def test_fuzz_variants_are_selected_by_bazel_tags(self):
        expected = {
            "Test text-full fuzzers": "fuzz_text_full",
            "Test Geode render fuzzers": "fuzz_geode",
        }
        for step, tag in expected.items():
            body = _step_body(self.fuzz, step)
            self.assertIn("--test_tag_filters=%s" % tag, body)
            self.assertIn("--build_tag_filters=%s" % tag, body)
            self.assertNotRegex(body.replace("//...", ""), r"//[^\s]+")

    def test_ubsan_corpora_are_selected_by_bazel_tags(self):
        body = _step_body(self.sanitizers, "Replay untrusted-input fuzzer corpora with UBSan")
        for tag in ("fuzz_ubsan", "fuzz_ubsan_text_full", "fuzz_ubsan_geode"):
            self.assertIn("--test_tag_filters=%s" % tag, body)
            self.assertIn("--build_tag_filters=%s" % tag, body)
        self.assertNotRegex(body.replace("//...", ""), r"//[^\s]+")

if __name__ == "__main__":
    unittest.main()
