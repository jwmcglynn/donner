"""Tests for immutable release-candidate record and retained artifact validation."""

from datetime import datetime, timedelta, timezone
import hashlib
import json
from pathlib import Path
import stat
import subprocess
import tempfile
import unittest
from unittest import mock
import zipfile

from tools import release_candidate as candidate


COMMIT = "a" * 40
TREE = "b" * 40
LOCK_LINUX = "c" * 64
LOCK_MACOS = "d" * 64
COMPILERS = {"cli-linux": "clang 21", "cli-macos": "Apple clang 17"}
NOW = datetime(2026, 10, 12, 12, 0, tzinfo=timezone.utc)


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def github_time(value: datetime) -> str:
    return value.astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


class FrozenDatetime(datetime):
    """The verifier's clock, fixed so retention windows are deterministic."""

    @classmethod
    def now(cls, tz=None):
        return NOW if tz is None else NOW.astimezone(tz)


class ZipValidationTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.path = self.root / "artifact.zip"
        self.output = self.root / "output"
        self.output.mkdir()

    def write_zip(self, files):
        with zipfile.ZipFile(self.path, "w") as archive:
            for name, data in files:
                archive.writestr(name, data)

    def test_bounded_regular_files_match_exact_inventory(self):
        self.write_zip([("site/", b""), ("site/index.html", b"ready")])
        candidate._extract_verified(self.path, {"site/index.html": digest(b"ready")}, self.output)
        self.assertEqual((self.output / "site/index.html").read_bytes(), b"ready")

    def test_path_escape_and_unexpected_file_are_rejected(self):
        self.write_zip([("../outside", b"escape")])
        with self.assertRaisesRegex(ValueError, "unsafe path"):
            candidate._extract_verified(self.path, {"../outside": digest(b"escape")}, self.output)
        self.write_zip([("safe", b"data"), ("extra", b"data")])
        with self.assertRaisesRegex(ValueError, "unexpected files"):
            candidate._extract_verified(self.path, {"safe": digest(b"data")}, self.output)

    def test_symlink_and_changed_file_bytes_are_rejected(self):
        link = zipfile.ZipInfo("link")
        link.create_system = 3
        link.external_attr = (stat.S_IFLNK | 0o777) << 16
        self.write_zip([(link, b"outside")])
        with self.assertRaisesRegex(ValueError, "link"):
            candidate._extract_verified(self.path, {"link": digest(b"outside")}, self.output)
        self.write_zip([("payload", b"actual")])
        with self.assertRaisesRegex(ValueError, "digest differs"):
            candidate._extract_verified(self.path, {"payload": digest(b"expected")}, self.output)

    def test_private_shaped_or_control_character_names_are_rejected_without_echo(self):
        with self.assertRaisesRegex(ValueError, "publishable role"):
            candidate._verify_role_files("source", {"home/user/private.txt"}, "0.8.0")
        name = "site/private\nsecret"
        self.write_zip([(name, b"data")])
        with self.assertRaises(ValueError) as error:
            candidate._extract_verified(self.path, {name: digest(b"data")}, self.output)
        self.assertNotIn("private", str(error.exception))

    def test_control_files_have_a_small_individual_budget(self):
        self.write_zip([("provenance.json", b"large")])
        with mock.patch.object(candidate, "MAX_SMALL_CONTROL_BYTES", 4):
            with self.assertRaisesRegex(ValueError, "byte budget"):
                candidate._extract_verified(
                    self.path, {"provenance.json": digest(b"large")}, self.output)


class RecordValidationTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for name in ("artifacts", "evidence", "reviews", "source"):
            (self.root / name).mkdir()
        self.record = {
            "schema": 1,
            "source": {"commit": COMMIT, "tree": TREE, "version": "0.8.0",
                       "compatibility_level": 0, "bazel_version": "8.8.1",
                       "module_locks_sha256": {"cli-linux": LOCK_LINUX,
                                               "cli-macos": LOCK_MACOS},
                       "compiler_versions": COMPILERS.copy()},
            "artifacts": {}, "evidence": {}, "reviews": {},
        }
        self.runs = {}
        self.metadata = {}
        self.jobs = {}
        self._add_artifacts()
        self._add_evidence()
        self._add_reviews()

    @staticmethod
    def fixture_files(role):
        names = candidate._fixed_role_files(role, "0.8.0")
        if names is None:
            names = {"provenance.json", "SHA256SUMS", "site/editor.wasm",
                     "deploy/editor.wasm.gz"}
            names.update(f"{prefix}/{name}" for prefix in ("site", "deploy")
                         for name in candidate.EDITOR_COMMON_FILES)
        return {name: name.encode() for name in sorted(names)}

    def _add_artifacts(self):
        for role, workflow in candidate.ARTIFACT_WORKFLOWS.items():
            run_id = 100 if role in ("source", "cli-linux", "cli-macos") else 101 + len(self.runs)
            artifact_id = len(self.metadata) + 1
            files = self.fixture_files(role)
            path = self.root / "artifacts" / f"{role}.zip"
            with zipfile.ZipFile(path, "w") as archive:
                for name, data in files.items():
                    archive.writestr(name, data)
            self.record["artifacts"][role] = {
                "run_id": run_id, "attempt": 1, "artifact_id": artifact_id,
                "name": candidate._artifact_name(role, COMMIT, 1),
                "zip_sha256": candidate._sha256(path),
                "files": {name: digest(data) for name, data in files.items()},
            }
            self.runs[run_id] = self.fixture_run(run_id, workflow)
            self.metadata[artifact_id] = {
                "id": artifact_id, "name": self.record["artifacts"][role]["name"],
                "digest": "sha256:" + candidate._sha256(path), "expired": False,
                "expires_at": github_time(NOW + timedelta(days=89)),
                "size_in_bytes": path.stat().st_size,
                "workflow_run": {"id": run_id, "head_sha": COMMIT, "head_branch": "main",
                                 "repository_id": 1, "head_repository_id": 1},
            }

    def _add_evidence(self):
        for index, (kind, workflow) in enumerate(candidate.EVIDENCE_WORKFLOWS.items(), 201):
            names = [alternatives[0] for alternatives in candidate.REQUIRED_EVIDENCE_JOBS[kind]]
            jobs = [{"name": name, "conclusion": "success"} for name in names]
            self.record["evidence"][kind] = {"run_id": index, "attempt": 1,
                                              "passed_jobs": len(names), "skipped_jobs": 0}
            self.write_evidence(kind, self.evidence_document(kind, index, jobs))
            self.runs[index] = self.fixture_run(index, workflow)
            self.jobs[index] = {"total_count": len(names), "jobs": jobs}

    @staticmethod
    def evidence_document(kind, run_id, jobs):
        return {"workflow": candidate.EVIDENCE_WORKFLOWS[kind], "run_id": run_id, "attempt": 1,
                "commit": COMMIT, "jobs": [dict(job) for job in jobs]}

    def write_evidence(self, kind, document):
        """Write evidence bytes and bind their digest, so only the content rules can reject it."""
        data = document if isinstance(document, bytes) else json.dumps(document).encode()
        (self.root / "evidence" / f"{kind}.json").write_bytes(data)
        self.record["evidence"][kind]["sha256"] = digest(data)

    def write_review(self, kind, data):
        (self.root / "reviews" / f"{kind}.md").write_bytes(data)
        self.record["reviews"][kind] = digest(data)

    def _add_reviews(self):
        for kind in candidate.REVIEW_NAMES:
            self.write_review(kind, f"# {kind} review\n\nCandidate commit: {COMMIT}\n".encode())

    @staticmethod
    def fixture_run(run_id, workflow):
        return {"id": run_id, "run_attempt": 1, "path": workflow,
                "head_sha": COMMIT, "head_branch": "main", "event": "push",
                "status": "completed", "conclusion": "success",
                "repository": {"full_name": candidate.REPOSITORY, "id": 1}}

    def github(self, endpoint):
        if "/actions/artifacts/" in endpoint:
            return self.metadata[int(endpoint.rsplit("/", 1)[1])]
        if endpoint.endswith("/jobs?per_page=100"):
            run_id = int(endpoint.split("/actions/runs/", 1)[1].split("/", 1)[0])
            return self.jobs[run_id]
        return self.runs[int(endpoint.split("/actions/runs/", 1)[1].split("/", 1)[0])]

    def verify(self):
        with mock.patch.object(candidate, "_verify_source", return_value=(COMMIT, TREE)), \
             mock.patch.object(candidate, "_verify_payload"), \
             mock.patch.object(candidate, "datetime", FrozenDatetime, create=True):
            candidate.verify(self.record, self.root / "source", self.root / "artifacts",
                             self.root / "evidence", self.root / "reviews", self.github)

    def test_complete_candidate_binds_all_artifacts_and_runs(self):
        self.verify()
        raw = candidate._canonical_bytes(self.record)
        self.assertEqual(raw, candidate._canonical_bytes(json.loads(raw)))

    def test_missing_role_or_changed_run_rejects_candidate(self):
        del self.record["artifacts"]["docs"]
        with self.assertRaisesRegex(ValueError, "every publishable artifact"):
            self.verify()
        self.record["artifacts"]["docs"] = {
            "run_id": 999, "attempt": 1, "artifact_id": 5, "name": "github-pages",
            "zip_sha256": "d" * 64, "files": {"artifact.tar": "e" * 64},
        }
        with self.assertRaises((KeyError, ValueError)):
            self.verify()

    def test_evidence_roles_are_exactly_the_required_lanes(self):
        # CodeQL is not a release gate: the workflow is disabled by maintainer decision.
        self.assertEqual({"ci", "coverage", "fuzz", "sanitizers"}, set(candidate.EVIDENCE_WORKFLOWS))
        self.assertEqual(set(candidate.EVIDENCE_WORKFLOWS), set(candidate.REQUIRED_EVIDENCE_JOBS))
        self.record["evidence"]["security"] = {"run_id": 999, "attempt": 1, "passed_jobs": 3,
                                               "skipped_jobs": 0, "sha256": "d" * 64}
        with self.assertRaisesRegex(ValueError, "every qualification evidence role"):
            self.verify()
        del self.record["evidence"]["security"]
        for kind in sorted(candidate.EVIDENCE_WORKFLOWS):
            with self.subTest(kind=kind):
                entry = self.record["evidence"].pop(kind)
                with self.assertRaisesRegex(ValueError, "every qualification evidence role"):
                    self.verify()
                self.record["evidence"][kind] = entry
        self.verify()

    def test_wrong_artifact_digest_or_review_bytes_rejects_candidate(self):
        self.record["artifacts"]["source"]["zip_sha256"] = "d" * 64
        with self.assertRaisesRegex(ValueError, "metadata"):
            self.verify()
        self.record["artifacts"]["source"]["zip_sha256"] = self.metadata[1]["digest"].removeprefix("sha256:")
        (self.root / "reviews/security.md").write_text("changed\n")
        with self.assertRaisesRegex(ValueError, "review report digest differs"):
            self.verify()

    def test_artifacts_must_stay_retained_through_release_and_rollback(self):
        self.assertEqual(candidate.MIN_REMAINING_RETENTION, timedelta(days=60))
        for artifact_id, metadata in self.metadata.items():
            with self.subTest(artifact=metadata["name"]):
                original = metadata["expires_at"]
                # Not yet expired is not enough: one day short of the window fails.
                metadata["expires_at"] = github_time(NOW + timedelta(days=59))
                with self.assertRaisesRegex(ValueError, "release and rollback window"):
                    self.verify()
                for value in (None, "", "next week", "2027-01-01T00:00:00"):
                    metadata["expires_at"] = value
                    with self.assertRaisesRegex(ValueError, "release and rollback window"):
                        self.verify()
                metadata["expires_at"] = github_time(NOW + timedelta(days=60))
                self.verify()
                metadata["expires_at"] = original

    def test_empty_or_unbound_evidence_is_rejected(self):
        run_id = self.record["evidence"]["ci"]["run_id"]
        valid = self.evidence_document("ci", run_id, self.jobs[run_id]["jobs"])
        cases = {
            "empty": b"",
            "empty object": {},
            "not JSON": b"passed\n",
            "not an object": [valid],
            "missing jobs": {key: value for key, value in valid.items() if key != "jobs"},
            "extra field": {**valid, "note": "all green"},
            "other workflow": {**valid, "workflow": candidate.EVIDENCE_WORKFLOWS["fuzz"]},
            "other run": {**valid, "run_id": run_id + 1},
            "other attempt": {**valid, "attempt": 2},
            "boolean attempt": {**valid, "attempt": True},
            "other commit": {**valid, "commit": "f" * 40},
            "no jobs": {**valid, "jobs": []},
            "failed job": {**valid, "jobs": [{"name": "linux", "conclusion": "failure"},
                                             {"name": "macos", "conclusion": "success"}]},
            "missing lane": {**valid, "jobs": [{"name": "linux", "conclusion": "success"}]},
            "renamed job": {**valid, "jobs": [{"name": "linux", "conclusion": "success"},
                                              {"name": "macos-self-hosted",
                                               "conclusion": "success"}]},
        }
        for case, document in cases.items():
            with self.subTest(case=case):
                self.write_evidence("ci", document)
                with self.assertRaisesRegex(ValueError, "ci evidence|job counts|execution lane"):
                    self.verify()
        self.write_evidence("ci", valid)
        self.verify()

    def test_empty_or_unbound_review_report_is_rejected(self):
        cases = {
            "empty": b"",
            "whitespace": b" \n\t\n",
            "no commit": b"# security review\n\nNo findings.\n",
            "short commit": f"Reviewed {COMMIT[:12]}\n".encode(),
            "other commit": f"Reviewed {'f' * 40}\n".encode(),
            "longer hex": f"Reviewed {COMMIT}ab\n".encode(),
            "not UTF-8": b"Reviewed \xff " + COMMIT.encode() + b"\n",
            "NUL": f"Reviewed {COMMIT}\n\x00".encode(),
        }
        for case, data in cases.items():
            with self.subTest(case=case):
                self.write_review("security", data)
                with self.assertRaisesRegex(ValueError, "security review report"):
                    self.verify()
        self.write_review("security", f"Security review of {COMMIT}: no findings.\n".encode())
        self.verify()

    def test_cross_repository_artifact_metadata_is_rejected(self):
        self.metadata[1]["workflow_run"]["head_repository_id"] = 999
        with self.assertRaisesRegex(ValueError, "artifact metadata"):
            self.verify()

    def test_pr_run_cannot_qualify_main_candidate(self):
        self.runs[100]["event"] = "pull_request"
        with self.assertRaisesRegex(ValueError, "exact-source attempt"):
            self.verify()

    def test_job_counts_must_match_the_complete_attempt_page(self):
        self.record["evidence"]["ci"]["passed_jobs"] += 1
        with self.assertRaisesRegex(ValueError, "job counts"):
            self.verify()

    def test_idle_sanitizer_run_cannot_qualify_candidate(self):
        run_id = self.record["evidence"]["sanitizers"]["run_id"]
        self.jobs[run_id] = {"total_count": 3, "jobs": [
            {"name": "gatekeeper", "conclusion": "success"},
            {"name": "asan", "conclusion": "skipped"},
            {"name": "ubsan", "conclusion": "skipped"},
        ]}
        self.record["evidence"]["sanitizers"]["passed_jobs"] = 1
        self.record["evidence"]["sanitizers"]["skipped_jobs"] = 2
        with self.assertRaisesRegex(ValueError, "required execution lane"):
            self.verify()

    def test_self_hosted_ci_lanes_can_qualify(self):
        ci_id = self.record["evidence"]["ci"]["run_id"]
        self.jobs[ci_id]["jobs"][0]["name"] = "linux-self-hosted"
        self.jobs[ci_id]["jobs"][1]["name"] = "macos-self-hosted"
        self.write_evidence("ci", self.evidence_document("ci", ci_id, self.jobs[ci_id]["jobs"]))
        self.verify()

    def test_coverage_run_without_its_codecov_upload_cannot_qualify(self):
        run_id = self.record["evidence"]["coverage"]["run_id"]
        self.jobs[run_id] = {"total_count": 2, "jobs": [
            {"name": "build", "conclusion": "success"},
            {"name": "upload-coverage", "conclusion": "skipped"},
        ]}
        self.record["evidence"]["coverage"]["passed_jobs"] = 1
        self.record["evidence"]["coverage"]["skipped_jobs"] = 1
        with self.assertRaisesRegex(ValueError, "required execution lane"):
            self.verify()


class SourceAndEditorTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def test_source_metadata_binds_a_clean_commit_and_tree(self):
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        (self.root / "MODULE.bazel").write_text(
            'module(name="donner", version="0.8.0", compatibility_level=1)\n', encoding="utf-8")
        (self.root / ".bazelversion").write_text("8.8.1\n", encoding="utf-8")
        subprocess.run(["git", "-C", str(self.root), "add", "--", "MODULE.bazel", ".bazelversion"],
                       check=True)
        subprocess.run(["git", "-C", str(self.root), "-c", "user.name=Test",
                        "-c", "user.email=test@example.com", "commit", "-qm", "source"], check=True)
        commit = subprocess.check_output(
            ["git", "-C", str(self.root), "rev-parse", "HEAD"], text=True).strip()
        tree = subprocess.check_output(
            ["git", "-C", str(self.root), "rev-parse", "HEAD^{tree}"], text=True).strip()
        source = {"commit": commit, "tree": tree, "version": "0.8.0", "compatibility_level": 1,
                  "bazel_version": "8.8.1",
                  "module_locks_sha256": {"cli-linux": LOCK_LINUX,
                                          "cli-macos": LOCK_MACOS},
                  "compiler_versions": COMPILERS.copy()}
        self.assertEqual(candidate._verify_source(source, self.root), (commit, tree))
        (self.root / "untracked").write_text("dirty")
        with self.assertRaisesRegex(ValueError, "clean exact-source"):
            candidate._verify_source(source, self.root)

    def test_cli_payload_binds_the_compiler_recorded_by_its_provenance(self):
        record = {"source": {"commit": COMMIT,
                             "module_locks_sha256": {"cli-linux": LOCK_LINUX,
                                                     "cli-macos": LOCK_MACOS},
                             "compiler_versions": COMPILERS.copy()}}
        entry = {"run_id": 100, "attempt": 1}
        provenance = {"generated_lock_sha256": LOCK_LINUX,
                      "compiler_version": COMPILERS["cli-linux"]}
        with mock.patch.object(candidate.release_cli, "verify", return_value=provenance):
            candidate._verify_payload("cli-linux", self.root, entry, record, self.root)
            provenance["compiler_version"] = "GCC 13.3.0"
            with self.assertRaisesRegex(ValueError, "compiler differs"):
                candidate._verify_payload("cli-linux", self.root, entry, record, self.root)

    def test_prerelease_module_version_cannot_be_a_release_candidate(self):
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        (self.root / ".bazelversion").write_text("8.8.1\n", encoding="utf-8")
        for version in ("0.8.0-pre", "0.8.0-rc.1", "1.0.0-alpha"):
            with self.subTest(version=version):
                (self.root / "MODULE.bazel").write_text(
                    f'module(name="donner", version="{version}", compatibility_level=1)\n',
                    encoding="utf-8")
                subprocess.run(["git", "-C", str(self.root), "add", "--", "MODULE.bazel",
                                ".bazelversion"], check=True)
                subprocess.run(["git", "-C", str(self.root), "-c", "user.name=Test",
                                "-c", "user.email=test@example.com", "commit", "-qm", version],
                               check=True)
                source = {
                    "commit": subprocess.check_output(
                        ["git", "-C", str(self.root), "rev-parse", "HEAD"], text=True).strip(),
                    "tree": subprocess.check_output(
                        ["git", "-C", str(self.root), "rev-parse", "HEAD^{tree}"],
                        text=True).strip(),
                    "version": version, "compatibility_level": 1, "bazel_version": "8.8.1",
                    "module_locks_sha256": {"cli-linux": LOCK_LINUX, "cli-macos": LOCK_MACOS},
                    "compiler_versions": COMPILERS.copy()}
                with self.assertRaisesRegex(ValueError, "stable X.Y.Z"):
                    candidate._verify_source(source, self.root)

    def test_editor_manifest_covers_site_deploy_and_lock(self):
        directory = self.root / "package"
        directory.mkdir()
        lock = self.root / "donner/editor/wasm/tests/package-lock.json"
        lock.parent.mkdir(parents=True)
        lock.write_bytes(b"lock")
        files = {"site/index.html": b"site", "site/editor.wasm": b"wasm",
                 "site/catalog-fonts.json": b"{}", "deploy/index.html": b"deploy",
                 "deploy/editor.wasm.gz": b"gzip"}
        expected = {name: digest(data) for name, data in files.items()}
        for name, data in files.items():
            target = directory / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        provenance = {"source_revision": COMMIT, "renderer": "geode",
                      "producer_run_id": "12", "producer_attempt": "2",
                      "bazel_configs": ["editor-wasm"], "transport_encoding": "gzip",
                      "targets": ["//tools/ci:editor_wasm_package"], "lockfile_sha256": digest(b"lock")}
        (directory / "provenance.json").write_text(json.dumps(provenance))
        (directory / "SHA256SUMS").write_text(
            "".join(f"{sha}  {name}\n" for name, sha in sorted(expected.items())))
        expected["provenance.json"] = digest((directory / "provenance.json").read_bytes())
        expected["SHA256SUMS"] = digest((directory / "SHA256SUMS").read_bytes())
        candidate._verify_editor(directory, COMMIT, self.root, expected, "12", "2")
        with self.assertRaisesRegex(ValueError, "provenance differs"):
            candidate._verify_editor(directory, COMMIT, self.root, expected, "12", "3")
        provenance["private_host"] = "hidden.example"
        (directory / "provenance.json").write_text(json.dumps(provenance))
        with self.assertRaisesRegex(ValueError, "provenance differs"):
            candidate._verify_editor(directory, COMMIT, self.root, expected, "12", "2")
        del provenance["private_host"]
        (directory / "provenance.json").write_text(json.dumps(provenance))
        del expected["deploy/editor.wasm.gz"]
        with self.assertRaisesRegex(ValueError, "required site or deploy"):
            candidate._verify_editor(directory, COMMIT, self.root, expected, "12", "2")


if __name__ == "__main__":
    unittest.main()
