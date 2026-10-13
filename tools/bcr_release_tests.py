"""Tests for artifact qualification and BCR publication authorization."""

import base64
import hashlib
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

    def check(self, assets=None, run_id="12", attempt="2"):
        return release.verify_published_source(SHA, "v1.0.0", {"assets": assets or self.assets},
                                               run_id, attempt)

    def test_published_bytes_are_bound_to_the_successful_exact_attempt(self):
        self.assertEqual(self.check(), self.receipt)
        self.assertEqual(self.download.call_count, 3)
        self.gh.assert_called_once_with("api", "repos/jwmcglynn/donner/actions/runs/12/attempts/2")

    def test_preflight_must_be_the_record_attempt_on_main(self):
        # The record names the qualifying attempt; the archive's provenance must name the same.
        for run_id, attempt in (("12", "1"), ("13", "2")):
            with self.subTest(run_id=run_id, attempt=attempt):
                with self.assertRaisesRegex(ValueError, "another preflight attempt"):
                    self.check(run_id=run_id, attempt=attempt)
        self.gh.assert_not_called()
        for change, message in (({"head_branch": "feature"}, "run on main"),
                                ({"head_branch": None}, "run on main"),
                                ({"id": 13}, "does not match provenance")):
            with self.subTest(change=change):
                self.gh.return_value = run_record(**change)
                with self.assertRaisesRegex(ValueError, message):
                    self.check()

    def test_missing_duplicate_or_wrong_digest_assets_are_rejected(self):
        cases = [self.assets[1:], self.assets + [self.assets[0]],
                 [dict(self.assets[0], digest="sha256:different"), *self.assets[1:]]]
        for assets in cases:
            with self.subTest(assets=assets), self.assertRaises(ValueError):
                self.check(assets)
        self.gh.assert_not_called()

    def test_unqualified_and_failed_attempts_are_rejected(self):
        for changes in [{"verified_run_id": None}, {"verified_run_attempt": "../other"}]:
            self.verify.return_value = dict(self.receipt, **changes)
            with self.subTest(changes=changes), self.assertRaisesRegex(ValueError, "missing its CI"):
                self.check()
        self.verify.return_value = self.receipt
        for changes in [{"conclusion": "failure"}, {"run_attempt": 3}, {"head_sha": "d" * 40}]:
            self.gh.return_value = run_record(**changes)
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                self.check()


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

    @mock.patch.object(release, "published_record")
    @mock.patch.object(release, "verify_published_source")
    @mock.patch.object(release, "check_source")
    @mock.patch.object(release.bcr_source, "git", return_value='module(name="donner", version="1.0.0-pre")')
    @mock.patch.object(release, "gh_json")
    def test_prerelease_never_reaches_submission(self, gh, git, check_source, verify, record):
        gh.side_effect = [run_record(path=release.RELEASE_PATH, event="release"),
                          {"tagName": "v1.0.0-pre", "isDraft": False, "isPrerelease": True}]
        self.assertEqual(release.plan_submission("12", DIGEST, Path("work"))["prepare"], "false")
        check_source.assert_not_called()
        verify.assert_not_called()
        record.assert_not_called()

    @mock.patch.object(release, "gh_json", return_value=run_record(path=release.RELEASE_PATH, event="workflow_dispatch"))
    def test_non_release_workflow_cannot_authorize_publication(self, gh):
        with self.assertRaisesRegex(ValueError, "successful build"):
            release.plan_submission("12", DIGEST, Path("work"))

    def test_run_id_cannot_inject_an_api_path(self):
        with self.assertRaisesRegex(ValueError, "numeric"):
            release.plan_submission("../other", DIGEST, Path("work"))

    @mock.patch.object(release, "gh_json")
    def test_malformed_record_digest_fails_before_any_api_call(self, gh):
        for value in ("not-a-digest", DIGEST.upper(), "sha256:" + DIGEST, "c" * 63):
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, "record SHA-256"):
                release.plan_submission("12", value, Path("work"))
        gh.assert_not_called()


def submission_record(**preflight):
    """A record whose source archive, registry entry and attempt the submission plan reads."""
    entry = {"run_id": 30, "attempt": 2, **preflight}
    artifacts = {role: {"run_id": 30, "attempt": 2}
                 for role in release.release_candidate.PREFLIGHT_ROLES}
    artifacts["source"] = {"run_id": 30, "attempt": 2,
                           "files": {"donner-1.0.0.tar.gz": "c" * 64}}
    artifacts["bcr-entry"] = entry
    return {"source": {"commit": SHA, "version": "1.0.0"}, "artifacts": artifacts}


