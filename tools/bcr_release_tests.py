"""Tests for artifact qualification and BCR publication authorization."""

import base64
import json
from pathlib import Path
import subprocess
import tempfile
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


DIGEST = "e" * 64


def assembly_run(**overrides):
    return run_record(**{"path": release.CANDIDATE_PATH, "event": "workflow_dispatch",
                         **overrides})


def retained_record(**overrides):
    artifacts = [{"id": 77, "name": "donner-release-candidate-2", "expired": False,
                  "size_in_bytes": 100},
                 {"id": 78, "name": "donner-release-candidate-1", "expired": False,
                  "size_in_bytes": 100}]
    artifacts[0].update(overrides)
    return {"artifacts": artifacts}


class QualificationTest(unittest.TestCase):
    def test_release_body_pins_one_candidate_record(self):
        line = f"Release-Candidate-Record: 12/2 sha256:{DIGEST}"
        self.assertEqual(release.candidate_ref(f"Release notes\n\n{line}\n"), ("12", "2", DIGEST))
        self.assertEqual(release.candidate_ref(f"Notes\r\n{line}\r\n"), ("12", "2", DIGEST))
        for body in ("Release notes", f"Release-Candidate-Record: 0/2 sha256:{DIGEST}",
                     "Release-Candidate-Record: 12/2", f"Release-Candidate-Record: 12/2 {DIGEST}",
                     f"Release-Candidate-Record: 12/2 sha256:{DIGEST.upper()}",
                     f"Release-Candidate-Record: 12/2 sha256:{DIGEST} extra",
                     f"{line}\n{line}",
                     f"{line}\nRelease-Candidate-Record: 13/1 sha256:{'f' * 64}",
                     f"{line}\nRelease-Candidate-Record: unparsed"):
            with self.subTest(body=body), self.assertRaisesRegex(ValueError, "exactly one"):
                release.candidate_ref(body)

    def test_release_body_cannot_also_name_a_preflight_attempt(self):
        body = (f"Release-Candidate-Record: 12/2 sha256:{DIGEST}\n"
                "Release-Candidate-Preflight: 12/2\n")
        with self.assertRaisesRegex(ValueError, "name the candidate record instead"):
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
    def test_record_of_the_named_assembly_attempt_is_selected(self, check_source, gh):
        gh.side_effect = [assembly_run(), [retained_record()]]
        result = release.select_record(SHA, "v1.0.0", "12", "2", DIGEST)
        self.assertEqual(result, {"run_id": "12", "attempt": "2", "artifact_id": "77",
                                  "record_sha256": DIGEST, "version": "1.0.0"})
        check_source.assert_called_once_with(SHA, "v1.0.0")
        self.assertEqual(gh.call_args_list[0].args,
                         ("api", f"repos/{release.REPOSITORY}/actions/runs/12/attempts/2"))

    @mock.patch.object(release, "gh_json")
    @mock.patch.object(release, "check_source", return_value="1.0.0")
    def test_record_requires_one_retained_nonempty_artifact(self, check_source, gh):
        for listed in [retained_record(expired=True), retained_record(size_in_bytes=0),
                       retained_record(id="77"), retained_record(name="donner-release-candidate-3"),
                       {"artifacts": retained_record()["artifacts"] * 2}]:
            with self.subTest(listed=listed):
                gh.side_effect = [assembly_run(), [listed]]
                with self.assertRaisesRegex(ValueError, "missing, ambiguous, empty or expired"):
                    release.select_record(SHA, "v1.0.0", "12", "2", DIGEST)

    @mock.patch.object(release, "gh_json")
    @mock.patch.object(release, "check_source", return_value="1.0.0")
    def test_record_lookup_reads_all_pages(self, check_source, gh):
        gh.side_effect = [assembly_run(), [{"artifacts": []}, retained_record()]]
        self.assertEqual(release.select_record(SHA, "v1.0.0", "12", "2", DIGEST)["artifact_id"],
                         "77")

    @mock.patch.object(release, "gh_json")
    @mock.patch.object(release, "check_source", return_value="1.0.0")
    def test_only_a_successful_main_assembly_of_the_release_commit_qualifies(self, check_source, gh):
        cases = {
            "failed": (assembly_run(conclusion="failure"), "successful build"),
            "preflight run": (run_record(), "successful build"),
            "push event": (assembly_run(event="push"), "successful build"),
            "other commit": (assembly_run(head_sha="c" * 40), "successful build"),
            "branch": (assembly_run(head_branch="feature"), "on main"),
            "other attempt": (assembly_run(run_attempt=1), "selected immutable attempt"),
            "other run": (assembly_run(id=13), "selected immutable attempt"),
        }
        for case, (run, message) in cases.items():
            with self.subTest(case=case):
                gh.side_effect = [run, [retained_record()]]
                with self.assertRaisesRegex(ValueError, message):
                    release.select_record(SHA, "v1.0.0", "12", "2", DIGEST)

    @mock.patch.object(release, "gh_json")
    @mock.patch.object(release, "check_source", return_value="1.0.0")
    def test_record_digest_must_be_a_full_sha256(self, check_source, gh):
        for value in ("e" * 63, "E" * 64, "sha256:" + "e" * 64):
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, "SHA-256"):
                release.select_record(SHA, "v1.0.0", "12", "2", value)
        gh.assert_not_called()

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
        self.assertEqual(release.plan_submission("12", SHA, "c" * 64)["prepare"], "false")
        check_source.assert_not_called()
        verify.assert_not_called()

    @mock.patch.object(release, "gh_json", return_value=run_record(path=release.RELEASE_PATH, event="workflow_dispatch"))
    def test_non_release_workflow_cannot_authorize_publication(self, gh):
        with self.assertRaisesRegex(ValueError, "successful build"):
            release.plan_submission("12", SHA, "c" * 64)

    def test_run_id_cannot_inject_an_api_path(self):
        with self.assertRaisesRegex(ValueError, "numeric"):
            release.plan_submission("../other", SHA, "c" * 64)

    @mock.patch.object(release, "existing_submission", return_value=None)
    @mock.patch.object(release, "verify_published_source", return_value={"sha256": "c" * 64})
    @mock.patch.object(release, "check_source")
    @mock.patch.object(release.bcr_source, "git", return_value='module(name="donner", version="1.0.0")')
    @mock.patch.object(release, "gh_json")
    def test_manual_submission_binds_approved_commit_and_digest(self, gh, git, check_source,
                                                                 verify, existing):
        run = run_record(path=release.RELEASE_PATH, event="release")
        published = {"tagName": "v1.0.0", "isDraft": False, "isPrerelease": False}
        gh.side_effect = [run, published]
        self.assertEqual(release.plan_submission("12", SHA, "c" * 64)["prepare"], "true")
        existing.assert_called_once()

        gh.side_effect = [run]
        with self.assertRaisesRegex(ValueError, "approved BCR submission"):
            release.plan_submission("12", FORK_SHA, "c" * 64)
        gh.side_effect = [run, published]
        with self.assertRaisesRegex(ValueError, "published source digest differs"):
            release.plan_submission("12", SHA, "d" * 64)

    @mock.patch.object(release, "gh_json")
    def test_malformed_approval_fails_before_any_api_call(self, gh):
        with self.assertRaisesRegex(ValueError, "approved source"):
            release.plan_submission("12", "not-a-commit", "c" * 64)
        with self.assertRaisesRegex(ValueError, "approved source"):
            release.plan_submission("12", SHA, "not-a-digest")
        gh.assert_not_called()


class GeneratedEntryTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.registry = Path(self.temp.name) / "registry"
        self.source = Path(self.temp.name) / "source"
        self.registry.mkdir()
        (self.source / ".bcr").mkdir(parents=True)
        subprocess.run(["git", "init", "-q", str(self.registry)], check=True)
        subprocess.run(["git", "-C", str(self.registry), "-c", "user.name=Test",
                        "-c", "user.email=test@example.com", "commit", "--allow-empty",
                        "-qm", "base"], check=True)
        entry = self.registry / "modules/donner/1.0.0"
        entry.mkdir(parents=True)
        (self.source / "MODULE.bazel").write_text("module(name='donner')\n", encoding="utf-8")
        (self.source / ".bcr/presubmit.yml").write_text("tasks: []\n", encoding="utf-8")
        template = {"homepage": "https://github.com/jwmcglynn/donner",
                    "maintainers": [{"github": "jwmcglynn"}],
                    "repository": ["github:jwmcglynn/donner"],
                    "versions": [], "yanked_versions": {}}
        (self.source / ".bcr/metadata.template.json").write_text(json.dumps(template), encoding="utf-8")
        (self.registry / "modules/donner/metadata.json").write_text(
            json.dumps(dict(template, versions=["1.0.0"])), encoding="utf-8")
        (entry / "MODULE.bazel").write_bytes((self.source / "MODULE.bazel").read_bytes())
        (entry / "presubmit.yml").write_bytes((self.source / ".bcr/presubmit.yml").read_bytes())
        expected = {"url": release.bcr_source.source_url("1.0.0"),
                    "strip_prefix": "donner-1.0.0",
                    "integrity": "sha256-" + base64.b64encode(bytes.fromhex("c" * 64)).decode()}
        (entry / "source.json").write_text(json.dumps(expected), encoding="utf-8")
        subprocess.run(["git", "-C", str(self.registry), "add", "--", "modules/donner"], check=True)
        self.entry = entry

    def verify(self):
        release.verify_generated_entry(self.registry, self.source, "1.0.0", "c" * 64)

    def test_exact_generated_entry_matches_approved_source(self):
        self.verify()

    def test_changed_source_integrity_or_module_bytes_fail(self):
        with self.assertRaisesRegex(ValueError, "approved source archive"):
            release.verify_generated_entry(self.registry, self.source, "1.0.0", "d" * 64)
        (self.entry / "MODULE.bazel").write_text("different\n", encoding="utf-8")
        subprocess.run(["git", "-C", str(self.registry), "add", "--", "modules/donner"], check=True)
        with self.assertRaisesRegex(ValueError, "approved module files"):
            self.verify()

    def test_unstaged_change_does_not_change_verified_commit_bytes(self):
        (self.entry / "MODULE.bazel").write_text("unstaged\n", encoding="utf-8")
        self.verify()

    def test_changed_metadata_history_or_maintainer_fails(self):
        metadata_file = self.registry / "modules/donner/metadata.json"
        metadata = json.loads(metadata_file.read_text(encoding="utf-8"))
        metadata["versions"] = ["0.9.0", "1.0.0"]
        metadata_file.write_text(json.dumps(metadata), encoding="utf-8")
        subprocess.run(["git", "-C", str(self.registry), "add", "--", "modules/donner"], check=True)
        with self.assertRaisesRegex(ValueError, "initial history"):
            self.verify()
        metadata["versions"] = ["1.0.0"]
        metadata["maintainers"] = []
        metadata_file.write_text(json.dumps(metadata), encoding="utf-8")
        subprocess.run(["git", "-C", str(self.registry), "add", "--", "modules/donner"], check=True)
        with self.assertRaisesRegex(ValueError, "maintainer fields"):
            self.verify()

    def test_committed_entry_is_verified_against_its_base(self):
        base = subprocess.check_output(
            ["git", "-C", str(self.registry), "rev-parse", "HEAD"], text=True).strip()
        subprocess.run(["git", "-C", str(self.registry), "-c", "user.name=Test",
                        "-c", "user.email=test@example.com", "commit", "-qm", "entry"], check=True)
        commit = subprocess.check_output(
            ["git", "-C", str(self.registry), "rev-parse", "HEAD"], text=True).strip()
        subprocess.run(["git", "-C", str(self.registry), "checkout", "--detach", "-q", base],
                       check=True)
        release.verify_generated_entry(self.registry, self.source, "1.0.0", "c" * 64, commit)

    def test_unexpected_staged_file_fails_closed(self):
        (self.registry / "modules/donner/unexpected").write_text("other", encoding="utf-8")
        subprocess.run(["git", "-C", str(self.registry), "add", "--", "modules/donner"], check=True)
        with self.assertRaisesRegex(ValueError, "unexpected staged files"):
            self.verify()


if __name__ == "__main__":
    unittest.main()
