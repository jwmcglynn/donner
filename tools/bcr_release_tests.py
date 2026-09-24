"""Tests for artifact qualification and BCR publication authorization."""

import base64
import json
import subprocess
import unittest
from unittest import mock

from tools import bcr_release as release


SHA = "a" * 40
FORK_SHA = "b" * 40


def run_record(**overrides):
    value = {"id": 12, "head_sha": SHA, "path": release.PREFLIGHT_PATH,
             "event": "push", "head_branch": "main", "status": "completed", "conclusion": "success",
             "repository": {"full_name": release.REPOSITORY}, "run_attempt": 2}
    value.update(overrides)
    return value


def retained_artifacts(**overrides):
    names = [f"donner-bcr-qualified-2", f"donner-svg-linux-x86-64-{SHA}-2",
             f"donner-svg-darwin-arm64-{SHA}-2"]
    artifacts = [{"name": name, "expired": False, "size_in_bytes": 100} for name in names]
    if overrides:
        artifacts[1].update(overrides)
    return {"artifacts": artifacts}


class QualificationTest(unittest.TestCase):
    def test_release_body_pins_one_preflight_attempt(self):
        self.assertEqual(
            release.candidate_ref("Release notes\n\nRelease-Candidate-Preflight: 12/2\n"),
            ("12", "2"),
        )
        for body in ("Release notes", "Release-Candidate-Preflight: 0/2",
                     "Release-Candidate-Preflight: 12/2\nRelease-Candidate-Preflight: 13/1"):
            with self.subTest(body=body), self.assertRaisesRegex(ValueError, "exactly one"):
                release.candidate_ref(body)

    def test_only_expected_successful_source_run_is_accepted(self):
        release.check_run(run_record(), SHA, release.PREFLIGHT_PATH, {"push"})
        for change in [{"head_sha": "c" * 40}, {"path": "other.yml"}, {"event": "pull_request"},
                       {"status": "in_progress"}, {"conclusion": "failure"},
                       {"repository": {"full_name": "untrusted/fork"}}, {"run_attempt": 0}]:
            with self.subTest(change=change), self.assertRaises(ValueError):
                release.check_run(run_record(**change), SHA, release.PREFLIGHT_PATH, {"push"})

    @mock.patch.object(release, "gh_json")
    @mock.patch.object(release, "check_source", return_value="1.0.0")
    def test_manual_preflight_is_accepted_without_an_empty_commit(self, check_source, gh):
        run = run_record(event="workflow_dispatch")
        gh.side_effect = [run, [retained_artifacts()]]
        result = release.select_preflight(SHA, "v1.0.0", "12", "2")
        self.assertEqual(result, {"run_id": "12", "attempt": "2", "version": "1.0.0",
                                  "artifact": "donner-bcr-qualified-2",
                                  "linux_artifact": f"donner-svg-linux-x86-64-{SHA}-2",
                                  "macos_artifact": f"donner-svg-darwin-arm64-{SHA}-2"})
        check_source.assert_called_once_with(SHA, "v1.0.0")

    @mock.patch.object(release, "gh_json")
    @mock.patch.object(release, "check_source", return_value="1.0.0")
    def test_preflight_requires_all_retained_nonempty_artifacts(self, check_source, gh):
        for listed in [retained_artifacts(expired=True),
                       retained_artifacts(size_in_bytes=0),
                       {"artifacts": retained_artifacts()["artifacts"][:2]},
                       {"artifacts": retained_artifacts()["artifacts"] * 2}]:
            with self.subTest(listed=listed):
                gh.side_effect = [run_record(), [listed]]
                with self.assertRaisesRegex(ValueError, "missing, ambiguous, empty or expired"):
                    release.select_preflight(SHA, "v1.0.0", "12", "2")

    @mock.patch.object(release, "gh_json")
    @mock.patch.object(release, "check_source", return_value="1.0.0")
    def test_artifact_lookup_reads_all_pages(self, check_source, gh):
        gh.side_effect = [run_record(), [{"artifacts": []}, retained_artifacts()]]
        self.assertEqual(release.select_preflight(SHA, "v1.0.0", "12", "2")["attempt"], "2")

    @mock.patch.object(release, "gh_json")
    @mock.patch.object(release, "check_source", return_value="1.0.0")
    def test_failed_selected_preflight_does_not_fall_back(self, check_source, gh):
        failed = run_record(id=13, conclusion="failure")
        gh.return_value = failed
        with self.assertRaisesRegex(ValueError, "successful build"):
            release.select_preflight(SHA, "v1.0.0", "13", "2")

    @mock.patch.object(release, "gh_json", return_value=run_record(event="pull_request"))
    @mock.patch.object(release, "check_source", return_value="1.0.0")
    def test_pr_artifacts_cannot_supply_a_release(self, check_source, gh):
        with self.assertRaisesRegex(ValueError, "successful build"):
            release.select_preflight(SHA, "v1.0.0", "12", "2")

    @mock.patch.object(release, "gh_json", return_value=run_record(head_branch="feature"))
    @mock.patch.object(release, "check_source", return_value="1.0.0")
    def test_manual_branch_attestation_cannot_supply_a_release(self, check_source, gh):
        with self.assertRaisesRegex(ValueError, "on main"):
            release.select_preflight(SHA, "v1.0.0", "12", "2")

    @mock.patch.object(release.release_artifact_publisher, "verify_remote_tag")
    @mock.patch.object(release.subprocess, "run")
    @mock.patch.object(release.bcr_source, "git", return_value='module(name="donner", version="1.0.0")')
    def test_version_and_main_ancestry_gate_source(self, git, execute, remote):
        with self.assertRaisesRegex(ValueError, "tag does not match"):
            release.check_source(SHA, "v2.0.0")
        remote.assert_not_called()
        execute.side_effect = subprocess.CalledProcessError(1, ["git"])
        with self.assertRaises(subprocess.CalledProcessError):
            release.check_source(SHA, "v1.0.0")
        remote.assert_not_called()

    def test_draft_and_unknown_release_state_are_rejected(self):
        for data in [{"tagName": "v1.0.0", "isDraft": True, "isPrerelease": False},
                     {"tagName": "v1.0.0"},
                     {"tagName": "v2.0.0", "isDraft": False, "isPrerelease": False}]:
            with self.subTest(data=data), self.assertRaises(ValueError):
                release.check_release(data, "v1.0.0")


