"""Pins review-driven security workflow selection and scheduling policy."""

from pathlib import Path
import json
import re
import unittest

from python.runfiles import runfiles


BAZEL_DIFF_TAG = "v18.1.0"
BAZEL_DIFF_SHA256 = "eadec6d0ca0991de78bd9e063bfed993a359ed3bc60e507818077c300b6f58e0"
CLOC_SHA256 = "73570f9da159fab13846038de7c3d8772554117c04117281dcbe6e5c7b988264"
DOXYGEN_SHA256 = "0ec2e5b2c3cd82b7106d19cb42d8466450730b8cb7a9e85af712be38bf4523a1"


def _read(path):
    resolved = runfiles.Create().Rlocation("donner/%s" % path)
    with open(resolved, encoding="utf-8") as handle:
        return handle.read()


def _read_supply_chain_files():
    resolver = runfiles.Create()
    main = Path(resolver.Rlocation("donner/.github/workflows/main.yml"))
    github = main.parent.parent
    paths = sorted((github / "workflows").glob("*.y*ml"))
    paths.extend(sorted((github / "actions").glob("*/action.y*ml")))
    return {
        str(path.relative_to(github.parent)): path.read_text(encoding="utf-8")
        for path in paths
    }


def _step_body(workflow, name):
    marker = "      - name: %s\n" % name
    if marker not in workflow:
        raise AssertionError("workflow step not found: %s" % name)
    body = workflow.split(marker, 1)[1]
    next_step = re.search(r"^      - name:", body, re.MULTILINE)
    return body[: next_step.start()] if next_step else body


def _run_bodies(workflow):
    lines = workflow.splitlines()
    bodies = []
    index = 0
    while index < len(lines):
        match = re.match(r"^(\s*)run:\s*\|", lines[index])
        if not match:
            index += 1
            continue
        indentation = len(match.group(1))
        index += 1
        body = []
        while index < len(lines):
            line = lines[index]
            if line and len(line) - len(line.lstrip()) <= indentation:
                break
            body.append(line)
            index += 1
        bodies.append("\n".join(body))
    return bodies


class SecurityWorkflowPolicyTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.codeql = _read(".github/workflows/codeql.yml")
        cls.fuzz = _read(".github/workflows/fuzz.yml")
        cls.sanitizers = _read(".github/workflows/sanitizers.yml")
        cls.supply_chain_files = _read_supply_chain_files()

    def test_external_actions_are_pinned_to_commits(self):
        """Every external action is immutable and remains update-tool friendly."""
        action_line = re.compile(
            r"^\s*(?:-\s*)?uses:\s*(?P<target>\S+?)(?:\s+#\s*(?P<version>\S+))?\s*$"
        )
        full_commit = re.compile(r"^[0-9a-f]{40}$")
        version_comment = re.compile(r"^v?\d+(?:\.\d+){0,2}$")
        pins = {}
        seen = 0

        for path, contents in self.supply_chain_files.items():
            for line_number, line in enumerate(contents.splitlines(), start=1):
                match = action_line.match(line)
                if not match or "@" not in match.group("target"):
                    continue

                target, revision = match.group("target").rsplit("@", 1)
                if target.startswith("./") or target.startswith("docker://"):
                    continue

                version = match.group("version")
                with self.subTest(path=path, line=line_number, action=target):
                    self.assertRegex(
                        revision,
                        full_commit,
                        "%s:%d uses a mutable external action revision" % (path, line_number),
                    )
                    self.assertIsNotNone(
                        version,
                        "%s:%d needs a trailing version comment for dependency updates"
                        % (path, line_number),
                    )
                    self.assertRegex(version, version_comment)

                pins.setdefault((target, version), set()).add(revision)
                seen += 1

        self.assertGreaterEqual(seen, 75, "external action discovery no longer covers the workflows")
        for (target, version), revisions in pins.items():
            self.assertEqual(
                1,
                len(revisions),
                "%s %s is pinned inconsistently: %s"
                % (target, version, sorted(revisions)),
            )

    def test_downloaded_bazel_diff_is_verified_before_execution(self):
        """The downloaded JAR must match the digest published for its release."""
        for path in (".github/workflows/main.yml", ".github/workflows/coverage.yml"):
            workflow = self.supply_chain_files[path]
            with self.subTest(path=path):
                self.assertIn('BAZEL_DIFF_TAG: "%s"' % BAZEL_DIFF_TAG, workflow)
                self.assertIn('BAZEL_DIFF_SHA256: "%s"' % BAZEL_DIFF_SHA256, workflow)

                download = _step_body(workflow, "Download bazel-diff")
                verification = "sha256sum --check --status"
                self.assertIn("$BAZEL_DIFF_SHA256", download)
                self.assertIn(verification, download)
                self.assertLess(download.index(verification), download.index("exit 0"))
                self.assertIn('rm -f "$out"', download)

    def test_downloaded_tools_are_verified_before_extraction(self):
        cases = (
            (".github/workflows/badges.yaml", "Install cloc", CLOC_SHA256),
            (".github/workflows/deploy_docs.yaml", "Install Doxygen", DOXYGEN_SHA256),
        )
        for path, step, digest in cases:
            with self.subTest(path=path):
                body = _step_body(self.supply_chain_files[path], step)
                self.assertIn(digest, body)
                verification = "sha256sum --check --strict"
                self.assertIn(verification, body)
                self.assertLess(body.index(verification), body.index("tar "))

    def test_release_builds_once_then_attests_and_publishes(self):
        release = self.supply_chain_files[".github/workflows/release.yml"]
        self.assertIn("types: [published]", release)
        self.assertNotIn("edited", release)
        self.assertNotIn("workflow_dispatch", release)
        self.assertIn("permissions:\n  contents: read", release)
        self.assertIn("publish-release-artifacts:", release)
        self.assertIn("contents: write", release)
        self.assertIn("id-token: write", release)
        self.assertIn("attestations: write", release)
        self.assertIn("actions/upload-artifact@", release)
        self.assertIn("actions/download-artifact@", release)
        self.assertIn("actions/attest-build-provenance@", release)
        self.assertIn("sha256sum --check --strict", release)
        self.assertGreaterEqual(release.count("if: github.run_attempt == 1"), 3)
        self.assertEqual(release.count("--lockfile_mode=error"), 2)
        self.assertIn("cmp --silent expected.provenance release/linux/", release)
        self.assertIn("cmp --silent expected.provenance release/macos/", release)
        self.assertIn("RELEASE_UPLOAD_URL: ${{ github.event.release.upload_url }}", release)
        self.assertIn("Upload verified release artifacts without replacement", release)
        self.assertIn("gh api --method POST", release)
        self.assertNotIn("softprops/action-gh-release", release)
        self.assertNotIn("publish-to-bcr", release)
        self.assertNotIn("svenstaro/upload-release-action", release)
        for body in _run_bodies(release):
            self.assertNotIn("${{", body, "release run blocks must use quoted environment values")

        lock = json.loads(_read("MODULE.bazel.lock"))
        self.assertGreaterEqual(lock["lockFileVersion"], 1)
        ignored = set(_read(".gitignore").splitlines())
        self.assertNotIn("MODULE.bazel.lock", ignored)

    def test_git_dependencies_use_immutable_commits(self):
        deps = _read("third_party/bazel/non_bcr_deps.bzl")
        self.assertNotRegex(deps, r"(?m)^\s*tag\s*=")
        for block in re.findall(r"(?:new_)?git_repository\((.*?)\n\s*\)", deps, re.DOTALL):
            if "remote =" not in block:
                continue
            self.assertRegex(block, r'commit\s*=\s*"[0-9a-f]{40}"')

    def test_fuzzer_variant_tags_derive_matching_ubsan_lanes(self):
        rules = _read("build_defs/rules.bzl")
        self.assertIn('"fuzz_text_full" in common_tags', rules)
        self.assertIn('ubsan_tags.append("fuzz_ubsan_text_full")', rules)
        self.assertIn('"fuzz_geode" in common_tags', rules)
        self.assertIn('ubsan_tags.append("fuzz_ubsan_geode")', rules)

    def test_pull_requests_never_receive_the_buildbuddy_write_key(self):
        guarded = (
            "github.event_name != 'pull_request' && "
            "github.event_name != 'pull_request_target'"
        )
        self.assertNotIn(
            "secrets.BUILDBUDDY_API_KEY",
            self.supply_chain_files[".github/workflows/sanitizers-pr.yml"],
        )
        for path, workflow in self.supply_chain_files.items():
            if re.search(r"(?m)^  pull_request:", workflow):
                with self.subTest(path=path, invariant="no-pr-cache-secret"):
                    self.assertNotIn("secrets.BUILDBUDDY_API_KEY", workflow)
        for path, workflow in self.supply_chain_files.items():
            if "uses: ./.github/actions/buildbuddy-cache" not in workflow:
                continue
            bodies = workflow.split("      - name: Configure BuildBuddy remote cache\n")[1:]
            for index, body in enumerate(bodies):
                step = body.split("\n      - name:", 1)[0]
                with self.subTest(path=path, step=index):
                    self.assertIn(guarded, step)
                    self.assertIn("secrets.BUILDBUDDY_API_KEY", step)

    def test_sensitive_workflows_use_least_privilege_and_drop_checkout_credentials(self):
        paths = (
            ".github/workflows/badges.yaml",
            ".github/workflows/cmake.yml",
            ".github/workflows/lint.yml",
            ".github/workflows/release.yml",
            ".github/workflows/sanitizers-pr.yml",
        )
        for path in paths:
            workflow = self.supply_chain_files[path]
            with self.subTest(path=path):
                self.assertRegex(workflow, r"(?m)^permissions:\n  contents: read$")
                checkout_count = workflow.count("uses: actions/checkout@")
                self.assertGreater(checkout_count, 0)
                self.assertEqual(checkout_count, workflow.count("persist-credentials: false"))

        for path, workflow in self.supply_chain_files.items():
            checkout_count = workflow.count("uses: actions/checkout@")
            if checkout_count == 0:
                continue
            with self.subTest(path=path, invariant="checkout-credentials"):
                self.assertEqual(checkout_count, workflow.count("persist-credentials: false"))

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
