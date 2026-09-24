"""Tests for selecting retained documentation bytes without rebuilding them."""

from __future__ import annotations

import io
from hashlib import sha256
import json
from unittest import mock
from pathlib import Path
import tarfile
import tempfile
import unittest

from tools import pages_artifact


class PagesArtifactTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.archive = Path(self.temp.name) / "artifact.tar"
        self._write_archive("index.html", b"<h1>reviewed docs</h1>")
        archive_digest = sha256(self.archive.read_bytes()).hexdigest()
        self.selected = {
            "artifact_id": 456,
            "run_id": 123,
            "attempt": 1,
            "source_commit": "a" * 40,
            "artifact_digest": "sha256:" + "b" * 64,
            "archive_sha256": archive_digest,
        }
        self.artifact = {
            "id": 456,
            "name": "github-pages",
            "expired": False,
            "size_in_bytes": self.archive.stat().st_size,
            "digest": self.selected["artifact_digest"],
            "workflow_run": {
                "id": 123,
                "head_branch": "main",
                "head_sha": "a" * 40,
                "repository_id": 321,
                "head_repository_id": 321,
            },
        }
        self.run = {
            "id": 123,
            "run_attempt": 1,
            "path": ".github/workflows/deploy_docs.yaml",
            "event": "push",
            "head_branch": "main",
            "head_sha": "a" * 40,
            "status": "completed",
            "conclusion": "success",
            "repository": {"id": 321, "full_name": "jwmcglynn/donner"},
        }

    def _write_archive(self, name: str, contents: bytes, *, symlink: bool = False,
                       marker: bytes | None = b'{"source_attempt": 1, "source_commit": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "source_run_id": 123}') -> None:
        with tarfile.open(self.archive, "w") as output:
            item = tarfile.TarInfo(name)
            if symlink:
                item.type = tarfile.SYMTYPE
                item.linkname = "/outside"
                output.addfile(item)
            else:
                item.size = len(contents)
                output.addfile(item, io.BytesIO(contents))
            if marker is not None:
                build = tarfile.TarInfo("docs-build.json")
                build.size = len(marker)
                output.addfile(build, io.BytesIO(marker))

    def test_previous_successful_main_build_can_be_selected_for_rollback(self) -> None:
        pages_artifact.verify_metadata(self.artifact, self.run, self.selected)
        pages_artifact.verify_archive(self.archive, self.selected["archive_sha256"], self.selected)

    def test_rejects_wrong_run_attempt_or_branch(self) -> None:
        self.run["run_attempt"] = 2
        with self.assertRaisesRegex(ValueError, "attempt"):
            pages_artifact.verify_metadata(self.artifact, self.run, self.selected)
        self.run["run_attempt"] = 1
        self.run["event"] = "pull_request"
        with self.assertRaisesRegex(ValueError, "successful main"):
            pages_artifact.verify_metadata(self.artifact, self.run, self.selected)

    def test_rejects_expired_or_substituted_artifact(self) -> None:
        self.artifact["expired"] = True
        with self.assertRaisesRegex(ValueError, "expired"):
            pages_artifact.verify_metadata(self.artifact, self.run, self.selected)
        self.artifact["expired"] = False
        self.artifact["digest"] = "sha256:" + "c" * 64
        with self.assertRaisesRegex(ValueError, "digest"):
            pages_artifact.verify_metadata(self.artifact, self.run, self.selected)

    def test_rejects_modified_archive_bytes(self) -> None:
        self.archive.write_bytes(self.archive.read_bytes() + b"tampered")
        with self.assertRaisesRegex(ValueError, "archive bytes"):
            pages_artifact.verify_archive(self.archive, self.selected["archive_sha256"], self.selected)

    def test_rejects_missing_index_and_symlink(self) -> None:
        self._write_archive("other.html", b"other")
        with self.assertRaisesRegex(ValueError, "index.html"):
            pages_artifact.verify_archive(self.archive, sha256(self.archive.read_bytes()).hexdigest(),
                                          self.selected)
        self._write_archive("index.html", b"", symlink=True)
        with self.assertRaisesRegex(ValueError, "unsafe"):
            pages_artifact.verify_archive(self.archive, sha256(self.archive.read_bytes()).hexdigest(),
                                          self.selected)

    def test_rejects_missing_or_wrong_build_marker_before_deployment(self) -> None:
        self._write_archive("index.html", b"page", marker=None)
        with self.assertRaisesRegex(ValueError, "no docs-build.json"):
            pages_artifact.verify_archive(self.archive, sha256(self.archive.read_bytes()).hexdigest(),
                                          self.selected)
        self._write_archive("index.html", b"page", marker=b"not json")
        with self.assertRaisesRegex(ValueError, "malformed"):
            pages_artifact.verify_archive(self.archive, sha256(self.archive.read_bytes()).hexdigest(),
                                          self.selected)
        self._write_archive("index.html", b"page", marker=b'{"source_attempt": 2}')
        with self.assertRaisesRegex(ValueError, "does not match"):
            pages_artifact.verify_archive(self.archive, sha256(self.archive.read_bytes()).hexdigest(),
                                          self.selected)

    def test_ids_must_be_canonical_before_deployment(self) -> None:
        with self.assertRaisesRegex(ValueError, "canonical"):
            pages_artifact.positive_id("00123", "run ID")
        with self.assertRaisesRegex(ValueError, "canonical"):
            pages_artifact.positive_id("01", "attempt")

    def test_build_marker_binds_source_run(self) -> None:
        marker = Path(self.temp.name) / "docs-build.json"
        with mock.patch.dict(
            "os.environ",
            {"GITHUB_SHA": "a" * 40, "GITHUB_RUN_ID": "123", "GITHUB_RUN_ATTEMPT": "1"},
        ):
            pages_artifact.write_build_marker(marker)
        self.assertEqual(
            json.loads(marker.read_text()),
            {"source_commit": "a" * 40, "source_run_id": 123, "source_attempt": 1},
        )


if __name__ == "__main__":
    unittest.main()
