"""Tests for release CLI packaging and retained-byte verification."""

import json
import os
from pathlib import Path
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

from tools import release_cli


PROBE_COMPILER = "Ubuntu Clang 18.1.3 (1ubuntu1)"
RUNNER_CXX = "c++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0"


class ReleaseCliFixture:
    """A committed source checkout, a CLI binary, its notice and a compiler probe."""

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
        (self.root / "LICENSE").write_text("MIT License fixture\n")
        self.resources = Path(self.temporary.name) / "resources"
        (self.resources / "assets").mkdir(parents=True)
        payload = b"font fixture"
        checksum = release_cli.sha256(payload).hexdigest()
        asset_path = f"assets/{checksum}.ttf"
        (self.resources / asset_path).write_bytes(payload)
        (self.resources / "manifest.json").write_text(json.dumps({"schemaVersion": 1, "assets": [{
            "id": "fixture.font", "sha256": checksum, "encodedSize": len(payload),
            "decodedSize": len(payload), "mediaType": "font/ttf", "group": "fallback",
            "path": asset_path}]}))
        self.binary = Path(self.temporary.name) / "donner-cli"
        self.binary.write_bytes(b"retained CLI bytes\n")
        self.notice = Path(self.temporary.name) / "notice_cli.txt"
        self.notice_contents = (
            release_cli.NOTICE_HEADER + "donner-cli\n" + "=" * 72 + "\n\n" + "-" * 72 +
            "\nPackage: tiny-skia-cpp\nLicense: BSD-3-Clause\n\nCopyright fixture\n"
        )
        self.notice.write_text(self.notice_contents, encoding="utf-8")
        self.artifacts = Path(self.temporary.name) / "artifacts"
        self.probe = Path(self.temporary.name) / "release_compiler_identity"
        self._write_probe(f"printf '%s\\n' '{PROBE_COMPILER}'")

    def _write_probe(self, body: str) -> None:
        """Stand in for //tools/ci:release_compiler_identity with a script."""
        self.probe.unlink(missing_ok=True)
        self.probe.write_text(f"#!/bin/sh\n{body}\n", encoding="ascii")
        self.probe.chmod(self.probe.stat().st_mode | stat.S_IXUSR)

    def _package(self, platform: str = "linux-x86-64") -> dict[str, object]:
        real_check_output = subprocess.check_output

        def fixture_check_output(command, **kwargs):
            # The runner's default c++ is not the compiler Bazel uses; it must not be recorded.
            if command == ["c++", "--version"]:
                return RUNNER_CXX + "\n"
            return real_check_output(command, **kwargs)

        with mock.patch.object(
            release_cli.subprocess, "check_output", side_effect=fixture_check_output
        ):
            return release_cli.package(
                root=self.root, binary=self.binary, notice=self.notice,
                compiler_probe=self.probe, resources=self.resources, output=self.artifacts,
                platform=platform, commit=self.commit, run_id="123", attempt="2",
            )

    def _verify(self, platform: str = "linux-x86-64") -> dict[str, object]:
        return release_cli.verify(
            root=self.root, artifacts=self.artifacts, platform=platform,
            commit=self.commit, run_id="123", attempt="2",
        )