class PublishedSourceTest(unittest.TestCase):
    def setUp(self):
        self.receipt = {"sha256": "c" * 64, "verified_run_id": "12", "verified_run_attempt": "2"}
        prefix = "donner-1.0.0"
        self.assets = [{"name": name, "digest": "sha256:" + "c" * 64} for name in (
            f"{prefix}.tar.gz", f"{prefix}.tar.gz.sha256", f"{prefix}.provenance.json")]
        patches = [
            mock.patch.object(release.bcr_source, "git", return_value='module(name="donner", version="1.0.0")'),
            mock.patch.object(release.bcr_source, "verify_archive", return_value=self.receipt),
            mock.patch.object(release.subprocess, "run"),
            mock.patch.object(release, "gh_json", return_value=run_record()),
        ]
        self.git, self.verify, self.download, self.gh = [patch.start() for patch in patches]
        for patch in patches:
            self.addCleanup(patch.stop)

    def test_published_bytes_are_bound_to_the_successful_exact_attempt(self):
        self.assertEqual(release.verify_published_source(SHA, "v1.0.0", {"assets": self.assets}), self.receipt)
        self.assertEqual(self.download.call_count, 3)
        self.gh.assert_called_once_with("api", "repos/jwmcglynn/donner/actions/runs/12/attempts/2")

    def test_missing_duplicate_or_wrong_digest_assets_are_rejected(self):
        cases = [self.assets[1:], self.assets + [self.assets[0]],
                 [dict(self.assets[0], digest="sha256:different"), *self.assets[1:]]]
        for assets in cases:
            with self.subTest(assets=assets), self.assertRaises(ValueError):
                release.verify_published_source(SHA, "v1.0.0", {"assets": assets})
        self.gh.assert_not_called()

    def test_unqualified_and_failed_attempts_are_rejected(self):
        for changes in [{"verified_run_id": None}, {"verified_run_attempt": "../other"}]:
            self.verify.return_value = dict(self.receipt, **changes)
            with self.subTest(changes=changes), self.assertRaisesRegex(ValueError, "missing its CI"):
                release.verify_published_source(SHA, "v1.0.0", {"assets": self.assets})
        self.verify.return_value = self.receipt
        for changes in [{"conclusion": "failure"}, {"run_attempt": 3}, {"head_sha": "d" * 40}]:
            self.gh.return_value = run_record(**changes)
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                release.verify_published_source(SHA, "v1.0.0", {"assets": self.assets})


