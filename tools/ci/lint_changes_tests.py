"""Changed-file lint works with only the merge tree and its two parents."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import lint_changes as lint


def git(*args):
    return subprocess.check_output(["git", *args], text=True, stderr=subprocess.DEVNULL).strip()


class LintChangesTest(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.TemporaryDirectory()
        self.addCleanup(self.work.cleanup)
        original = Path.cwd()
        self.addCleanup(os.chdir, original)
        self.source = Path(self.work.name, "source")
        self.source.mkdir()
        os.chdir(self.source)
        git("init", "-b", "main")
        git("config", "user.email", "tests@example.invalid")
        git("config", "user.name", "Test")
        Path("shared.txt").write_text("common")
        git("add", "--", ".")
        git("commit", "-m", "base")
        self.first = git("rev-parse", "HEAD")
        git("checkout", "-b", "feature")
        Path("new \nfile.py").write_text("value = 1")
        git("add", "--", ".")
        git("commit", "-m", "feature")
        self.feature = git("rev-parse", "HEAD")
        git("checkout", "main")
        Path("base-only").write_text("main")
        git("add", "--", ".")
        git("commit", "-m", "main advanced")
        self.base = git("rev-parse", "HEAD")
        git("checkout", "-b", "integration")
        git("merge", "--no-ff", "feature", "-m", "merge")
        self.head = git("rev-parse", "HEAD")
        target = Path(self.work.name, "shallow")
        git("clone", "--no-local", "--depth=2", self.source.as_uri(), str(target))
        os.chdir(target)

    def test_merge_uses_its_first_parent_and_scans_the_feature_only(self):
        self.assertEqual(git("rev-parse", "--is-shallow-repository"), "true")
        event = {"pull_request": {"base": {"sha": self.base}, "head": {"sha": self.feature}}}
        base = lint.base_tree("pull_request", event, self.head)
        self.assertEqual(lint.changed_paths(base), ["new \nfile.py"])
        phases = list(lint.commands(lint.changed_paths(base), base))
        self.assertEqual(len(phases), 2)
        self.assertEqual([command[-1] for _, command in phases], ["new \nfile.py"] * 2)
        self.assertFalse(lint.requires_bazel(lint.changed_paths(base)))

    def test_base_branch_advanced_after_the_payload_base(self):
        # GitHub merges into the base branch tip of when the run starts. The payload keeps the
        # older base commit, which this depth-two checkout does not even contain.
        event = {"pull_request": {"base": {"sha": self.first}, "head": {"sha": self.feature}}}
        base = lint.base_tree("pull_request", event, self.head)
        self.assertEqual(base, self.base)
        self.assertEqual(lint.changed_paths(base), ["new \nfile.py"])
        self.assertNotEqual(subprocess.run(["git", "cat-file", "-e", self.first],
                                           stderr=subprocess.DEVNULL).returncode, 0)

    def test_checkout_must_be_the_event_commit(self):
        event = {"pull_request": {"base": {"sha": self.base}, "head": {"sha": self.feature}}}
        with self.assertRaisesRegex(ValueError, f"Checkout {self.head} is not the event commit {self.base}"):
            lint.base_tree("pull_request", event, self.base)

    def test_merge_second_parent_must_be_the_payload_head(self):
        for head in (self.base, self.first):
            event = {"pull_request": {"base": {"sha": self.base}, "head": {"sha": head}}}
            with self.subTest(head=head), self.assertRaisesRegex(
                    ValueError, f"Checkout {self.head} is not a merge of pull request head {head} "
                                f"\\(parents: {self.base} {self.feature}\\)"):
                lint.base_tree("pull_request", event, self.head)

    def test_non_merge_checkout_is_rejected(self):
        git("config", "user.email", "tests@example.invalid")
        git("config", "user.name", "Test")
        git("commit", "--allow-empty", "-m", "single parent")
        single = git("rev-parse", "HEAD")
        event = {"pull_request": {"base": {"sha": self.base}, "head": {"sha": self.head}}}
        with self.assertRaisesRegex(ValueError, f"Checkout {single} is not a merge of pull request head "
                                                f"{self.head} \\(parents: {self.head}\\)"):
            lint.base_tree("pull_request", event, single)
        # The shallow boundary commit shows no parents at all.
        git("checkout", "--detach", self.feature)
        event["pull_request"]["head"]["sha"] = self.feature
        with self.assertRaisesRegex(ValueError, "\\(parents: none\\)"):
            lint.base_tree("pull_request", event, self.feature)

    def test_batch_push_fetches_the_exact_missing_before_commit(self):
        before = {"before": self.first, "after": self.head}
        self.assertNotEqual(subprocess.run(["git", "cat-file", "-e", self.first],
                                           stderr=subprocess.DEVNULL).returncode, 0)
        self.assertEqual(lint.base_tree("push", before, self.head), self.first)
        self.assertEqual(lint.changed_paths(self.first), ["base-only", "new \nfile.py"])
        self.assertEqual(git("rev-parse", "--is-shallow-repository"), "true")

    def test_new_branch_and_initial_push(self):
        event = {"before": lint.ZERO, "after": self.head, "repository": {"default_branch": "main"},
                 "ref": "refs/heads/main"}
        self.assertEqual(set(lint.changed_paths(lint.base_tree("push", event, self.head))),
                         {"new \nfile.py", "base-only", "shared.txt"})
        event["ref"] = "refs/heads/feature"
        real = subprocess.run
        def recorded(command, **kwargs):
            if command[0] == "gh":
                return subprocess.CompletedProcess(command, 0, json.dumps({"merge_base_commit": {"sha": self.first}}))
            return real(command, **kwargs)
        with mock.patch.object(subprocess, "run", side_effect=recorded):
            self.assertEqual(lint.base_tree("push", event, self.head), self.first)

    def test_orphan_and_api_failure_are_distinct(self):
        with mock.patch.object(subprocess, "run", return_value=subprocess.CompletedProcess(
                ["gh"], 1, '{"message":"No common ancestor between main and feature."}')), \
                mock.patch.object(lint, "empty_tree", return_value="empty"):
            self.assertEqual(lint.branch_fork_point("main", self.head), "empty")
        with mock.patch.object(subprocess, "run", return_value=subprocess.CompletedProcess(
                ["gh"], 1, '{"message":"Not Found"}')), self.assertRaises(subprocess.CalledProcessError):
            lint.branch_fork_point("main", self.head)

    def test_changed_policy_rechecks_its_governed_inventory(self):
        docs = "docs/design_docs/0001-edited.md"
        paths = [docs, "tools/check_design_doc_index.py", "tools/check_design_doc_provenance.py",
                 "build_defs/check_local_gpu_tags.py", "tools/check_private_details.py",
                 "build_defs/check_text_tier_defines.py"]
        checks = dict(lint.commands(paths, self.base))
        self.assertNotIn(docs, checks["Check design document provenance"])
        self.assertNotIn(docs, checks["Check design document index"])
        self.assertIn("Check GPU scheduling policy", checks)
        self.assertIn("Check text tier macro policy", checks)
        self.assertIn("Check tracked files against changed privacy rules", checks)

    def test_changed_starlark_rechecks_variant_transitions(self):
        command = [sys.executable, "build_defs/check_variant_transitions.py"]
        for path in ("build_defs/rules.bzl", "build_defs/check_variant_transitions.py"):
            with self.subTest(path=path):
                self.assertEqual(dict(lint.commands([path], self.base)).get("Check variant transitions"),
                                 command)
        self.assertNotIn("Check variant transitions", dict(lint.commands(["shared.txt"], self.base)))

    def test_changed_routing_file_rechecks_gpu_routing(self):
        command = [sys.executable, "build_defs/check_gpu_routing.py"]
        for path in (".bazelrc", "build_defs/rules.bzl", ".github/workflows/main.yml",
                     "build_defs/check_gpu_routing.py"):
            with self.subTest(path=path):
                self.assertEqual(dict(lint.commands([path], self.base)).get("Check remote GPU routing"),
                                 command)
        self.assertNotIn("Check remote GPU routing", dict(lint.commands(["shared.txt"], self.base)))

    def test_changed_gitlink_name_is_checked_without_following_it(self):
        git("config", "user.email", "tests@example.invalid")
        git("config", "user.name", "Test")
        git("update-index", "--add", "--cacheinfo", "160000," + self.feature + ",new-link")
        git("commit", "-m", "new link")
        checks = dict(lint.commands(lint.changed_paths(self.head), self.head))
        self.assertEqual(checks["Check changed tracked files for private details"][-1], "new-link")
        self.assertTrue(lint.requires_bazel([".clang-format"]))

    def test_failed_privacy_check_only_propagates_its_status(self):
        scripts = Path("tools/ci")
        scripts.mkdir(parents=True, exist_ok=True)
        (scripts / "lane.sh").write_text('lane_phase() { shift; "$@"; }\n')
        with self.assertRaises(SystemExit) as failed:
            lint.run_checks([("Privacy", ["bash", "-c", "exit 17", "synthetic-sensitive-filename"])])
        self.assertEqual(failed.exception.code, 17)
        self.assertNotIn("synthetic-sensitive-filename", str(failed.exception))

    def test_deletion_triggers_relationship_checks_without_reading_a_missing_file(self):
        phases = dict(lint.commands(["docs/design_docs/0001-deleted.md", "removed/BUILD.bazel"], self.base))
        self.assertIn("Check design document index", phases)
        self.assertIn("Check fuzzer soak coverage", phases)
        self.assertIn("Check changed Rust boundary inputs", phases)
        self.assertNotIn("Check changed tracked files for private details", phases)
        self.assertIn("Check retained source exemptions", phases)
        self.assertEqual(list(lint.commands([], self.base)), [])


if __name__ == "__main__":
    unittest.main()
