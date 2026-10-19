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

    def _selection_environment(self) -> dict[str, str]:
        return {
            "GITHUB_REPOSITORY": "jwmcglynn/donner",
            "DOC_ARTIFACT_ID": "456", "DOC_SOURCE_RUN_ID": "123", "DOC_SOURCE_ATTEMPT": "1",
            "DOC_SOURCE_COMMIT": "a" * 40, "DOC_ARTIFACT_DIGEST": self.selected["artifact_digest"],
            "DOC_ARCHIVE_SHA256": self.selected["archive_sha256"],
        }

    def _verify_command(self, responses: dict[str, object], environ: dict[str, str]):
        """Runs `verify` as the deploy step does, with `gh api` answering from `responses`."""
        calls = []

        def gh(command, text):
            calls.append(command)
            self.assertTrue(text)
            return json.dumps(responses[command[2]])

        argv = ["pages_artifact", "verify", "--archive", str(self.archive)]
        with mock.patch.dict("os.environ", environ), mock.patch("sys.argv", argv), \
                mock.patch.object(pages_artifact.subprocess, "check_output", side_effect=gh), \
                mock.patch("builtins.print"):
            pages_artifact.main()
        return calls

    def test_verify_fetches_the_selected_metadata_like_the_previous_workflow_step(self) -> None:
        artifact = "repos/jwmcglynn/donner/actions/artifacts/456"
        run = "repos/jwmcglynn/donner/actions/runs/123/attempts/1"
        calls = self._verify_command({artifact: self.artifact, run: self.run},
                                     self._selection_environment())
        # The two `gh api` reads the workflow step made before calling `verify`, in its order.
        self.assertEqual(calls, [["gh", "api", artifact], ["gh", "api", run]])

    def test_verify_rejects_fetched_metadata_for_another_build(self) -> None:
        artifact = "repos/jwmcglynn/donner/actions/artifacts/456"
        run = "repos/jwmcglynn/donner/actions/runs/123/attempts/1"
        self.run["head_sha"] = "c" * 40
        with self.assertRaisesRegex(ValueError, "successful main docs build"):
            self._verify_command({artifact: self.artifact, run: self.run},
                                 self._selection_environment())

    def test_verify_fetches_nothing_for_a_noncanonical_selection(self) -> None:
        environ = {**self._selection_environment(), "DOC_ARTIFACT_ID": "0456"}
        # No response is configured, so any `gh api` read would fail with KeyError instead.
        with self.assertRaisesRegex(ValueError, "canonical"):
            self._verify_command({}, environ)

    def test_summary_records_the_retained_build_like_the_previous_workflow_step(self) -> None:
        summary = Path(self.temp.name) / "summary.md"
        summary.write_text("earlier step\n", encoding="utf-8")
        argv = ["pages_artifact", "summary", "--archive", str(self.archive)]
        environ = {"GITHUB_SHA": "a" * 40, "GITHUB_RUN_ID": "123", "GITHUB_RUN_ATTEMPT": "2",
                   "ARTIFACT_ID": "456", "GITHUB_STEP_SUMMARY": str(summary)}
        with mock.patch.dict("os.environ", environ), mock.patch("sys.argv", argv):
            pages_artifact.main()
        digest = self.selected["archive_sha256"]
        # Byte for byte what the previous inline step appended to the job summary.
        self.assertEqual(summary.read_text(encoding="utf-8"), (
            "earlier step\n"
            "### Retained documentation artifact\n"
            f"- Source commit: `{'a' * 40}`\n"
            "- Build run: `123`, attempt `2`\n"
            "- Pages artifact ID: `456`\n"
            f"- artifact.tar SHA-256: `{digest}`\n"
            "- Artifact metadata digest: retrieve from the Actions artifact API before manual "
            "deployment\n"
            "- Build provenance: `donner-docs-provenance-2`\n"))

    def test_summary_fails_without_the_retained_archive(self) -> None:
        with self.assertRaises(FileNotFoundError):
            pages_artifact.build_summary(Path(self.temp.name) / "missing.tar")

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

    def _write_provenance(self) -> Path:
        root = Path(self.temp.name) / "source"
        root.mkdir(exist_ok=True)
        (root / ".bazelversion").write_text("8.8.1\n", encoding="utf-8")
        self.lock = root / "MODULE.bazel.lock"
        self.lock.write_text('{"registryFileHashes": {"https://bcr.bazel.build/x": "0"}}\n')
        output = Path(self.temp.name) / "docs-provenance"
        with mock.patch.dict(
            "os.environ",
            {"GITHUB_SHA": "a" * 40, "GITHUB_RUN_ID": "123", "GITHUB_RUN_ATTEMPT": "1"},
        ):
            pages_artifact.write_provenance(self.archive, self.lock, output, root,
                                            "1.15.0 (abc123)", "dot - graphviz version 2.43.0 (0)")
        return output

    def test_docs_provenance_binds_archive_lockfile_and_tools(self) -> None:
        output = self._write_provenance()
        self.assertEqual({path.name for path in output.iterdir()},
                         {"provenance.json", "MODULE.bazel.lock"})
        lock_sha256 = sha256(self.lock.read_bytes()).hexdigest()
        provenance = json.loads((output / "provenance.json").read_text())
        self.assertEqual(provenance, {
            "schema": 1, "source_commit": "a" * 40, "producer_run_id": 123,
            "producer_attempt": 1, "pages_archive_sha256": self.selected["archive_sha256"],
            "generated_lock_sha256": lock_sha256, "bazel_version": "8.8.1",
            "doxygen_version": "1.15.0 (abc123)",
            "graphviz_version": "dot - graphviz version 2.43.0 (0)",
        })
        self.assertEqual(pages_artifact.verify_provenance(output, self.selected, "8.8.1"),
                         lock_sha256)

    def test_docs_provenance_rejects_other_builds_locks_and_tools(self) -> None:
        output = self._write_provenance()
        path = output / "provenance.json"
        original = json.loads(path.read_text())
        changes = {
            "other attempt": {"producer_attempt": 2},
            "string run": {"producer_run_id": "123"},
            "other commit": {"source_commit": "c" * 40},
            "other archive": {"pages_archive_sha256": "d" * 64},
            "other lock": {"generated_lock_sha256": "e" * 64},
            "other Bazel": {"bazel_version": "7.6.0"},
            "tool newline": {"doxygen_version": "1.15.0\nextra"},
            "extra field": {"host": "builder"},
        }
        for case, change in changes.items():
            with self.subTest(case=case):
                path.write_text(json.dumps({**original, **change}))
                with self.assertRaisesRegex(ValueError, "does not match the selected Pages build"):
                    pages_artifact.verify_provenance(output, self.selected, "8.8.1")
        path.write_text(json.dumps(original))
        (output / "MODULE.bazel.lock").write_text('{"path": "/home/runner/private"}')
        with self.assertRaisesRegex(ValueError, "non-public"):
            pages_artifact.verify_provenance(output, self.selected, "8.8.1")
        (output / "extra.txt").write_text("unexpected")
        with self.assertRaisesRegex(ValueError, "unexpected files"):
            pages_artifact.verify_provenance(output, self.selected, "8.8.1")

    def test_docs_provenance_refuses_private_lockfile_or_reused_output(self) -> None:
        output = self._write_provenance()
        with mock.patch.dict(
            "os.environ",
            {"GITHUB_SHA": "a" * 40, "GITHUB_RUN_ID": "123", "GITHUB_RUN_ATTEMPT": "1"},
        ):
            with self.assertRaisesRegex(ValueError, "must be empty"):
                pages_artifact.write_provenance(self.archive, self.lock, output,
                                                self.lock.parent, "1.15.0", "2.43.0")
            self.lock.write_text('{"path": "/home/runner/private"}')
            with self.assertRaisesRegex(ValueError, "non-public"):
                pages_artifact.write_provenance(self.archive, self.lock,
                                                Path(self.temp.name) / "other",
                                                self.lock.parent, "1.15.0", "2.43.0")

    def test_tool_version_reads_one_ascii_line_or_fails_closed(self) -> None:
        completed = mock.Mock(returncode=0, stdout=b"", stderr=b"dot - graphviz version 2.43.0 (0)\n")
        with mock.patch.object(pages_artifact.subprocess, "run", return_value=completed):
            self.assertEqual(pages_artifact.tool_version(["dot", "-V"]),
                             "dot - graphviz version 2.43.0 (0)")
        for result in (mock.Mock(returncode=1, stdout=b"1.15.0\n", stderr=b""),
                       mock.Mock(returncode=0, stdout=b"", stderr=b""),
                       mock.Mock(returncode=0, stdout=b"1.15.0 \xff\n", stderr=b""),
                       mock.Mock(returncode=0, stdout=b"1.15.0; rm\n", stderr=b"")):
            with self.subTest(result=result.stdout):
                with mock.patch.object(pages_artifact.subprocess, "run", return_value=result):
                    with self.assertRaisesRegex(ValueError, "tool version"):
                        pages_artifact.tool_version(["doxygen", "--version"])
        with mock.patch.object(pages_artifact.subprocess, "run", side_effect=OSError):
            with self.assertRaisesRegex(ValueError, "could not be read"):
                pages_artifact.tool_version(["doxygen", "--version"])


if __name__ == "__main__":
    unittest.main()