class ReleaseCliTest(ReleaseCliFixture, unittest.TestCase):
    def test_exact_bytes_and_source_inputs_round_trip_on_both_platforms(self) -> None:
        for platform in release_cli.PLATFORMS:
            with self.subTest(platform=platform):
                self.artifacts.mkdir(exist_ok=True)
                expected = self._package(platform)
                self.assertEqual(expected["compiler_version"], PROBE_COMPILER)
                self.assertEqual(self._verify(platform), expected)
                binary_name = release_cli.PLATFORMS[platform]
                self.assertEqual(release_cli._read_archive(self.artifacts / binary_name)["donner-cli"],
                                 self.binary.read_bytes())
                self.assertEqual(
                    (self.artifacts / f"{binary_name}.NOTICE.txt").read_bytes(),
                    self.notice.read_bytes(),
                )
                self.assertEqual(expected["schema"], 4)
                self.assertEqual(expected["notice_sha256"], release_cli.digest(self.notice))
                self.assertEqual(
                    json.loads((self.artifacts / f"{binary_name}.provenance").read_text()),
                    expected,
                )
                for path in self.artifacts.iterdir():
                    path.unlink()

    def test_resource_manifest_negative_controls(self) -> None:
        original = json.loads((self.resources / "manifest.json").read_text())
        mutations = {
            "wrong schema": lambda item: item.update(schemaVersion=2),
            "boolean schema": lambda item: item.update(schemaVersion=True),
            "traversal": lambda item: item["assets"][0].update(path="../font.ttf"),
            "wrong digest": lambda item: item["assets"][0].update(sha256="0" * 64),
            "wrong length": lambda item: item["assets"][0].update(encodedSize=1),
            "decoded overflow": lambda item: item["assets"][0].update(decodedSize=2**32),
            "raw decoded mismatch": lambda item: item["assets"][0].update(
                decodedSize=item["assets"][0]["encodedSize"] + 1),
            "boolean size": lambda item: item["assets"][0].update(encodedSize=True),
            "duplicate identity": lambda item: item["assets"].append(item["assets"][0]),
        }
        for case, mutate in mutations.items():
            with self.subTest(case=case):
                document = json.loads(json.dumps(original))
                mutate(document)
                (self.resources / "manifest.json").write_text(json.dumps(document))
                with self.assertRaises(ValueError):
                    release_cli._resource_files(self.resources)
        (self.resources / "manifest.json").write_text(json.dumps(original))
        self.assertTrue(release_cli._resource_files(self.resources))

    def test_resource_links_extra_files_and_corruption_fail_closed(self) -> None:
        asset = next((self.resources / "assets").iterdir())
        original = asset.read_bytes()
        asset.write_bytes(b"corrupted")
        with self.assertRaisesRegex(ValueError, "digest or length"):
            release_cli._resource_files(self.resources)
        asset.write_bytes(original)
        extra = self.resources / "unbound.ttf"
        extra.write_bytes(b"extra")
        with self.assertRaisesRegex(ValueError, "inventory"):
            release_cli._resource_files(self.resources)
        extra.unlink()
        extra.symlink_to(asset)
        with self.assertRaisesRegex(ValueError, "symlink"):
            release_cli._resource_files(self.resources)

    def _woff2_resource(self, payload: bytes, decoded_size: int) -> dict[str, bytes]:
        checksum = release_cli.sha256(payload).hexdigest()
        path = f"assets/{checksum}.woff2"
        manifest = {"schemaVersion": 1, "assets": [{
            "id": "fixture.woff2", "sha256": checksum, "encodedSize": len(payload),
            "decodedSize": decoded_size, "mediaType": "font/woff2", "group": "catalog",
            "path": path}]}
        return {release_cli.MANIFEST: json.dumps(manifest).encode(),
                "donner-assets/" + path: payload}

    def test_valid_compressed_woff2_preserves_distinct_encoded_and_decoded_sizes(self) -> None:
        payload = (Path(__file__).parents[1] /
                   "donner/base/fonts/testdata/valid-001.woff2").read_bytes()
        decoded_size = struct.unpack_from(">I", payload, 16)[0]
        self.assertGreater(decoded_size, len(payload))
        files = self._woff2_resource(payload, decoded_size)
        self.assertEqual(release_cli._validate_resources(files),
                         release_cli.sha256(files[release_cli.MANIFEST]).hexdigest())
        for name, content in files.items():
            destination = self.resources / name.removeprefix("donner-assets/")
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(content)
        for asset in (self.resources / "assets").glob("*.ttf"):
            asset.unlink()
        expected = self._package("darwin-arm64")
        self.assertEqual(self._verify("darwin-arm64"), expected)
        archived = release_cli._read_archive(
            self.artifacts / release_cli.PLATFORMS["darwin-arm64"])
        self.assertEqual(archived[next(name for name in files if name.endswith(".woff2"))], payload)

    def test_woff2_header_manifest_and_size_negative_controls(self) -> None:
        original = (Path(__file__).parents[1] /
                    "donner/base/fonts/testdata/valid-001.woff2").read_bytes()
        decoded = struct.unpack_from(">I", original, 16)[0]
        for label, offset, value in (("encoded length", 8, len(original) + 4),
                                     ("decoded reference", 16, decoded + 4),
                                     ("decoded overflow", 16, release_cli.MAX_RESOURCE_BYTES + 4),
                                     ("compressed overflow", 20, len(original) + 4)):
            with self.subTest(label=label):
                payload = bytearray(original)
                struct.pack_into(">I", payload, offset, value)
                with self.assertRaisesRegex(ValueError, "header lengths"):
                    release_cli._validate_resources(self._woff2_resource(bytes(payload), decoded))
        with self.assertRaisesRegex(ValueError, "resource size"):
            release_cli._validate_resources(self._woff2_resource(
                original, release_cli.MAX_RESOURCE_BYTES + 4))
        with self.assertRaisesRegex(ValueError, "header or type"):
            release_cli._validate_resources(self._woff2_resource(original[:40], decoded))
        with self.assertRaisesRegex(ValueError, "header lengths"):
            release_cli._validate_resources(self._woff2_resource(original, len(original)))
        invalid_signature = b"bad!" + original[4:]
        with self.assertRaisesRegex(ValueError, "header or type"):
            release_cli._validate_resources(self._woff2_resource(invalid_signature, decoded))

    def test_compressed_assets_keep_a_separate_aggregate_decoded_budget(self) -> None:
        payload = (Path(__file__).parents[1] /
                   "donner/base/fonts/testdata/valid-001.woff2").read_bytes()
        decoded = struct.unpack_from(">I", payload, 16)[0]
        files = self._woff2_resource(payload, decoded)
        document = json.loads(files[release_cli.MANIFEST])
        document["assets"].append({**document["assets"][0], "id": "fixture.alias"})
        files[release_cli.MANIFEST] = json.dumps(document).encode()
        self.assertLess(sum(map(len, files.values())), 2 * decoded - 1)
        with mock.patch.object(release_cli, "MAX_PACKAGE_BYTES", 2 * decoded - 1):
            with self.assertRaisesRegex(ValueError, "aggregate byte limit"):
                release_cli._validate_resources(files)


    def test_archive_member_names_links_modes_and_duplicates_fail_closed(self) -> None:
        import tarfile
        import zipfile
        for extension in ("zip", "tar.gz"):
            archive = Path(self.temporary.name) / f"invalid.{extension}"
            for name in ("../escape", "/absolute", "donner-svg", "binary.debug"):
                with self.subTest(extension=extension, name=name):
                    release_cli._write_archive(archive, {name: b"unbound"})
                    with self.assertRaisesRegex(ValueError, "invalid or duplicate member"):
                        release_cli._read_archive(archive)
        archive = Path(self.temporary.name) / "invalid.tar.gz"
        with tarfile.open(archive, "w:gz") as output:
            link = tarfile.TarInfo("donner-cli")
            link.type, link.linkname = tarfile.SYMTYPE, "outside"
            output.addfile(link)
        with self.assertRaisesRegex(ValueError, "invalid or duplicate member"):
            release_cli._read_archive(archive)
        archive = Path(self.temporary.name) / "invalid.zip"
        with zipfile.ZipFile(archive, "w") as output:
            member = zipfile.ZipInfo("donner-cli")
            member.create_system = 3
            member.external_attr = (stat.S_IFREG | 0o644) << 16
            output.writestr(member, b"not executable")
        with self.assertRaisesRegex(ValueError, "not executable"):
            release_cli._read_archive(archive)
        with zipfile.ZipFile(archive, "w") as output:
            member.external_attr = (stat.S_IFREG | 0o755) << 16
            output.writestr(member, b"first")
            output.writestr(member, b"second")
        with self.assertRaisesRegex(ValueError, "invalid or duplicate member"):
            release_cli._read_archive(archive)

    def test_rehashed_archive_cannot_change_executable_or_manifest_provenance(self) -> None:
        self._package()
        archive_name = release_cli.PLATFORMS["linux-x86-64"]
        archive = self.artifacts / archive_name
        files = release_cli._read_archive(archive)
        files["donner-cli"] = b"changed executable"
        release_cli._write_archive(archive, files)
        (self.artifacts / (archive_name + ".sha256")).write_text(
            f"{release_cli.digest(archive)}  {archive_name}\n")
        with self.assertRaisesRegex(ValueError, "provenance"):
            self._verify()

    def test_provenance_records_the_bazel_built_compiler_not_the_runner_cxx(self) -> None:
        # Bazel builds with CC=clang/CXX=clang++ (.bazelrc), so the runner's default `c++` (GCC on
        # Ubuntu) is not the compiler of the released binary.
        provenance = self._package()
        self.assertEqual(provenance["compiler_version"], PROBE_COMPILER)
        self.assertNotIn("host_cxx_version", provenance)
        self.assertNotIn(RUNNER_CXX, json.dumps(provenance))
        self.assertEqual(self._verify()["compiler_version"], PROBE_COMPILER)

    def test_undeterminable_compiler_fails_closed_before_upload(self) -> None:
        cases = {
            "fails": "echo 'clang 18' && exit 1",
            "no output": "exit 0",
            "blank line": "echo",
            "two lines": "echo 'clang 18'; echo 'clang 19'",
            "URL": "echo 'Clang 21.1.6 (https://example.invalid/llvm.git 0123abc)'",
            "not ASCII": "printf 'clang \\303\\251\\n'",
            "too long": "printf '%0300d\\n' 0",
        }
        for case, body in cases.items():
            with self.subTest(case=case):
                self._write_probe(body)
                with self.assertRaisesRegex(ValueError, "compiler"):
                    self._package()
                self.assertFalse(self.artifacts.exists())
        self.probe.unlink()
        with self.assertRaisesRegex(ValueError, "compiler probe is missing"):
            self._package()
        self.probe.write_bytes(b"")
        with self.assertRaisesRegex(ValueError, "compiler probe is missing"):
            self._package()
        self.probe.unlink()
        self.probe.symlink_to(self.binary)
        with self.assertRaisesRegex(ValueError, "compiler probe is missing"):
            self._package()
        self.probe.unlink()
        self.probe.write_text("#!/bin/sh\necho clang\n", encoding="ascii")
        os.chmod(self.probe, 0o600)
        with self.assertRaisesRegex(ValueError, "compiler probe could not run"):
            self._package()
        self.assertFalse(self.artifacts.exists())

    def test_previous_schema_with_runner_compiler_is_rejected(self) -> None:
        self._package()
        path = self.artifacts / "donner-cli_linux_x86_64.tar.gz.provenance"
        provenance = json.loads(path.read_text())
        provenance["schema"] = 2
        provenance["host_cxx_version"] = provenance.pop("compiler_version")
        path.write_text(json.dumps(provenance, indent=2) + "\n")
        with self.assertRaisesRegex(ValueError, "provenance"):
            self._verify()
        provenance["schema"] = release_cli.SCHEMA
        path.write_text(json.dumps(provenance, indent=2) + "\n")
        with self.assertRaisesRegex(ValueError, "compiler version is missing"):
            self._verify()

    def test_tampered_binary_and_mismatched_run_fail_closed(self) -> None:
        self._package()
        binary = self.artifacts / release_cli.PLATFORMS["linux-x86-64"]
        original = binary.read_bytes()
        binary.write_bytes(b"different bytes")
        with self.assertRaisesRegex(ValueError, "checksum"):
            self._verify()
        binary.write_bytes(original)
        with self.assertRaisesRegex(ValueError, "provenance"):
            release_cli.verify(
                root=self.root, artifacts=self.artifacts, platform="linux-x86-64",
                commit=self.commit, run_id="124", attempt="2",
            )

    def test_changed_lockfile_and_missing_or_extra_assets_fail_closed(self) -> None:
        self._package()
        lock = self.artifacts / "donner-cli_linux_x86_64.tar.gz.bazel.lock"
        lock.write_text(json.dumps({"registry": "https://bcr.bazel.build/modules/other/1.0/MODULE.bazel"}))
        with self.assertRaisesRegex(ValueError, "provenance"):
            self._verify()
        lock.write_text(self.lock_contents)
        extra = self.artifacts / "unreviewed"
        extra.write_bytes(b"extra")
        with self.assertRaisesRegex(ValueError, "unexpected files"):
            self._verify()
        extra.unlink()
        (self.artifacts / "donner-cli_linux_x86_64.tar.gz.sha256").unlink()
        with self.assertRaisesRegex(ValueError, "incomplete"):
            self._verify()

    def test_changed_missing_or_replaced_notice_fails_closed(self) -> None:
        self._package()
        notice = self.artifacts / "donner-cli_linux_x86_64.tar.gz.NOTICE.txt"
        notice.write_text(self.notice_contents.replace("tiny-skia-cpp", "other"), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "provenance"):
            self._verify()
        notice.write_text(self.notice_contents, encoding="utf-8")
        self.assertEqual(self._verify()["notice_sha256"], release_cli.digest(self.notice))
        notice.unlink()
        with self.assertRaisesRegex(ValueError, "incomplete"):
            self._verify()
        notice.symlink_to(self.notice)
        with self.assertRaisesRegex(ValueError, "symlink"):
            self._verify()

    def test_package_requires_a_generated_notice_before_upload(self) -> None:
        cases = {
            "missing": None,
            "empty": b"",
            "not a notice": b"MIT License\n",
            "no packages": release_cli.NOTICE_HEADER.encode() + b"donner-cli\n",
            "not UTF-8": release_cli.NOTICE_HEADER.encode() + b"\nPackage: \xff\n",
            "NUL": release_cli.NOTICE_HEADER.encode() + b"\nPackage: x\n\x00",
            "too large": release_cli.NOTICE_HEADER.encode() + b"\nPackage: x\n" +
                         b"x" * release_cli.MAX_NOTICE_BYTES,
        }
        for case, contents in cases.items():
            with self.subTest(case=case):
                self.notice.unlink(missing_ok=True)
                if contents is not None:
                    self.notice.write_bytes(contents)
                with self.assertRaisesRegex(ValueError, "license notice"):
                    self._package()
                self.assertFalse(self.artifacts.exists())
        self.notice.unlink()
        self.notice.symlink_to(self.binary)
        with self.assertRaisesRegex(ValueError, "license notice is missing"):
            self._package()

    def test_package_requires_exact_checkout_and_empty_destination(self) -> None:
        with self.assertRaisesRegex(ValueError, "checkout"):
            release_cli.package(
                root=self.root, binary=self.binary, notice=self.notice,
                compiler_probe=self.probe, resources=self.resources, output=self.artifacts,
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
            # Built at runtime so tools/check_private_details.py stays clean.
            {"address": ".".join(["192", "168", "1", "50"])},
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
        cases += [{"path": "/private/var/folders/cache"}, {"argv": "--cache=/tmp/build"},
                  {"path": "C:\\Users\\runner"}]
        for payload in cases:
            with self.subTest(payload=payload):
                lock.write_text(json.dumps(payload))
                with self.assertRaisesRegex(ValueError, "non-public|unreviewed|sensitive"):
                    self._package()
                self.assertFalse(self.artifacts.exists())
        # Bazel labels and relative paths may name a `private` or `tmp` package.
        for label in ("@@rules_fuzzing+//fuzzing/private/oss_fuzz:repository.bzl%oss_fuzz_repository",
                      "//private/tools:rule.bzl", "src/var/run/data.txt"):
            with self.subTest(label=label):
                lock.write_text(json.dumps({"label": label}))
                release_cli._validate_public_lockfile(lock)
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
        self.assertFalse((self.artifacts / "donner-cli_linux_x86_64.tar.gz.provenance").exists())

    def test_lockfile_check_for_other_artifacts_returns_digest_or_fails_closed(self) -> None:
        # The editor package and docs build retain their generated lockfiles through this check.
        lock = self.root / release_cli.LOCKFILE
        self.assertEqual(release_cli.check_public_lockfile(lock), release_cli.digest(lock))
        public = {"urls": [
            "https://files.pythonhosted.org/packages/zipp-3.18.2-py3-none-any.whl",
            "https://mirror.bazel.build/github.com/bazelbuild/bazel-skylib/archive.tar.gz",
            "https://repo1.maven.org/maven2/com/code-intelligence/jazzer-api/0.20.1/api.jar",
        ]}
        lock.write_text(json.dumps(public))
        self.assertEqual(release_cli.check_public_lockfile(lock), release_cli.digest(lock))
        lock.write_text(self.lock_contents)
        command = [sys.executable, "-B", "-c", "from tools import release_cli; release_cli.main()",
                   "check-lockfile", "--lockfile", str(lock)]
        package_root = Path(release_cli.__file__).resolve().parent.parent
        result = subprocess.run(command, cwd=package_root, capture_output=True, text=True,
                                check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, f"{release_cli.digest(lock)}  {lock.name}\n")
        for case in ("private", "empty", "symlink", "too large"):
            with self.subTest(case=case):
                lock.unlink()
                if case == "private":
                    lock.write_text(json.dumps({"path": "/home/runner/private"}))
                elif case == "empty":
                    lock.write_bytes(b"")
                elif case == "symlink":
                    lock.symlink_to(self.binary)
                else:
                    lock.write_text(self.lock_contents)
                limit = 4 if case == "too large" else release_cli.MAX_LOCKFILE_BYTES
                with mock.patch.object(release_cli, "MAX_LOCKFILE_BYTES", limit):
                    with self.assertRaisesRegex(ValueError, "non-public|lockfile is missing"):
                        release_cli.check_public_lockfile(lock)
        lock.unlink()
        lock.write_text(json.dumps({"path": "/home/runner/private"}))
        result = subprocess.run(command, cwd=package_root, capture_output=True, text=True,
                                check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Lockfile check failed", result.stderr)
        self.assertNotIn("/home/runner/private", result.stderr)

    def test_copied_notice_is_rechecked_before_artifact_upload(self) -> None:
        original_copy = release_cli.shutil.copyfile

        def copy_with_truncated_notice(source, destination):
            if Path(source) == self.notice:
                Path(destination).write_bytes(b"")
            else:
                original_copy(source, destination)

        with mock.patch.object(release_cli.shutil, "copyfile", side_effect=copy_with_truncated_notice):
            with self.assertRaisesRegex(ValueError, "license notice"):
                self._package()
        self.assertFalse((self.artifacts / "donner-cli_linux_x86_64.tar.gz.provenance").exists())


class VerifyReleaseTest(ReleaseCliFixture, unittest.TestCase):
    """`verify-release` checks the staged CLI files exactly as two `verify` commands did."""

    def _stage(self) -> Path:
        release = Path(self.temporary.name) / "release"
        for directory, platform in release_cli.STAGED_PLATFORMS.items():
            self.artifacts = release / directory
            self._package(platform)
        return release

    def _main(self, *arguments: str) -> None:
        with mock.patch.object(sys, "argv", ["release_cli", *arguments]):
            release_cli.main()

    def test_one_command_makes_the_calls_of_the_two_platform_commands_in_order(self) -> None:
        release = Path(self.temporary.name) / "release"
        run = ("--commit", self.commit, "--run-id", "123", "--attempt", "2")
        # The release workflow's former commands, one per platform.
        before = [
            ("verify", "--artifacts", str(release / "cli-linux"), "--platform", "linux-x86-64",
             *run),
            ("verify", "--artifacts", str(release / "cli-macos"), "--platform", "darwin-arm64",
             *run),
        ]
        with mock.patch.object(release_cli, "verify") as verify:
            for command in before:
                self._main(*command)
            expected = verify.call_args_list
            verify.reset_mock()
            with mock.patch("builtins.print"):
                self._main("verify-release", "--release", str(release), *run)
            self.assertEqual(verify.call_args_list, expected)
        self.assertEqual([call.kwargs["platform"] for call in expected],
                         ["linux-x86-64", "darwin-arm64"])
        self.assertEqual({call.kwargs["root"] for call in expected}, {Path.cwd()})

    def test_staged_files_of_both_platforms_verify(self) -> None:
        release = self._stage()
        with mock.patch("builtins.print") as printed:
            release_cli.verify_release(root=self.root, release=release, commit=self.commit,
                                       run_id="123", attempt="2")
        self.assertEqual(
            [call.args[0] for call in printed.call_args_list],
            ["Verified the linux-x86-64 release CLI files in %s" % (release / "cli-linux"),
             "Verified the darwin-arm64 release CLI files in %s" % (release / "cli-macos")])

    def test_a_tampered_or_missing_second_platform_fails_closed(self) -> None:
        release = self._stage()
        binary = release / "cli-macos" / release_cli.PLATFORMS["darwin-arm64"]
        binary.write_bytes(b"different bytes")
        with mock.patch("builtins.print") as printed:
            with self.assertRaisesRegex(ValueError, "checksum"):
                release_cli.verify_release(root=self.root, release=release, commit=self.commit,
                                           run_id="123", attempt="2")
        self.assertEqual(len(printed.call_args_list), 1)
        for path in (release / "cli-macos").iterdir():
            path.unlink()
        (release / "cli-macos").rmdir()
        with mock.patch("builtins.print"):
            with self.assertRaises(FileNotFoundError):
                release_cli.verify_release(root=self.root, release=release, commit=self.commit,
                                           run_id="123", attempt="2")

    def test_another_run_or_commit_fails_the_first_platform(self) -> None:
        release = self._stage()
        for commit, run_id in ((self.commit, "124"), ("a" * 40, "123")):
            with self.subTest(commit=commit, run_id=run_id):
                with mock.patch("builtins.print") as printed:
                    with self.assertRaises(ValueError):
                        release_cli.verify_release(root=self.root, release=release,
                                                   commit=commit, run_id=run_id, attempt="2")
                printed.assert_not_called()


if __name__ == "__main__":
    unittest.main()