class RecordSubmissionTest(unittest.TestCase):
    """Plan BCR fork preparation from the published release's candidate record."""

    def setUp(self):
        self.run = run_record(path=release.RELEASE_PATH, event="release")
        self.release = {"tagName": "v1.0.0", "isDraft": False, "isPrerelease": False,
                        "body": f"Notes\n\nRelease-Candidate-Record: 40/1 sha256:{DIGEST}\n",
                        "assets": []}
        self.record = submission_record()
        patches = {
            "git": mock.patch.object(release.bcr_source, "git",
                                     return_value='module(name="donner", version="1.0.0")'),
            "gh": mock.patch.object(release, "gh_json"),
            "check_source": mock.patch.object(release, "check_source"),
            "published": mock.patch.object(release, "published_record"),
            "attest": mock.patch.object(release, "verify_record_attestation"),
            "verify": mock.patch.object(release, "verify_published_source",
                                        return_value={"sha256": "c" * 64}),
            "existing": mock.patch.object(release, "existing_submission", return_value=None),
        }
        self.mocks = {name: patch.start() for name, patch in patches.items()}
        for patch in patches.values():
            self.addCleanup(patch.stop)
        self.mocks["gh"].side_effect = lambda *args: (self.run if args[0] == "api"
                                                      else self.release)
        self.mocks["published"].side_effect = lambda *args: (Path("record.json"), self.record)

    def plan(self, digest=DIGEST):
        return release.plan_submission("12", digest, Path("work"))

    def test_approved_digest_and_attempt_come_from_the_record(self):
        result = self.plan()
        self.assertEqual(result, {"prepare": "true", "tag": "v1.0.0", "version": "1.0.0",
                                  "existing_pr": None, "source_commit": SHA,
                                  "source_sha256": "c" * 64, "record_sha256": DIGEST})
        self.mocks["published"].assert_called_once_with("v1.0.0", "1.0.0", DIGEST, self.release,
                                                        Path("work"))
        self.mocks["attest"].assert_called_once_with(Path("record.json"), SHA)
        self.mocks["verify"].assert_called_once_with(SHA, "v1.0.0", self.release, "30", "2")
        self.assertIn(mock.call("release", "view", "v1.0.0", "--repo", release.REPOSITORY,
                                "--json", "tagName,isDraft,isPrerelease,assets,body"),
                      self.mocks["gh"].call_args_list)

    def test_release_body_must_name_the_approved_record(self):
        cases = {"other digest": f"Release-Candidate-Record: 40/1 sha256:{'f' * 64}\n",
                 "no record": "Release notes\n",
                 "preflight line": f"Release-Candidate-Record: 40/1 sha256:{DIGEST}\n"
                                   "Release-Candidate-Preflight: 30/2\n"}
        for case, body in cases.items():
            with self.subTest(case=case):
                self.release["body"] = body
                with self.assertRaises(ValueError):
                    self.plan()
        self.release.pop("body")
        with self.assertRaisesRegex(ValueError, "different candidate record"):
            self.plan()
        self.mocks["published"].assert_not_called()

    def test_record_for_another_commit_or_split_preflight_is_rejected(self):
        self.record["source"]["commit"] = "d" * 40
        with self.assertRaisesRegex(ValueError, "another commit"):
            self.plan()
        self.record = submission_record(attempt=1)
        with self.assertRaisesRegex(ValueError, "different preflight attempts"):
            self.plan()
        self.record = submission_record()
        del self.record["artifacts"]["source"]["files"]["donner-1.0.0.tar.gz"]
        with self.assertRaisesRegex(ValueError, "approved source archive digest"):
            self.plan()
        self.mocks["verify"].assert_not_called()

    def test_published_source_must_equal_the_record_archive(self):
        self.mocks["verify"].return_value = {"sha256": "d" * 64}
        with self.assertRaisesRegex(ValueError, "differs from the candidate record"):
            self.plan()
        self.mocks["existing"].assert_not_called()

    def test_unattested_record_stops_before_source_checks(self):
        self.mocks["attest"].side_effect = subprocess.CalledProcessError(1, ["gh"])
        with self.assertRaises(subprocess.CalledProcessError):
            self.plan()
        self.mocks["verify"].assert_not_called()


