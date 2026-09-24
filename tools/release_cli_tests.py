"""Tests for release CLI packaging and retained-byte verification."""

import json
from pathlib import Path
import subprocess
import tempfile
import unittest

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
        lock = self.root / "MODULE.bazel.lock"
        lock.write_text("different lockfile\n")
        with self.assertRaisesRegex(ValueError, "provenance"):
            self._verify()
        lock.write_text("fixture:MODULE.bazel.lock\n")
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


if __name__ == "__main__":
    unittest.main()
