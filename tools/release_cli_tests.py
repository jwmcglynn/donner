"""Tests for release CLI packaging and retained-byte verification."""

import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

from tools import release_cli


class ReleaseCliTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "source"
        self.root.mkdir()
        for name in release_cli.INPUTS:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("8.8.0\n" if name == ".bazelversion" else f"fixture:{name}\n")
        subprocess.run(["git", "init", "--quiet", str(self.root)], check=True)
        subprocess.run(["git", "add", "."], cwd=self.root, check=True)
        subprocess.run(
            ["git", "-c", "user.name=Test", "-c", "user.email=test@example.invalid",
             "commit", "--quiet", "-m", "fixture"],
            cwd=self.root, check=True,
        )
        self.commit = subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=self.root, text=True
        ).strip()
        self.lock_contents = json.dumps({
            "registry": "https://bcr.bazel.build/modules/donner/0.8.0/MODULE.bazel",
        }) + "\n"
        (self.root / release_cli.LOCKFILE).write_text(self.lock_contents)
        self.binary = Path(self.temporary.name) / "donner-svg"
        self.binary.write_bytes(b"retained CLI bytes\n")
        self.artifacts = Path(self.temporary.name) / "artifacts"

    def _package(self, platform: str = "linux-x86-64") -> dict[str, object]:
        return release_cli.package(
            root=self.root, binary=self.binary, output=self.artifacts,
            platform=platform, commit=self.commit, run_id="123", attempt="2",
        )

    def _verify(self, platform: str = "linux-x86-64") -> dict[str, object]:
        return release_cli.verify(
            root=self.root, artifacts=self.artifacts, platform=platform,
            commit=self.commit, run_id="123", attempt="2",
        )

    def test_exact_bytes_and_source_inputs_round_trip_on_both_platforms(self) -> None:
        for platform in release_cli.PLATFORMS:
            with self.subTest(platform=platform):
                self.artifacts.mkdir(exist_ok=True)
                expected = self._package(platform)
                self.assertEqual(self._verify(platform), expected)
                binary_name = release_cli.PLATFORMS[platform]
                self.assertEqual((self.artifacts / binary_name).read_bytes(), self.binary.read_bytes())
                self.assertEqual(
                    json.loads((self.artifacts / f"{binary_name}.provenance").read_text()),
                    expected,
                )
                for path in self.artifacts.iterdir():
                    path.unlink()

    def test_tampered_binary_and_mismatched_run_fail_closed(self) -> None:
        self._package()
        binary = self.artifacts / release_cli.PLATFORMS["linux-x86-64"]
        binary.write_bytes(b"different bytes")
        with self.assertRaisesRegex(ValueError, "checksum"):
            self._verify()
        binary.write_bytes(self.binary.read_bytes())
        with self.assertRaisesRegex(ValueError, "provenance"):
            release_cli.verify(
                root=self.root, artifacts=self.artifacts, platform="linux-x86-64",
                commit=self.commit, run_id="124", attempt="2",
            )

    def test_changed_lockfile_and_missing_or_extra_assets_fail_closed(self) -> None:
        self._package()
        lock = self.artifacts / "donner-svg_linux_x86_64.bazel.lock"
        lock.write_text(json.dumps({"registry": "https://bcr.bazel.build/modules/other/1.0/MODULE.bazel"}))
        with self.assertRaisesRegex(ValueError, "provenance"):
            self._verify()
        lock.write_text(self.lock_contents)
        extra = self.artifacts / "unreviewed"
        extra.write_bytes(b"extra")
        with self.assertRaisesRegex(ValueError, "unexpected files"):
            self._verify()
        extra.unlink()
        (self.artifacts / "donner-svg_linux_x86_64.sha256").unlink()
        with self.assertRaisesRegex(ValueError, "incomplete"):
            self._verify()

    def test_package_requires_exact_checkout_and_empty_destination(self) -> None:
        with self.assertRaisesRegex(ValueError, "checkout"):
            release_cli.package(
                root=self.root, binary=self.binary, output=self.artifacts,
                platform="linux-x86-64", commit="a" * 40, run_id="123", attempt="2",
            )
        self._package()
        with self.assertRaisesRegex(ValueError, "must be empty"):
            self._package()

    def test_package_requires_a_generated_lockfile(self) -> None:
        (self.root / release_cli.LOCKFILE).unlink()
        with self.assertRaisesRegex(ValueError, "lockfile is missing"):
            self._package()
        self.assertFalse(self.artifacts.exists())

    def test_private_lockfile_content_and_symlink_fail_before_upload(self) -> None:
        lock = self.root / release_cli.LOCKFILE
        cases = [
            {"path": "file:///home/runner/private/module"},
            {"path": "/root/credentials"},
            {"address": "192.168.1.50"},
            {"url": "https://user:password@github.com/archive"},
            {"url": "HTTPS://internal.example.invalid/module"},
            {"url": "ssh://internal.example.invalid/repo"},
            {"url": "https://storage.googleapis.com/archive?Signature=private"},
            {"url": "https://internal.example.invalid/module"},
            {"access_token": "credential"},
            {"Authorization": "Bearer sample-credential"},
            {"client_secret": "credential"},
            {"value": "github_pat_abcdefghijklmnopqrstuvwx"},
            {"secret": "credential"},
        ]
        for payload in cases:
            with self.subTest(payload=payload):
                lock.write_text(json.dumps(payload))
                with self.assertRaisesRegex(ValueError, "non-public|unreviewed|sensitive"):
                    self._package()
                self.assertFalse(self.artifacts.exists())
        lock.unlink()
        lock.symlink_to(self.binary)
        with self.assertRaisesRegex(ValueError, "lockfile is missing"):
            self._package()

    def test_copied_lockfile_is_rechecked_before_artifact_upload(self) -> None:
        original_copy = release_cli.shutil.copyfile

        def copy_with_changed_lock(source, destination):
            if Path(source).name == release_cli.LOCKFILE:
                Path(destination).write_text(json.dumps({"path": "/home/runner/private"}))
            else:
                original_copy(source, destination)

        with mock.patch.object(release_cli.shutil, "copyfile", side_effect=copy_with_changed_lock):
            with self.assertRaisesRegex(ValueError, "non-public"):
                self._package()
        self.assertFalse((self.artifacts / "donner-svg_linux_x86_64.provenance").exists())


if __name__ == "__main__":
    unittest.main()