class PublishedRecordTest(unittest.TestCase):
    """The release asset must be the approved record, byte for byte."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name) / "record"
        self.raw = b'{"canonical":true}\n'
        self.digest = hashlib.sha256(self.raw).hexdigest()
        self.release = {"assets": [{"name": "donner-1.0.0.candidate.json",
                                    "digest": "sha256:" + self.digest}]}
        self.parsed = {"source": {"version": "1.0.0"}}

    def download(self, data):
        def write(args, check):
            (Path(args[args.index("--dir") + 1]) / "donner-1.0.0.candidate.json").write_bytes(data)
        return mock.patch.object(release.subprocess, "run", side_effect=write)

    def fetch(self, digest=None):
        return release.published_record("v1.0.0", "1.0.0", digest or self.digest, self.release,
                                        self.directory)

    def test_release_asset_is_loaded_as_the_approved_record(self):
        with self.download(self.raw), mock.patch.object(
                release.release_candidate, "load_record", return_value=self.parsed) as load:
            path, record = self.fetch()
        self.assertEqual(path, self.directory / "donner-1.0.0.candidate.json")
        self.assertEqual(record, self.parsed)
        load.assert_called_once_with(self.raw, self.digest)

    def test_attestation_names_the_assembly_workflow_and_commit(self):
        with mock.patch.object(release.subprocess, "run") as execute:
            release.verify_record_attestation(Path("record.json"), SHA)
        execute.assert_called_once_with(
            ["gh", "attestation", "verify", "record.json", "--repo", "jwmcglynn/donner",
             "--signer-workflow", "jwmcglynn/donner/.github/workflows/release_candidate.yml",
             "--source-digest", SHA], check=True, stdout=subprocess.DEVNULL)

    def test_missing_duplicate_or_other_asset_digest_is_rejected(self):
        asset = self.release["assets"][0]
        for assets in ([], [asset, asset], [dict(asset, digest="sha256:" + "f" * 64)]):
            with self.subTest(assets=assets):
                self.release["assets"] = assets
                with self.assertRaisesRegex(ValueError, "exactly the approved candidate record"):
                    self.fetch()

    def test_downloaded_bytes_and_version_are_checked(self):
        with self.download(b'{"other":true}\n'), \
                self.assertRaisesRegex(ValueError, "differs from the approved record"):
            self.fetch()
        self.directory = Path(self.temp.name) / "version"
        with self.download(self.raw), mock.patch.object(
                release.release_candidate, "load_record",
                return_value={"source": {"version": "1.0.1"}}), \
                self.assertRaisesRegex(ValueError, "another version"):
            self.fetch()


class EntryPromotionTest(unittest.TestCase):
    """Fetch and stage the registry entry preflight validated, instead of regenerating it."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)

    @mock.patch.object(release.release_candidate, "fetch_role", return_value=Path("entry"))
    @mock.patch.object(release, "published_record")
    @mock.patch.object(release, "gh_json")
    @mock.patch.object(release.subprocess, "check_output", return_value=SHA + "\n")
    def test_fetch_entry_reads_the_record_role_for_the_source_commit(self, head, gh, published,
                                                                     fetch):
        gh.return_value = {"tagName": "v1.0.0", "isDraft": False, "isPrerelease": False}
        published.return_value = (Path("record.json"), submission_record())
        work = self.base / "work"
        self.assertEqual(release.fetch_entry("v1.0.0", DIGEST, Path("this"), work), Path("entry"))
        published.assert_called_once_with("v1.0.0", "1.0.0", DIGEST, gh.return_value,
                                          work / "record")
        fetch.assert_called_once_with(submission_record(), "bcr-entry", Path("this"),
                                      work / "bcr-entry.zip", work / "entry")
        head.return_value = "d" * 40 + "\n"
        with self.assertRaisesRegex(ValueError, "another commit"):
            release.fetch_entry("v1.0.0", DIGEST, Path("this"), work)
        published.return_value = (Path("record.json"), submission_record(run_id=31))
        head.return_value = SHA + "\n"
        with self.assertRaisesRegex(ValueError, "different preflight attempts"):
            release.fetch_entry("v1.0.0", DIGEST, Path("this"), work)

    @mock.patch.object(release, "gh_json")
    def test_fetch_entry_needs_a_stable_tag_and_digest(self, gh):
        for tag, digest in (("v1.0.0-pre", DIGEST), ("1.0.0", DIGEST), ("v1.0.0", "e" * 63)):
            with self.subTest(tag=tag), self.assertRaisesRegex(ValueError, "stable release tag"):
                release.fetch_entry(tag, digest, Path("this"), self.base)
        gh.assert_not_called()

    def validated_entry(self):
        entry = self.base / "entry"
        for path in release.bcr_source.entry_paths("1.0.0"):
            target = entry / "registry" / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(f"validated {path.name}\n", encoding="utf-8")
        return entry

    def registry(self):
        registry = self.base / "registry"
        (registry / "modules/donner").mkdir(parents=True)
        (registry / "modules/donner/metadata.json").write_text("upstream\n", encoding="utf-8")
        subprocess.run(["git", "init", "-q", str(registry)], check=True)
        return registry

    def test_stage_entry_copies_exactly_the_validated_files(self):
        entry, registry = self.validated_entry(), self.registry()
        release.stage_entry(entry, registry, "1.0.0")
        staged = subprocess.check_output(["git", "-C", str(registry), "diff", "--cached",
                                          "--name-only"], text=True).split()
        self.assertEqual(set(staged), {path.as_posix() for path in
                                       release.bcr_source.entry_paths("1.0.0")})
        for path in release.bcr_source.entry_paths("1.0.0"):
            self.assertEqual((registry / path).read_bytes(),
                             (entry / "registry" / path).read_bytes())

    def test_stage_entry_refuses_an_existing_version_or_linked_file(self):
        entry, registry = self.validated_entry(), self.registry()
        (registry / "modules/donner/1.0.0").mkdir()
        with self.assertRaisesRegex(ValueError, "already has this donner version"):
            release.stage_entry(entry, registry, "1.0.0")
        (registry / "modules/donner/1.0.0").rmdir()
        source = entry / "registry/modules/donner/1.0.0/source.json"
        source.unlink()
        source.symlink_to(self.base / "elsewhere.json")
        (self.base / "elsewhere.json").write_text("{}", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "missing, linked or too large"):
            release.stage_entry(entry, registry, "1.0.0")


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
        # The admission-validated entry from preflight: the same bytes as the staged change.
        self.validated = Path(self.temp.name) / "validated"
        for path in release.bcr_source.entry_paths("1.0.0"):
            target = self.validated / "registry" / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes((self.registry / path).read_bytes())

    def verify(self, sha256="c" * 64, commit=None):
        release.verify_generated_entry(self.registry, self.source, "1.0.0", sha256,
                                       self.validated, commit)

    def stage_validated(self):
        """Stage the registry change and make it the validated entry, to reach later checks."""
        subprocess.run(["git", "-C", str(self.registry), "add", "--", "modules/donner"], check=True)
        for path in release.bcr_source.entry_paths("1.0.0"):
            (self.validated / "registry" / path).write_bytes((self.registry / path).read_bytes())

    def test_proposed_entry_must_be_the_admission_validated_bytes(self):
        metadata = self.validated / "registry/modules/donner/metadata.json"
        original = metadata.read_bytes()
        # Equal JSON is not enough: the validated bytes are what upstream checked.
        metadata.write_text(json.dumps(json.loads(original), indent=4), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "admission-validated entry"):
            self.verify()
        metadata.write_bytes(original)
        (self.validated / "registry/modules/donner/1.0.0/presubmit.yml").unlink()
        with self.assertRaisesRegex(ValueError, "missing, linked or too large"):
            self.verify()

    def test_exact_generated_entry_matches_approved_source(self):
        self.verify()

    def test_changed_source_integrity_or_module_bytes_fail(self):
        with self.assertRaisesRegex(ValueError, "approved source archive"):
            self.verify("d" * 64)
        (self.entry / "MODULE.bazel").write_text("different\n", encoding="utf-8")
        subprocess.run(["git", "-C", str(self.registry), "add", "--", "modules/donner"], check=True)
        (self.validated / "registry/modules/donner/1.0.0/MODULE.bazel").write_text(
            "different\n", encoding="utf-8")
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
        self.stage_validated()
        with self.assertRaisesRegex(ValueError, "initial history"):
            self.verify()
        metadata["versions"] = ["1.0.0"]
        metadata["maintainers"] = []
        metadata_file.write_text(json.dumps(metadata), encoding="utf-8")
        self.stage_validated()
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
        self.verify(commit=commit)

    def test_unexpected_staged_file_fails_closed(self):
        (self.registry / "modules/donner/unexpected").write_text("other", encoding="utf-8")
        subprocess.run(["git", "-C", str(self.registry), "add", "--", "modules/donner"], check=True)
        with self.assertRaisesRegex(ValueError, "unexpected staged files"):
            self.verify()


if __name__ == "__main__":
    unittest.main()
