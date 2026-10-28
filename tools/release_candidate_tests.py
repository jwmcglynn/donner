"""Tests for immutable release-candidate record and retained artifact validation."""

import contextlib
from datetime import datetime, timedelta, timezone
import hashlib
import gzip
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
LOCKS = {"cli-linux": LOCK_LINUX, "cli-macos": LOCK_MACOS, "editor": "1" * 64, "docs": "2" * 64, "editor-macos": "3" * 64}
COMPILERS = {"cli-linux": "clang 21", "cli-macos": "Apple clang 17", "editor-macos": "Apple clang 17"}
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

    def test_registry_entry_inventory_is_the_validated_entry_and_earlier_versions(self):
        required = candidate._fixed_role_files("bcr-entry", "0.8.0")
        self.assertEqual(required, {
            "admission.json", "registry/bazel_registry.json",
            "registry/modules/donner/metadata.json",
            "registry/modules/donner/0.8.0/MODULE.bazel",
            "registry/modules/donner/0.8.0/presubmit.yml",
            "registry/modules/donner/0.8.0/source.json"})
        previous = {"registry/modules/donner/0.7.0/MODULE.bazel",
                    "registry/modules/donner/0.7.0/source.json"}
        candidate._verify_role_files("bcr-entry", required | previous, "0.8.0")
        for name in ("registry/modules/donner/0.8.0/patches/fix.patch",
                     "registry/modules/other/1.0.0/MODULE.bazel",
                     "registry/modules/donner/notes.txt",
                     "registry/modules/donner/latest/MODULE.bazel", "extra.json"):
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, "publishable role"):
                candidate._verify_role_files("bcr-entry", required | {name}, "0.8.0")
        with self.assertRaisesRegex(ValueError, "publishable role"):
            candidate._verify_role_files("bcr-entry", required - {"admission.json"}, "0.8.0")

    def test_editor_inventory_accepts_legacy_or_complete_dialog_script_pair(self):
        legacy = {"provenance.json", "SHA256SUMS", "MODULE.bazel.lock", "site/editor.wasm",
                  "deploy/editor.wasm.gz"}
        legacy.update(f"{prefix}/{name}" for prefix in ("site", "deploy")
                      for name in candidate.EDITOR_COMMON_FILES)
        dialogs = {"site/browser-file-dialogs.js", "deploy/browser-file-dialogs.js"}
        candidate._verify_role_files("editor", legacy, "0.8.0")
        candidate._verify_role_files("editor", legacy | dialogs, "0.8.0")
        for extra in ({name} for name in dialogs):
            with self.subTest(extra=extra), self.assertRaisesRegex(ValueError, "publishable role"):
                candidate._verify_role_files("editor", legacy | extra, "0.8.0")
        with self.assertRaisesRegex(ValueError, "publishable role"):
            candidate._verify_role_files("editor", legacy | dialogs | {"site/extra.js"}, "0.8.0")

    def test_editor_resource_inventory_requires_complete_paired_metadata_and_safe_assets(self):
        names = {"provenance.json", "SHA256SUMS", "MODULE.bazel.lock", "site/editor.wasm",
                 "deploy/editor.wasm.gz"}
        names.update(f"{prefix}/{name}" for prefix in ("site", "deploy")
                     for name in candidate.EDITOR_COMMON_FILES)
        assets = {f"{prefix}/assets/{'a' * 64}.ttf" for prefix in ("site", "deploy")}
        metadata = candidate.EDITOR_RESOURCE_FILES
        candidate._verify_role_files("editor", names | assets | metadata, "0.8.0")
        for incomplete in (assets, metadata - {next(iter(metadata))},
                           assets - {next(iter(assets))}):
            with self.subTest(incomplete=incomplete), self.assertRaises(ValueError):
                candidate._verify_role_files("editor", names | incomplete, "0.8.0")
        for unsafe in ("assets/../font.ttf", "assets/name.ttf", f"assets/{'a' * 64}.exe"):
            invalid = {f"{prefix}/{unsafe}" for prefix in ("site", "deploy")}
            with self.subTest(unsafe=unsafe), self.assertRaises(ValueError):
                candidate._verify_role_files("editor", names | metadata | invalid, "0.8.0")

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
            "schema": 4,
            "source": {"commit": COMMIT, "tree": TREE, "version": "0.8.0",
                       "compatibility_level": 0, "bazel_version": "8.8.1",
                       "module_locks_sha256": LOCKS.copy(),
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
            names = {"provenance.json", "SHA256SUMS", "MODULE.bazel.lock", "site/editor.wasm",
                     "deploy/editor.wasm.gz"}
            names.update(f"{prefix}/{name}" for prefix in ("site", "deploy")
                         for name in candidate.EDITOR_COMMON_FILES)
        return {name: name.encode() for name in sorted(names)}

    def _add_artifacts(self):
        for role, workflow in candidate.ARTIFACT_WORKFLOWS.items():
            # One preflight attempt builds source and CLIs; one docs attempt retains both docs roles.
            run_id = {"editor": 101, "docs": 102, "docs-provenance": 102}.get(role, 100)
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

    def verify(self, **options):
        with mock.patch.object(candidate, "_verify_source", return_value=(COMMIT, TREE)), \
             mock.patch.object(candidate, "_verify_payload"), \
             mock.patch.object(candidate, "datetime", FrozenDatetime, create=True):
            candidate.verify(self.record, self.root / "source", self.root / "artifacts",
                             self.root / "evidence", self.root / "reviews", self.github,
                             **options)

    def test_previous_schema_without_dependency_state_is_rejected(self):
        for schema in (1, 2, 3):
            self.record["schema"] = schema
            with self.subTest(schema=schema), self.assertRaisesRegex(ValueError, "unsupported schema"):
                self.verify()
        self.record["schema"] = 4
        self.verify()
        del self.record["artifacts"]["docs-provenance"]
        with self.assertRaisesRegex(ValueError, "every publishable artifact"):
            self.verify()

    def test_native_editor_role_is_mandatory_and_distinct_from_wasm(self):
        self.verify()
        del self.record["artifacts"]["editor-macos"]
        with self.assertRaisesRegex(ValueError, "every publishable artifact"):
            self.verify()
        self.assertIn("editor", self.record["artifacts"])
        self.assertNotIn("editor-macos", candidate.CLI_PLATFORMS)

    def test_record_binds_the_admission_validated_registry_entry(self):
        self.assertEqual(candidate.SCHEMA, 4)
        self.assertEqual(candidate.ARTIFACT_WORKFLOWS["bcr-entry"],
                         ".github/workflows/bcr_preflight.yml")
        self.assertEqual(self.record["artifacts"]["bcr-entry"]["name"], "donner-bcr-entry-1")
        self.verify()
        entry = self.record["artifacts"].pop("bcr-entry")
        with self.assertRaisesRegex(ValueError, "every publishable artifact"):
            self.verify()
        self.record["artifacts"]["bcr-entry"] = dict(entry, name="donner-bcr-entry-2")
        with self.assertRaisesRegex(ValueError, "artifact name"):
            self.verify()

    def test_registry_entry_must_come_from_the_qualifying_preflight_attempt(self):
        entry = self.record["artifacts"]["bcr-entry"]
        entry["run_id"] = 103
        self.runs[103] = self.fixture_run(103, candidate.ARTIFACT_WORKFLOWS["bcr-entry"])
        self.metadata[entry["artifact_id"]]["workflow_run"]["id"] = 103
        with self.assertRaisesRegex(ValueError, "registry entry artifacts must share one"):
            self.verify()

    def test_docs_provenance_must_come_from_the_docs_build_attempt(self):
        entry = self.record["artifacts"]["docs-provenance"]
        entry["run_id"] = 103
        self.runs[103] = self.fixture_run(103, candidate.ARTIFACT_WORKFLOWS["docs-provenance"])
        self.metadata[entry["artifact_id"]]["workflow_run"]["id"] = 103
        with self.assertRaisesRegex(ValueError, "docs build attempt"):
            self.verify()

    def test_publication_reverification_keeps_only_the_rollback_window(self):
        # Release re-verifies up to 30 days after assembly; the 60-day assembly window would
        # then reject every record, while 30 days still covers post-release rollback.
        for metadata in self.metadata.values():
            metadata["expires_at"] = github_time(NOW + timedelta(days=45))
        with self.assertRaisesRegex(ValueError, "release and rollback window"):
            self.verify()
        self.verify(min_remaining=candidate.PUBLICATION_REMAINING_RETENTION)
        self.metadata[1]["expires_at"] = github_time(NOW + timedelta(days=29))
        with self.assertRaisesRegex(ValueError, "release and rollback window"):
            self.verify(min_remaining=candidate.PUBLICATION_REMAINING_RETENTION)
        self.assertEqual(candidate.PUBLICATION_REMAINING_RETENTION, timedelta(days=30))

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
        for metadata in self.metadata.values():
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
        self.assertEqual(candidate.MIN_REMAINING_RETENTION, timedelta(days=60))

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

    def test_native_editor_payload_binds_its_own_compiler_and_lock(self):
        provenance = {"generated_lock_sha256": LOCKS["editor-macos"],
                      "compiler_version": COMPILERS["editor-macos"]}
        entry = {"run_id": 12, "attempt": 2}
        record = {"source": {"commit": COMMIT, "module_locks_sha256": LOCKS.copy(),
                             "compiler_versions": COMPILERS.copy()}}
        with mock.patch.object(candidate.release_macos_editor, "verify", return_value=provenance):
            candidate._verify_payload("editor-macos", self.root, entry, record, self.root)
            record["source"]["compiler_versions"]["editor-macos"] = "another compiler"
            with self.assertRaisesRegex(ValueError, "native editor compiler"):
                candidate._verify_payload("editor-macos", self.root, entry, record, self.root)
            record["source"]["compiler_versions"]["editor-macos"] = COMPILERS["editor-macos"]
            record["source"]["module_locks_sha256"]["editor-macos"] = "f" * 64
            with self.assertRaisesRegex(ValueError, "native editor|generated lock"):
                candidate._verify_payload("editor-macos", self.root, entry, record, self.root)

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
                  "module_locks_sha256": LOCKS.copy(),
                  "compiler_versions": COMPILERS.copy()}
        self.assertEqual(candidate._verify_source(source, self.root), (commit, tree))
        (self.root / "untracked").write_text("dirty")
        with self.assertRaisesRegex(ValueError, "clean exact-source"):
            candidate._verify_source(source, self.root)

    def test_cli_payload_binds_the_compiler_recorded_by_its_provenance(self):
        record = {"source": {"commit": COMMIT,
                             "module_locks_sha256": LOCKS.copy(),
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
                    "module_locks_sha256": LOCKS.copy(),
                    "compiler_versions": COMPILERS.copy()}
                with self.assertRaisesRegex(ValueError, "stable X.Y.Z"):
                    candidate._verify_source(source, self.root)

    def _editor_package(self):
        """Write a minimal editor package whose build retained its generated module lock."""
        directory = self.root / "package"
        directory.mkdir()
        lock = self.root / "donner/editor/wasm/tests/package-lock.json"
        lock.parent.mkdir(parents=True)
        lock.write_bytes(b"lock")
        module_lock = b'{"registryFileHashes": {"https://bcr.bazel.build/x": "0"}}\n'
        (directory / "MODULE.bazel.lock").write_bytes(module_lock)
        (self.root / "MODULE.bazel").write_text('module(name="donner", version="0.8.0", compatibility_level=0)\n')
        wasm = b"\0asm\1\0\0\0" + b"0.8.0\n" + COMMIT.encode() + b"\n"
        files = {"site/index.html": b"site", "site/editor.wasm": wasm,
                 "site/catalog-fonts.json": b"{}", "deploy/index.html": b"deploy",
                 "deploy/editor.wasm.gz": gzip.compress(wasm, mtime=0)}
        expected = {name: digest(data) for name, data in files.items()}
        for name, data in files.items():
            target = directory / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        provenance = {"source_revision": COMMIT, "renderer": "geode",
                      "producer_run_id": "12", "producer_attempt": "2",
                      "bazel_configs": ["editor-wasm"], "transport_encoding": "gzip",
                      "targets": ["//tools/ci:editor_wasm_package"], "lockfile_sha256": digest(b"lock"),
                      "generated_lock_sha256": digest(module_lock)}
        (directory / "SHA256SUMS").write_text(
            "".join(f"{sha}  {name}\n" for name, sha in sorted(expected.items())))
        expected["SHA256SUMS"] = digest((directory / "SHA256SUMS").read_bytes())
        expected["MODULE.bazel.lock"] = digest(module_lock)
        return directory, provenance, expected

    def _write_editor_provenance(self, directory, provenance, expected):
        (directory / "provenance.json").write_text(json.dumps(provenance))
        expected["provenance.json"] = digest((directory / "provenance.json").read_bytes())

    def test_editor_manifest_covers_site_deploy_and_lock(self):
        directory, provenance, expected = self._editor_package()
        self._write_editor_provenance(directory, provenance, expected)
        self.assertEqual(candidate._verify_editor(directory, COMMIT, self.root, expected, "12", "2"),
                         expected["MODULE.bazel.lock"])
        with self.assertRaisesRegex(ValueError, "provenance differs"):
            candidate._verify_editor(directory, COMMIT, self.root, expected, "12", "3")
        provenance["private_host"] = "hidden.example"
        self._write_editor_provenance(directory, provenance, expected)
        with self.assertRaisesRegex(ValueError, "provenance differs"):
            candidate._verify_editor(directory, COMMIT, self.root, expected, "12", "2")
        del provenance["private_host"]
        self._write_editor_provenance(directory, provenance, expected)
        del expected["deploy/editor.wasm.gz"]
        with self.assertRaisesRegex(ValueError, "required site or deploy"):
            candidate._verify_editor(directory, COMMIT, self.root, expected, "12", "2")

    def test_editor_dialog_script_matches_selected_source_and_preserves_legacy(self):
        script = b"globalThis.dialogs = {};\n"
        cases = {
            "legacy": (False, {}, True),
            "new": (True, {"site": script, "deploy": script}, True),
            "missing": (True, {}, False),
            "missing-site": (True, {"deploy": script}, False),
            "missing-deploy": (True, {"site": script}, False),
            "changed": (True, {"site": script, "deploy": b"changed"}, False),
            "legacy-extra": (False, {"site": script, "deploy": script}, False),
        }
        for name, (has_source, copies, accepted) in cases.items():
            with self.subTest(case=name):
                self.setUp()
                directory, provenance, expected = self._editor_package()
                if has_source:
                    (self.root / "donner/editor/wasm/browser-file-dialogs.js").write_bytes(script)
                for prefix, data in copies.items():
                    relative = f"{prefix}/browser-file-dialogs.js"
                    (directory / relative).write_bytes(data)
                    expected[relative] = digest(data)
                checksums = "".join(f"{sha}  {path}\n" for path, sha in sorted(expected.items())
                                    if path.startswith(("site/", "deploy/")))
                (directory / "SHA256SUMS").write_text(checksums)
                expected["SHA256SUMS"] = digest(checksums.encode())
                self._write_editor_provenance(directory, provenance, expected)
                if accepted:
                    self.assertEqual(
                        candidate._verify_editor(directory, COMMIT, self.root, expected, "12", "2"),
                        expected["MODULE.bazel.lock"])
                else:
                    with self.assertRaisesRegex(ValueError, "dialog script"):
                        candidate._verify_editor(directory, COMMIT, self.root, expected, "12", "2")

    def _rewrite_editor_bytes(self, directory, expected, raw, served):
        for name, data in (("site/editor.wasm", raw), ("deploy/editor.wasm.gz", served)):
            (directory / name).write_bytes(data)
            expected[name] = digest(data)
        checksums = "".join(f"{sha}  {name}\n" for name, sha in sorted(expected.items())
                            if name.startswith(("site/", "deploy/")))
        (directory / "SHA256SUMS").write_text(checksums)
        expected["SHA256SUMS"] = digest(checksums.encode())

    def test_editor_identity_spans_a_decompressed_chunk_boundary(self):
        directory, provenance, expected = self._editor_package()
        identity = b"0.8.0\n" + COMMIT.encode() + b"\n"
        raw = b"\0asm\1\0\0\0" + b"x" * (1024 * 1024 - 8 - 12) + identity + b"y" * (1024 * 1024)
        self._rewrite_editor_bytes(directory, expected, raw, gzip.compress(raw, mtime=0))
        self._write_editor_provenance(directory, provenance, expected)
        self.assertEqual(candidate._verify_editor(directory, COMMIT, self.root, expected, "12", "2"),
                         expected["MODULE.bazel.lock"])

    def test_editor_rejects_wrong_embedded_version_with_the_selected_commit(self):
        directory, provenance, expected = self._editor_package()
        wrong = b"\0asm\1\0\0\0" + b"0.7.0\n" + COMMIT.encode() + b"\n"
        self._rewrite_editor_bytes(directory, expected, wrong, gzip.compress(wrong, mtime=0))
        self._write_editor_provenance(directory, provenance, expected)
        with self.assertRaisesRegex(ValueError, "build identity"):
            candidate._verify_editor(directory, COMMIT, self.root, expected, "12", "2")

    def test_editor_rejects_stale_embedded_identity_with_consistent_provenance(self):
        directory, provenance, expected = self._editor_package()
        stale = b"\0asm\1\0\0\0" + b"0.8.0\n" + b"b" * 40 + b"\n"
        self._rewrite_editor_bytes(directory, expected, stale, gzip.compress(stale, mtime=0))
        self._write_editor_provenance(directory, provenance, expected)
        with self.assertRaisesRegex(ValueError, "build identity"):
            candidate._verify_editor(directory, COMMIT, self.root, expected, "12", "2")

    def test_editor_rejects_different_deployed_payload_with_matching_manifests(self):
        directory, provenance, expected = self._editor_package()
        raw = (directory / "site/editor.wasm").read_bytes()
        self._rewrite_editor_bytes(directory, expected, raw, gzip.compress(b"stale served payload", mtime=0))
        self._write_editor_provenance(directory, provenance, expected)
        with self.assertRaisesRegex(ValueError, "deployed|gzip"):
            candidate._verify_editor(directory, COMMIT, self.root, expected, "12", "2")

    def test_editor_refuses_corrupt_truncated_oversized_or_trailing_gzip(self):
        directory, provenance, expected = self._editor_package()
        raw = (directory / "site/editor.wasm").read_bytes()
        valid = gzip.compress(raw, mtime=0)
        corrupt_crc = bytearray(valid)
        corrupt_crc[-8] ^= 1
        for served in (b"not gzip", bytes(corrupt_crc), valid[:-3],
                       gzip.compress(raw[:-1], mtime=0), gzip.compress(raw + b"extra", mtime=0),
                       valid + b"trailer", valid + gzip.compress(b"", mtime=0)):
            self._rewrite_editor_bytes(directory, expected, raw, served)
            self._write_editor_provenance(directory, provenance, expected)
            with self.assertRaisesRegex(ValueError, "deployed|gzip"):
                candidate._verify_editor(directory, COMMIT, self.root, expected, "12", "2")

    def test_editor_compressed_input_uses_the_existing_artifact_size_bound(self):
        directory, provenance, expected = self._editor_package()
        raw = (directory / "site/editor.wasm").read_bytes()
        served = gzip.compress(raw, mtime=0) + b"x" * len(raw)
        self._rewrite_editor_bytes(directory, expected, raw, served)
        self._write_editor_provenance(directory, provenance, expected)
        with mock.patch.object(candidate, "MAX_UNPACKED_BYTES", len(raw)):
            with self.assertRaisesRegex(ValueError, "artifact size limit"):
                candidate._verify_editor(directory, COMMIT, self.root, expected, "12", "2")

    def test_editor_provenance_binds_its_generated_module_lock(self):
        directory, provenance, expected = self._editor_package()
        legacy = {key: value for key, value in provenance.items() if key != "generated_lock_sha256"}
        self._write_editor_provenance(directory, legacy, expected)
        with self.assertRaisesRegex(ValueError, "provenance differs"):
            candidate._verify_editor(directory, COMMIT, self.root, expected, "12", "2")
        self._write_editor_provenance(directory, {**provenance, "generated_lock_sha256": "e" * 64},
                                      expected)
        with self.assertRaisesRegex(ValueError, "provenance differs"):
            candidate._verify_editor(directory, COMMIT, self.root, expected, "12", "2")
        self._write_editor_provenance(directory, provenance, expected)
        (directory / "MODULE.bazel.lock").write_text('{"path": "/home/runner/private"}')
        with self.assertRaisesRegex(ValueError, "non-public"):
            candidate._verify_editor(directory, COMMIT, self.root, expected, "12", "2")
        (directory / "MODULE.bazel.lock").unlink()
        with self.assertRaisesRegex(ValueError, "lockfile is missing"):
            candidate._verify_editor(directory, COMMIT, self.root, expected, "12", "2")

    def test_registry_entry_payload_is_checked_against_the_record_archive(self):
        record = {"source": {"commit": COMMIT, "version": "0.8.0"},
                  "artifacts": {"source": {"files": {"donner-0.8.0.tar.gz": "4" * 64}}}}
        entry = {"run_id": 100, "attempt": 1}
        with mock.patch.object(candidate.bcr_source, "verify_validated_entry") as check:
            candidate._verify_payload("bcr-entry", self.root / "entry", entry, record, self.root)
        check.assert_called_once_with(self.root / "entry", self.root, "0.8.0", "4" * 64)

    def test_editor_and_docs_locks_must_equal_the_record_dependency_state(self):
        # Docs provenance is checked against the record's Bazel version, which the source check
        # binds to `.bazelversion`, so docs deployment needs no checkout of the candidate.
        record = {"source": {"commit": COMMIT, "module_locks_sha256": LOCKS.copy(),
                             "bazel_version": "8.8.1"},
                  "artifacts": {"docs": {"files": {"artifact.tar": "3" * 64}}}}
        (self.root / ".bazelversion").write_text("9.0.0\n", encoding="utf-8")
        entry = {"run_id": 102, "attempt": 1, "files": {}}
        cases = (("editor", "_verify_editor", candidate, ()),
                 ("docs-provenance", "verify_provenance", candidate.pages_artifact, ("docs",)))
        for role, function, module, _ in cases:
            lock_role = "docs" if role == "docs-provenance" else role
            with self.subTest(role=role):
                with mock.patch.object(module, function, return_value=LOCKS[lock_role]) as check:
                    candidate._verify_payload(role, self.root, entry, record, self.root)
                    if role == "docs-provenance":
                        check.assert_called_once_with(
                            self.root, {"run_id": 102, "attempt": 1, "source_commit": COMMIT,
                                        "archive_sha256": "3" * 64}, "8.8.1")
                with mock.patch.object(module, function, return_value="f" * 64):
                    with self.assertRaisesRegex(ValueError, "generated lock differs"):
                        candidate._verify_payload(role, self.root, entry, record, self.root)



class AssemblyAndReleaseTest(unittest.TestCase):
    """Assemble a record from named attempts, then re-verify it the way Release does."""

    RUNS = {"preflight": (100, 2), "editor": (101, 1), "docs": (102, 1), "ci": (201, 1),
            "coverage": (202, 1), "fuzz": (203, 3), "sanitizers": (204, 1)}
    REVIEWS = {"security": 301, "supply-chain": 302}

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.root = self.base / "source"
        self.root.mkdir()
        (self.root / "MODULE.bazel").write_text(
            'module(name="donner", version="0.8.0", compatibility_level=1)\n', encoding="utf-8")
        (self.root / ".bazelversion").write_text("8.8.1\n", encoding="utf-8")
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        subprocess.run(["git", "-C", str(self.root), "add", "--", "MODULE.bazel", ".bazelversion"],
                       check=True)
        subprocess.run(["git", "-C", str(self.root), "-c", "user.name=Test",
                        "-c", "user.email=test@example.com", "commit", "-qm", "source"], check=True)
        self.commit = subprocess.check_output(
            ["git", "-C", str(self.root), "rev-parse", "HEAD"], text=True).strip()
        self.remote = self.base / "remote"
        self.remote.mkdir()
        self.artifacts = {}
        self.runs = {}
        self.jobs = {}
        self.comments = {}
        self.downloads = []
        for run_input, roles in candidate.ARTIFACT_RUN_INPUTS.items():
            run_id, attempt = self.RUNS[run_input]
            self.runs[run_id] = self.fixture_run(run_id, attempt,
                                                 candidate.ARTIFACT_WORKFLOWS[roles[0]])
            for role in roles:
                self.add_artifact(role, run_id, attempt)
        for kind, workflow in candidate.EVIDENCE_WORKFLOWS.items():
            run_id, attempt = self.RUNS[kind]
            self.runs[run_id] = self.fixture_run(run_id, attempt, workflow)
            names = [alternatives[0] for alternatives in candidate.REQUIRED_EVIDENCE_JOBS[kind]]
            self.jobs[run_id] = {"total_count": len(names) + 1, "jobs": [
                *({"name": name, "conclusion": "success", "id": 9} for name in names),
                {"name": "gatekeeper", "conclusion": "skipped", "id": 9}]}
        for kind, comment_id in self.REVIEWS.items():
            self.comments[comment_id] = {
                "id": comment_id, "author_association": "OWNER",
                "issue_url": f"https://api.github.com/repos/{candidate.REPOSITORY}/issues/1422",
                "body": f"# {kind} review\r\n\r\nReviewed {self.commit}: no open findings.\r\n"}

    def test_documentation_workflow_split_preserves_exact_run_binding(self):
        for path in candidate.pages_artifact.SOURCE_WORKFLOWS:
            run = self.fixture_run(103, 1, candidate.ARTIFACT_WORKFLOWS["docs"])
            run["path"] = path
            candidate._verify_run(run, 103, 1, self.commit, candidate.ARTIFACT_WORKFLOWS["docs"])
            for change in ({"event": "workflow_run"}, {"head_branch": "feature"},
                           {"path": ".github/workflows/unreviewed_docs.yaml"}):
                with self.subTest(path=path, change=change), self.assertRaises(ValueError):
                    candidate._verify_run(run | change, 103, 1, self.commit,
                                          candidate.ARTIFACT_WORKFLOWS["docs"])

    def fixture_run(self, run_id, attempt, workflow):
        # Docs builds run only on main pushes; the other qualifying runs were dispatched.
        event = "push" if workflow == candidate.ARTIFACT_WORKFLOWS["docs"] else "workflow_dispatch"
        return {"id": run_id, "run_attempt": attempt, "path": workflow,
                "head_sha": self.commit, "head_branch": "main", "event": event,
                "status": "completed", "conclusion": "success",
                "repository": {"full_name": candidate.REPOSITORY, "id": 1}}

    def role_files(self, role):
        names = candidate._fixed_role_files(role, "0.8.0")
        if names is None:
            names = {"provenance.json", "SHA256SUMS", "MODULE.bazel.lock", "site/editor.wasm",
                     "deploy/editor.wasm.gz"}
            names.update(f"{prefix}/{name}" for prefix in ("site", "deploy")
                         for name in candidate.EDITOR_COMMON_FILES)
        files = {name: name.encode() for name in sorted(names)}
        lock = f"lock for {role}".encode()
        if role in candidate.CLI_PLATFORMS:
            binary = candidate.release_cli._names(candidate.CLI_PLATFORMS[role])
            files[binary[3]] = lock
            files[binary[2]] = json.dumps({"generated_lock_sha256": digest(lock),
                                           "compiler_version": COMPILERS[role]}).encode()
        elif role == "editor-macos":
            files[candidate.release_macos_editor.NAMES[3]] = lock
            files[candidate.release_macos_editor.NAMES[2]] = json.dumps({
                "generated_lock_sha256": digest(lock), "compiler_version": COMPILERS[role]}).encode()
        elif role in ("editor", "docs-provenance"):
            files["MODULE.bazel.lock"] = lock
            files["provenance.json"] = json.dumps({"generated_lock_sha256": digest(lock)}).encode()
        return files

    def add_artifact(self, role, run_id, attempt, files=None):
        artifact_id = 500 + len(self.artifacts)
        path = self.remote / f"{artifact_id}.zip"
        with zipfile.ZipFile(path, "w") as archive:
            for name, data in (files or self.role_files(role)).items():
                archive.writestr(name, data)
        self.artifacts[artifact_id] = {
            "id": artifact_id, "name": candidate._artifact_name(role, self.commit, attempt),
            "digest": "sha256:" + candidate._sha256(path), "expired": False,
            "expires_at": github_time(NOW + timedelta(days=89)),
            "size_in_bytes": path.stat().st_size,
            "workflow_run": {"id": run_id, "head_sha": self.commit, "head_branch": "main",
                             "repository_id": 1, "head_repository_id": 1}}
        return artifact_id

    def github(self, endpoint):
        prefix = f"repos/{candidate.REPOSITORY}/"
        self.assertTrue(endpoint.startswith(prefix), endpoint)
        path = endpoint.removeprefix(prefix)
        if path.startswith("issues/comments/"):
            return self.comments[int(path.rsplit("/", 1)[1])]
        if path.startswith("actions/artifacts/"):
            return self.artifacts[int(path.rsplit("/", 1)[1])]
        run_id = int(path.split("/")[2])
        if "/artifacts?" in path:
            name = path.split("&name=", 1)[1]
            return {"artifacts": [artifact for artifact in self.artifacts.values()
                                  if artifact["workflow_run"]["id"] == run_id and
                                  artifact["name"] == name]}
        if path.endswith("/jobs?per_page=100"):
            return self.jobs[run_id]
        return self.runs[run_id]

    def download(self, artifact_id, path):
        self.downloads.append(artifact_id)
        path.write_bytes((self.remote / f"{artifact_id}.zip").read_bytes())

    def patches(self):
        stack = contextlib.ExitStack()
        stack.enter_context(mock.patch.object(candidate, "_verify_payload"))
        stack.enter_context(mock.patch.object(candidate, "datetime", FrozenDatetime, create=True))
        return stack

    def assemble(self, output="candidate", work="work", **changes):
        options = {"commit": self.commit, "runs": dict(self.RUNS), "reviews": dict(self.REVIEWS),
                   "root": self.root, "work": self.base / work, "output": self.base / output,
                   "github_json": self.github, "download": self.download, **changes}
        with self.patches():
            return candidate.assemble(**options)

    def test_assembly_writes_one_verified_canonical_record_from_named_attempts(self):
        raw = self.assemble()
        output = self.base / "candidate"
        self.assertEqual((output / "candidate.json").read_bytes(), raw)
        record = json.loads(raw)
        self.assertEqual(raw, candidate._canonical_bytes(record))
        self.assertEqual((output / "candidate.json.sha256").read_text(),
                         f"{hashlib.sha256(raw).hexdigest()}  candidate.json\n")
        self.assertEqual({path.relative_to(output).as_posix() for path in output.rglob("*")
                          if path.is_file()}, candidate.RECORD_FILES)
        self.assertEqual(record["schema"], 4)
        self.assertEqual(record["artifacts"]["bcr-entry"]["name"], "donner-bcr-entry-2")
        self.assertEqual(record["artifacts"]["bcr-entry"]["run_id"], 100)
        self.assertEqual(record["source"]["commit"], self.commit)
        self.assertEqual(record["source"]["version"], "0.8.0")
        self.assertEqual(record["source"]["compatibility_level"], 1)
        self.assertEqual(record["source"]["compiler_versions"], COMPILERS)
        self.assertEqual(record["source"]["module_locks_sha256"],
                         {role: digest(f"lock for {'docs-provenance' if role == 'docs' else role}"
                                       .encode()) for role in candidate.LOCK_ROLES})
        self.assertEqual(record["artifacts"]["docs-provenance"]["run_id"], 102)
        self.assertEqual(record["artifacts"]["docs-provenance"]["name"],
                         "donner-docs-provenance-1")
        self.assertEqual(record["artifacts"]["cli-linux"]["attempt"], 2)
        self.assertEqual(record["evidence"]["fuzz"]["attempt"], 3)
        self.assertEqual(record["evidence"]["ci"]["skipped_jobs"], 1)
        review = (output / "reviews/security.md").read_bytes()
        self.assertEqual(record["reviews"]["security"], digest(review))
        self.assertIn(b"\r\n", review, "review bytes are retained exactly as posted")
        self.assertEqual(len(self.downloads), len(candidate.ARTIFACT_WORKFLOWS))

    def test_assembly_must_run_on_the_candidate_commit(self):
        with self.assertRaisesRegex(ValueError, "while main points at that commit"):
            self.assemble(commit="f" * 40)
        self.assertFalse((self.base / "candidate" / "candidate.json").exists())

    def test_assembly_needs_every_named_input_and_empty_directories(self):
        runs = dict(self.RUNS)
        del runs["sanitizers"]
        with self.assertRaisesRegex(ValueError, "every qualifying run"):
            self.assemble(runs=runs)
        reviews = {"security": 301}
        with self.assertRaisesRegex(ValueError, "every qualifying run"):
            self.assemble(reviews=reviews)
        (self.base / "used").mkdir()
        (self.base / "used" / "stale").write_text("old record")
        with self.assertRaisesRegex(ValueError, "must be new or empty"):
            self.assemble(output="used")

    def test_missing_ambiguous_or_changed_artifacts_fail_assembly(self):
        editor = next(item for item in self.artifacts.values()
                      if item["name"].startswith("donner-editor-wasm-geode-"))
        cases = {
            "missing": lambda: editor.update(name="renamed"),
            "expired": lambda: editor.update(expired=True),
            "ambiguous": lambda: self.add_artifact("editor", 101, 1),
            "changed bytes": lambda: editor.update(digest="sha256:" + "e" * 64),
        }
        for case, change in cases.items():
            with self.subTest(case=case):
                saved = {key: dict(value) for key, value in self.artifacts.items()}
                change()
                with self.assertRaisesRegex(ValueError, "editor"):
                    self.assemble(output=f"out-{case}", work=f"work-{case}")
                self.assertFalse((self.base / f"out-{case}" / "candidate.json").exists())
                self.artifacts.clear()
                self.artifacts.update(saved)
                editor = next(item for item in self.artifacts.values()
                              if item["name"].startswith("donner-editor-wasm-geode-"))

    def retain_rerun_artifacts(self, editor_attempts, docs_attempts, provenance_attempt=2):
        """Retain one editor package and one Pages archive per attempt of runs 101 and 102.

        Their names omit the attempt, so every attempt's artifact shares one name.
        """
        for artifact_id, item in list(self.artifacts.items()):
            if item["workflow_run"]["id"] in (101, 102):
                del self.artifacts[artifact_id]
        editor = {}
        for attempt in editor_attempts:
            files = self.role_files("editor")
            files["provenance.json"] = json.dumps({
                "producer_run_id": "101", "producer_attempt": str(attempt),
                "generated_lock_sha256": digest(files["MODULE.bazel.lock"])}).encode()
            editor[attempt] = self.add_artifact("editor", 101, attempt, files)
        docs = {attempt: self.add_artifact("docs", 102, attempt,
                                           {"artifact.tar": f"site {attempt}".encode()})
                for attempt in docs_attempts}
        files = self.role_files("docs-provenance")
        files["provenance.json"] = json.dumps({
            "pages_archive_sha256": digest(f"site {provenance_attempt}".encode()),
            "generated_lock_sha256": digest(files["MODULE.bazel.lock"])}).encode()
        self.add_artifact("docs-provenance", 102, provenance_attempt, files)
        for run_id in (101, 102):
            self.runs[run_id]["run_attempt"] = 2
        return editor, docs

    def test_rerun_assembles_from_the_artifacts_the_requested_attempt_built(self):
        editor, docs = self.retain_rerun_artifacts(editor_attempts=(1, 2), docs_attempts=(1, 2))
        record = json.loads(self.assemble(runs={**self.RUNS, "editor": (101, 2),
                                                "docs": (102, 2)}))
        self.assertEqual(record["artifacts"]["editor"]["artifact_id"], editor[2])
        self.assertEqual(record["artifacts"]["docs"]["artifact_id"], docs[2])
        self.assertEqual(record["artifacts"]["docs"]["files"]["artifact.tar"],
                         digest(b"site 2"))
        work = self.base / "work" / "artifacts"
        self.assertEqual(sorted(path.name for path in work.iterdir()),
                         sorted(f"{role}.zip" for role in candidate.ARTIFACT_WORKFLOWS))

    def test_rerun_without_one_artifact_from_the_requested_attempt_fails_assembly(self):
        runs = {**self.RUNS, "editor": (101, 2), "docs": (102, 2)}
        cases = {
            "editor": dict(editor_attempts=(1, 1), docs_attempts=(1, 2)),
            "docs": dict(editor_attempts=(1, 2), docs_attempts=(1, 3)),
            "two editor packages claim the attempt": dict(editor_attempts=(2, 2),
                                                          docs_attempts=(1, 2)),
        }
        for case, attempts in cases.items():
            with self.subTest(case=case):
                saved = {key: dict(value) for key, value in self.artifacts.items()}
                self.retain_rerun_artifacts(**attempts)
                role = case.split()[-1] if case in ("editor", "docs") else "editor"
                with self.assertRaisesRegex(ValueError, f"{role} needs exactly one retained "
                                                        "artifact .* built by attempt 2"):
                    self.assemble(runs=runs, output=f"out-{case.replace(' ', '-')}",
                                  work=f"work-{case.replace(' ', '-')}")
                self.artifacts.clear()
                self.artifacts.update(saved)

    def test_review_must_be_an_owner_comment_naming_the_commit(self):
        comment = self.comments[301]
        cases = {
            "not owner": {"author_association": "CONTRIBUTOR"},
            "other repository": {"issue_url": "https://api.github.com/repos/other/donner/issues/1"},
            "pull request review": {"issue_url": None},
            "other comment": {"id": 999},
        }
        for case, change in cases.items():
            with self.subTest(case=case):
                self.comments[301] = {**comment, **change}
                with self.assertRaisesRegex(ValueError, "repository owner's comment"):
                    self.assemble(output=f"out-{case}", work=f"work-{case}")
        self.comments[301] = {**comment, "body": "Reviewed the release; no findings."}
        with self.assertRaisesRegex(ValueError, "does not name the candidate commit"):
            self.assemble(output="out-unbound", work="work-unbound")
        self.assertFalse((self.base / "out-unbound" / "candidate.json").exists())

    def test_failed_evidence_attempt_cannot_be_assembled(self):
        self.jobs[201]["jobs"][0]["conclusion"] = "failure"
        with self.assertRaisesRegex(ValueError, "job counts|not green"):
            self.assemble()

    def test_cli_reads_named_runs_and_review_comments(self):
        self.assertEqual(candidate._run_reference("38190325156/2", "preflight run"),
                         (38190325156, 2))
        for value in ("38190325156", "0/1", "12/0", "12/1 ", "-1/1", "12/01", "1e3/1"):
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, "RUN_ID/ATTEMPT"):
                candidate._run_reference(value, "preflight run")
        for value in ("0", "12a", "", "01"):
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, "positive decimal"):
                candidate._decimal_id(value, "comment ID")

    # Release side.

    def publish_record(self, raw_files=None, attempt=1, run_id=400):
        """Retain the assembled record files as the assembly run's artifact."""
        if raw_files is None:
            self.assemble()
            output = self.base / "candidate"
            raw_files = {path.relative_to(output).as_posix(): path.read_bytes()
                         for path in output.rglob("*") if path.is_file()}
        self.runs[run_id] = self.fixture_run(run_id, attempt, candidate.ASSEMBLY_WORKFLOW)
        path_id = 900 + len(self.artifacts)
        path = self.remote / f"{path_id}.zip"
        with zipfile.ZipFile(path, "w") as archive:
            for name, data in sorted(raw_files.items()):
                archive.writestr(name, data)
        self.artifacts[path_id] = {
            "id": path_id, "name": f"donner-release-candidate-{attempt}",
            "digest": "sha256:" + candidate._sha256(path), "expired": False,
            "expires_at": github_time(NOW + timedelta(days=89)),
            "size_in_bytes": path.stat().st_size,
            "workflow_run": {"id": run_id, "head_sha": self.commit, "head_branch": "main",
                             "repository_id": 1, "head_repository_id": 1}}
        return path_id, hashlib.sha256(raw_files["candidate.json"]).hexdigest(), raw_files

    def release(self, artifact_id, record_sha256, run_id=400, attempt=1, name="release"):
        with self.patches():
            return candidate.release(
                root=self.root, record_run_id=run_id, record_attempt=attempt,
                record_artifact_id=artifact_id, record_sha256=record_sha256,
                work=self.base / f"{name}-work", output=self.base / name,
                github_json=self.github, download=self.download)

    def test_release_reverifies_the_record_and_stages_exactly_its_release_files(self):
        artifact_id, record_sha256, files = self.publish_record()
        self.downloads.clear()
        result = self.release(artifact_id, record_sha256)
        record = json.loads(files["candidate.json"])
        self.assertEqual(result, {"preflight_run_id": "100", "preflight_attempt": "2",
                                  "version": "0.8.0", "record_sha256": record_sha256})
        staged = {path.relative_to(self.base / "release").as_posix(): path.read_bytes()
                  for path in (self.base / "release").rglob("*") if path.is_file()}
        expected = {f"{role}/{name}" for role in candidate.RELEASE_ROLES
                    for name in record["artifacts"][role]["files"]}
        expected.add("donner-0.8.0.candidate.json")
        self.assertEqual(set(staged), expected)
        self.assertEqual(staged["donner-0.8.0.candidate.json"], files["candidate.json"])
        for name, data in staged.items():
            role, _, member = name.partition("/")
            if role in candidate.RELEASE_ROLES:
                self.assertEqual(digest(data), record["artifacts"][role]["files"][member])
        # The record, then every bound artifact by its recorded ID; nothing is selected by name.
        self.assertEqual(self.downloads, [artifact_id] + [
            record["artifacts"][role]["artifact_id"] for role in candidate.ARTIFACT_WORKFLOWS])

    def test_release_rejects_a_different_or_rewritten_record(self):
        artifact_id, record_sha256, files = self.publish_record()
        with self.assertRaisesRegex(ValueError, "differs from the release body"):
            self.release(artifact_id, "0" * 64, name="other-digest")
        changed = dict(files)
        changed["candidate.json.sha256"] = b"0" * 64 + b"  candidate.json\n"
        changed_id, _, _ = self.publish_record(changed, attempt=2, run_id=401)
        with self.assertRaisesRegex(ValueError, "checksum file differs"):
            self.release(changed_id, record_sha256, run_id=401, attempt=2, name="checksum")
        record = json.loads(files["candidate.json"])
        pretty = (json.dumps(record, indent=2) + "\n").encode()
        changed = {**files, "candidate.json": pretty, "candidate.json.sha256":
                   f"{hashlib.sha256(pretty).hexdigest()}  candidate.json\n".encode()}
        pretty_id, pretty_sha256, _ = self.publish_record(changed, attempt=3, run_id=402)
        with self.assertRaisesRegex(ValueError, "canonical"):
            self.release(pretty_id, pretty_sha256, run_id=402, attempt=3, name="pretty")
        extra_id, _, _ = self.publish_record({**files, "notes.txt": b"later edit"}, attempt=4,
                                             run_id=403)
        with self.assertRaisesRegex(ValueError, "missing or unexpected files"):
            self.release(extra_id, record_sha256, run_id=403, attempt=4, name="extra")

    def test_release_rejects_a_record_from_another_attempt_or_expiring_soon(self):
        artifact_id, record_sha256, _ = self.publish_record()
        with self.assertRaisesRegex(ValueError, "named assembly attempt"):
            self.release(artifact_id, record_sha256, attempt=2, name="attempt")
        with self.assertRaisesRegex(ValueError, "named assembly attempt"):
            self.release(artifact_id, record_sha256, run_id=401, name="run")
        self.artifacts[artifact_id]["expires_at"] = github_time(NOW + timedelta(days=29))
        with self.assertRaisesRegex(ValueError, "release and rollback window"):
            self.release(artifact_id, record_sha256, name="expiring")

    def test_release_rejects_changed_or_expiring_bound_artifacts(self):
        artifact_id, record_sha256, files = self.publish_record()
        record = json.loads(files["candidate.json"])
        cli = record["artifacts"]["cli-macos"]["artifact_id"]
        original = (self.remote / f"{cli}.zip").read_bytes()
        (self.remote / f"{cli}.zip").write_bytes(original + b"tail")
        with self.assertRaisesRegex(ValueError, "cli-macos artifact download differs"):
            self.release(artifact_id, record_sha256, name="changed")
        (self.remote / f"{cli}.zip").write_bytes(original)
        editor = record["artifacts"]["editor"]["artifact_id"]
        self.artifacts[editor]["expires_at"] = github_time(NOW + timedelta(days=29))
        with self.assertRaisesRegex(ValueError, "editor artifact expires"):
            self.release(artifact_id, record_sha256, name="expiring")
        self.artifacts[editor]["expires_at"] = github_time(NOW + timedelta(days=31))
        self.release(artifact_id, record_sha256, name="within-window")

    def test_published_role_is_fetched_by_recorded_id_and_verified(self):
        _, _, files = self.publish_record()
        record = json.loads(files["candidate.json"])
        entry = record["artifacts"]["bcr-entry"]
        self.downloads.clear()
        with self.patches() as stack:
            payload = stack.enter_context(mock.patch.object(candidate, "_verify_payload"))
            output = candidate.fetch_role(record, "bcr-entry", self.root, self.base / "entry.zip",
                                          self.base / "entry", self.github, self.download)
        self.assertEqual(self.downloads, [entry["artifact_id"]])
        self.assertEqual({path.relative_to(output).as_posix() for path in output.rglob("*")
                          if path.is_file()}, set(entry["files"]))
        payload.assert_called_once_with("bcr-entry", output, entry, record, self.root)

    def test_editor_fetch_requires_the_clean_exact_source_checkout(self):
        record = json.loads(self.assemble())
        with self.patches():
            candidate.fetch_role(record, "editor", self.root, self.base / "clean.zip",
                                 self.base / "clean", self.github, self.download)
            (self.root / "untracked.txt").write_text("dirty")
            with self.assertRaisesRegex(ValueError, "clean exact-source"):
                candidate.fetch_role(record, "editor", self.root, self.base / "dirty.zip",
                                     self.base / "dirty", self.github, self.download)
            (self.root / "untracked.txt").unlink()
            record["source"]["commit"] = "f" * 40
            with self.assertRaisesRegex(ValueError, "clean exact-source"):
                candidate.fetch_role(record, "editor", self.root, self.base / "other.zip",
                                     self.base / "other", self.github, self.download)

    def test_published_role_must_still_be_retained_and_unchanged(self):
        _, _, files = self.publish_record()
        record = json.loads(files["candidate.json"])
        entry = record["artifacts"]["bcr-entry"]
        metadata = self.artifacts[entry["artifact_id"]]
        # Past the publication window an artifact is still usable until it expires.
        metadata["expires_at"] = github_time(NOW + timedelta(days=1))
        with self.patches():
            candidate.fetch_role(record, "bcr-entry", self.root, self.base / "ok.zip",
                                 self.base / "ok", self.github, self.download)
        cases = {
            "expired": (lambda: metadata.update(expires_at=github_time(NOW - timedelta(days=1))),
                        "expires before"),
            "changed bytes": (lambda: (self.remote / f"{entry['artifact_id']}.zip").write_bytes(
                b"other"), "download differs"),
            "other run": (lambda: metadata["workflow_run"].update(id=999), "metadata"),
        }
        for case, (change, message) in cases.items():
            with self.subTest(case=case):
                saved = json.loads(json.dumps(metadata))
                original = (self.remote / f"{entry['artifact_id']}.zip").read_bytes()
                change()
                with self.patches(), self.assertRaisesRegex(ValueError, message):
                    candidate.fetch_role(record, "bcr-entry", self.root,
                                         self.base / f"{case}.zip", self.base / case,
                                         self.github, self.download)
                metadata.clear()
                metadata.update(saved)
                (self.remote / f"{entry['artifact_id']}.zip").write_bytes(original)

    def test_load_record_requires_the_approved_canonical_bytes(self):
        _, record_sha256, files = self.publish_record()
        raw = files["candidate.json"]
        self.assertEqual(candidate.load_record(raw, record_sha256), json.loads(raw))
        with self.assertRaisesRegex(ValueError, "differs from the approved record"):
            candidate.load_record(raw, "0" * 64)
        pretty = (json.dumps(json.loads(raw), indent=2) + "\n").encode()
        with self.assertRaisesRegex(ValueError, "canonical"):
            candidate.load_record(pretty, digest(pretty))
        old = candidate._canonical_bytes({**json.loads(raw), "schema": 2})
        with self.assertRaisesRegex(ValueError, "unsupported schema"):
            candidate.load_record(old, digest(old))

    # Docs deployment side.

    def docs_record(self, record_sha256, run_id=400, attempt=1, name="docs"):
        with self.patches():
            return candidate.docs_record(record_run_id=run_id, record_attempt=attempt,
                                         record_sha256=record_sha256,
                                         work=self.base / f"{name}-record",
                                         github_json=self.github, download=self.download)

    def docs_selection(self, record_sha256, run_id=400, attempt=1, name="docs"):
        """Fetch the record, then select from it, as the deploy job does around its attestation."""
        fetched = self.docs_record(record_sha256, run_id, attempt, name)
        with self.patches():
            return candidate.docs_selection(record_path=Path(fetched["record_path"]),
                                            record_commit=fetched["record_commit"],
                                            record_sha256=record_sha256,
                                            work=self.base / name, github_json=self.github,
                                            download=self.download)

    def test_docs_deploy_selects_exactly_the_record_docs_build(self):
        artifact_id, record_sha256, files = self.publish_record()
        record = json.loads(files["candidate.json"])
        docs = record["artifacts"]["docs"]
        self.downloads.clear()
        fetched = self.docs_record(record_sha256)
        self.assertEqual(fetched, {"record_path": str(self.base / "docs-record/record/candidate.json"),
                                   "record_commit": self.commit})
        # Fetching the record downloads nothing it names: the job verifies the attestation first.
        self.assertEqual(self.downloads, [artifact_id])
        with self.patches():
            result = candidate.docs_selection(
                record_path=Path(fetched["record_path"]), record_commit=self.commit,
                record_sha256=record_sha256, work=self.base / "docs",
                github_json=self.github, download=self.download)
        self.assertEqual(result, {
            "artifact_id": str(docs["artifact_id"]), "run_id": "102", "attempt": "1",
            "source_commit": self.commit, "artifact_digest": "sha256:" + docs["zip_sha256"],
            "archive_sha256": docs["files"]["artifact.tar"], "record_sha256": record_sha256})
        # Then the docs provenance; the Pages artifact is promoted by ID, not copied.
        self.assertEqual(self.downloads,
                         [artifact_id, record["artifacts"]["docs-provenance"]["artifact_id"]])

    def test_docs_deploy_rejects_an_unapproved_or_unassembled_record(self):
        _, record_sha256, _ = self.publish_record()
        with self.assertRaisesRegex(ValueError, "differs from the dispatch input"):
            self.docs_selection("0" * 64, name="digest")
        for case, change in {"push": {"event": "push"}, "branch": {"head_branch": "feature"},
                             "failed": {"conclusion": "failure"},
                             "other workflow": {"path": candidate.ARTIFACT_WORKFLOWS["docs"]}}.items():
            with self.subTest(case=case):
                saved = dict(self.runs[400])
                self.runs[400].update(change)
                with self.assertRaisesRegex(ValueError, "successful manual assembly on main"):
                    self.docs_selection(record_sha256, name=f"run-{case}")
                self.runs[400] = saved
        with self.assertRaisesRegex(ValueError, "successful manual assembly"):
            self.docs_selection(record_sha256, attempt=2, name="attempt")

    def test_docs_deploy_rejects_a_record_for_another_commit_than_its_assembly(self):
        _, record_sha256, _ = self.publish_record()
        # A consistent assembly run and record artifact of another commit, whose record names
        # this commit: the record cannot select docs for a commit its assembly did not check.
        other = "f" * 40
        self.runs[400]["head_sha"] = other
        record_artifact = next(item for item in self.artifacts.values()
                               if item["name"] == "donner-release-candidate-1")
        record_artifact["workflow_run"]["head_sha"] = other
        fetched = self.docs_record(record_sha256, name="other")
        self.assertEqual(fetched["record_commit"], other)
        with self.patches(), self.assertRaisesRegex(ValueError, "another commit than its assembly"):
            candidate.docs_selection(record_path=Path(fetched["record_path"]), record_commit=other,
                                     record_sha256=record_sha256, work=self.base / "other",
                                     github_json=self.github, download=self.download)

    def test_docs_deploy_rejects_changed_or_expired_docs_artifacts(self):
        _, record_sha256, files = self.publish_record()
        record = json.loads(files["candidate.json"])
        provenance = record["artifacts"]["docs-provenance"]["artifact_id"]
        original = (self.remote / f"{provenance}.zip").read_bytes()
        (self.remote / f"{provenance}.zip").write_bytes(original + b"tail")
        with self.assertRaisesRegex(ValueError, "docs-provenance artifact download differs"):
            self.docs_selection(record_sha256, name="changed")
        (self.remote / f"{provenance}.zip").write_bytes(original)
        docs = self.artifacts[record["artifacts"]["docs"]["artifact_id"]]
        docs["expires_at"] = github_time(NOW - timedelta(hours=1))
        with self.assertRaisesRegex(ValueError, "docs artifact expires"):
            self.docs_selection(record_sha256, name="expired")

    def test_docs_selection_modes_are_exclusive(self):
        rollback = {"DOC_ARTIFACT_ID": "456", "DOC_SOURCE_RUN_ID": "123", "DOC_SOURCE_ATTEMPT": "1",
                    "DOC_SOURCE_COMMIT": "a" * 40, "DOC_ARTIFACT_DIGEST": "sha256:" + "b" * 64,
                    "DOC_ARCHIVE_SHA256": "c" * 64}
        def args(selection, path="", commit="", sha="", run=""):
            return candidate._parser().parse_args(
                ["docs-selection", "--selection", selection, "--record-run", run,
                 "--record-path", path, "--record-commit", commit, "--record-sha256", sha,
                 "--work", str(self.base / "unused")])
        record = args("record", "candidate.json", "a" * 40, "e" * 64, "400/1")
        with self.assertRaisesRegex(ValueError, "must not also name rollback values"):
            candidate._docs_selection_command(record, rollback)
        for name, value in (("sha", ""), ("commit", ""), ("path", "")):
            with self.subTest(missing=name), \
                    self.assertRaisesRegex(ValueError, "needs the fetched record"):
                values = {"path": "candidate.json", "commit": "a" * 40, "sha": "e" * 64,
                          name: value}
                candidate._docs_selection_command(args("record", **values), {})
        for named in ({"run": "400/1"}, {"sha": "e" * 64}, {"path": "candidate.json"}):
            with self.subTest(named=named), \
                    self.assertRaisesRegex(ValueError, "must not also name a candidate record"):
                candidate._docs_selection_command(args("rollback", **named), rollback)
        with mock.patch.dict(candidate.os.environ, rollback, clear=True):
            self.assertEqual(candidate._docs_selection_command(args("rollback"), rollback), {
                "selection": "rollback", "artifact_id": "456", "run_id": "123", "attempt": "1",
                "source_commit": "a" * 40, "artifact_digest": "sha256:" + "b" * 64,
                "archive_sha256": "c" * 64})
        with mock.patch.object(candidate, "docs_selection", return_value={"run_id": "102"}) as select:
            self.assertEqual(candidate._docs_selection_command(record, {"DOC_ARTIFACT_ID": ""}),
                             {"selection": "record", "run_id": "102"})
        select.assert_called_once_with(record_path=Path("candidate.json"), record_commit="a" * 40,
                                       record_sha256="e" * 64, work=self.base / "unused")
        with self.assertRaisesRegex(ValueError, "RUN_ID/ATTEMPT"):
            candidate._docs_record_command(candidate._parser().parse_args(
                ["docs-record", "--record-run", "400", "--record-sha256", "e" * 64,
                 "--work", str(self.base / "unused")]))

    def test_release_rejects_a_record_for_another_commit(self):
        artifact_id, record_sha256, _ = self.publish_record()
        (self.root / "NOTICE").write_text("later change\n")
        subprocess.run(["git", "-C", str(self.root), "add", "--", "NOTICE"], check=True)
        subprocess.run(["git", "-C", str(self.root), "-c", "user.name=Test",
                        "-c", "user.email=test@example.com", "commit", "-qm", "later"], check=True)
        with self.assertRaisesRegex(ValueError, "named assembly attempt"):
            self.release(artifact_id, record_sha256, name="later")


if __name__ == "__main__":
    unittest.main()