class SubmissionTest(unittest.TestCase):
    def setUp(self):
        self.receipt = {"version": "1.0.0", "sha256": "c" * 64}
        self.source = {
            "url": "https://github.com/jwmcglynn/donner/releases/download/v1.0.0/donner-1.0.0.tar.gz",
            "strip_prefix": "donner-1.0.0",
            "integrity": "sha256-" + base64.b64encode(bytes.fromhex("c" * 64)).decode(),
        }
        self.ref = {"ref": "refs/heads/donner-v1.0.0", "object": {"sha": FORK_SHA}}
        self.pr = {"head": {"sha": FORK_SHA, "repo": {"full_name": release.REGISTRY_FORK}},
                   "state": "open", "html_url": "https://github.com/bazelbuild/bazel-central-registry/pull/1"}

    @mock.patch.object(release, "gh_json", return_value=[])
    def test_new_branch_allows_a_new_submission(self, gh):
        self.assertIsNone(release.existing_submission("v1.0.0", SHA, self.receipt))

    @mock.patch.object(release.subprocess, "check_output", return_value=b"expected")
    @mock.patch.object(release, "fork_contents")
    @mock.patch.object(release, "gh_json")
    def test_matching_existing_submission_is_idempotent(self, gh, contents, git):
        git.return_value = b'{}'
        gh.side_effect = [[self.ref], [self.pr]]
        contents.side_effect = [json.dumps(self.source).encode(), b"{}", b"{}",
                                b'{"versions": ["0.9.0", "1.0.0"], "yanked_versions": {"0.9.0": "old"}}']
        self.assertEqual(release.existing_submission("v1.0.0", SHA, self.receipt), self.pr["html_url"])
        self.assertFalse(any("POST" in str(call) or "PATCH" in str(call) for call in gh.call_args_list))

    @mock.patch.object(release, "fork_contents")
    @mock.patch.object(release, "gh_json")
    def test_changed_existing_source_is_not_replaced(self, gh, contents):
        gh.return_value = [self.ref]
        contents.return_value = json.dumps(dict(self.source, integrity="sha256:different")).encode()
        with self.assertRaisesRegex(ValueError, "different source metadata"):
            release.existing_submission("v1.0.0", SHA, self.receipt)
        self.assertEqual(gh.call_count, 1)

    @mock.patch.object(release.subprocess, "check_output", return_value=b"expected")
    @mock.patch.object(release, "fork_contents")
    @mock.patch.object(release, "gh_json")
    def test_orphan_submission_branch_requires_manual_recovery(self, gh, contents, git):
        git.return_value = b'{}'
        gh.side_effect = [[self.ref], []]
        contents.side_effect = [json.dumps(self.source).encode(), b"{}", b"{}",
                                b'{"versions": ["0.9.0", "1.0.0"], "yanked_versions": {"0.9.0": "old"}}']
        with self.assertRaisesRegex(ValueError, "manual PR recovery"):
            release.existing_submission("v1.0.0", SHA, self.receipt)

    @mock.patch.object(release.subprocess, "check_output")
    @mock.patch.object(release, "fork_contents")
    @mock.patch.object(release, "gh_json")
    def test_stale_registry_metadata_does_not_count_as_identical(self, gh, contents, git):
        template = {"homepage": "https://github.com/jwmcglynn/donner", "maintainers": [{"github": "jwmcglynn"}],
                    "repository": ["github:jwmcglynn/donner"], "versions": [], "yanked_versions": {}}
        git.side_effect = lambda args: (json.dumps(template).encode()
                                       if args[-1].endswith("metadata.template.json") else b"expected")
        valid = dict(template, versions=["0.9.0", "1.0.0"])
        for change in [{"maintainers": []}, {"versions": ["0.9.0"]},
                       {"versions": ["1.0.0", "1.0.0"]}, {"yanked_versions": {"1.0.0": "withdrawn"}},
                       {"yanked_versions": {"0.9.0": []}}]:
            metadata = dict(valid, **change)
            gh.side_effect = [[self.ref], [self.pr]]
            def file_contents(path, head):
                if path.endswith("metadata.json"):
                    return json.dumps(metadata).encode()
                return json.dumps(self.source).encode() if path.endswith("source.json") else b"expected"
            contents.side_effect = file_contents
            with self.subTest(change=change), self.assertRaisesRegex(ValueError, "registry metadata"):
                release.existing_submission("v1.0.0", SHA, self.receipt)

    @mock.patch.object(release, "verify_published_source")
    @mock.patch.object(release, "check_source")
    @mock.patch.object(release.bcr_source, "git", return_value='module(name="donner", version="1.0.0-pre")')
    @mock.patch.object(release, "gh_json")
    def test_prerelease_never_reaches_submission(self, gh, git, check_source, verify):
        gh.side_effect = [run_record(path=release.RELEASE_PATH, event="release"),
                          {"tagName": "v1.0.0-pre", "isDraft": False, "isPrerelease": True}]
        self.assertEqual(release.plan_submission("12")["publish"], "false")
        check_source.assert_not_called()
        verify.assert_not_called()

    @mock.patch.object(release, "gh_json", return_value=run_record(path=release.RELEASE_PATH, event="workflow_dispatch"))
    def test_non_release_workflow_cannot_authorize_publication(self, gh):
        with self.assertRaisesRegex(ValueError, "successful build"):
            release.plan_submission("12")

    def test_run_id_cannot_inject_an_api_path(self):
        with self.assertRaisesRegex(ValueError, "numeric"):
            release.plan_submission("../other")


if __name__ == "__main__":
    unittest.main()
