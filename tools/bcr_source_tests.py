"""Tests for source archives, provenance, and registry consumer isolation."""

import io
import json
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest
from unittest import mock

from tools import bcr_source as source


class ArchiveSafetyTest(unittest.TestCase):
    def archive(self, entries):
        stream = io.BytesIO()
        with tarfile.open(fileobj=stream, mode="w") as archive:
            for name, kind in entries:
                item = tarfile.TarInfo(name)
                item.type = kind
                item.size = 0
                archive.addfile(item, io.BytesIO())
        stream.seek(0)
        return tarfile.open(fileobj=stream)

    def test_rejects_traversal_links_duplicate_and_wrong_prefix(self):
        cases = [
            [("donner-1.0.0/../outside", tarfile.REGTYPE)],
            [("/absolute", tarfile.REGTYPE)],
            [("other/MODULE.bazel", tarfile.REGTYPE)],
            [("donner-1.0.0/link", tarfile.SYMTYPE)],
            [("donner-1.0.0/link", tarfile.LNKTYPE)],
            [("donner-1.0.0/MODULE.bazel", tarfile.REGTYPE)] * 2,
            [("donner-1.0.0/MODULE.bazel", tarfile.REGTYPE),
             ("donner-1.0.0/./MODULE.bazel", tarfile.REGTYPE)],
        ]
        for entries in cases:
            with self.subTest(entries=entries), self.archive(entries) as archive:
                with self.assertRaises(ValueError):
                    source.archive_members(archive, "donner-1.0.0")

    def test_requires_module_and_limits_entry_count(self):
        with self.archive([("donner-1.0.0/file", tarfile.REGTYPE)]) as archive:
            with self.assertRaisesRegex(ValueError, "missing MODULE"):
                source.archive_members(archive, "donner-1.0.0")
        with self.archive([("donner-1.0.0/MODULE.bazel", tarfile.REGTYPE)]) as archive:
            with mock.patch.object(source, "MAX_FILES", 0):
                with self.assertRaisesRegex(ValueError, "excessive"):
                    source.archive_members(archive, "donner-1.0.0")

    def test_module_identity_and_version_are_literal_and_safe(self):
        self.assertEqual(source.module_values('module(name="donner", version="0.8.0-pre")')["version"],
                         "0.8.0-pre")
        for value in ['module(name="other", version="1.0.0")',
                      'module(name="donner", version="../../bad")', 'x = 1']:
            with self.subTest(value=value), self.assertRaises(ValueError):
                source.module_values(value)


class SourceFixtureTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.repo = self.base / "repo"
        self.repo.mkdir()
        self.previous = Path.cwd()
        os.chdir(self.repo)
        self.addCleanup(os.chdir, self.previous)
        env = mock.patch.dict(os.environ, {
            "GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1",
            "GITHUB_RUN_ID": "123", "GITHUB_RUN_ATTEMPT": "1",
        })
        env.start()
        self.addCleanup(env.stop)
        self.git("init", "-q")
        self.git("config", "user.name", "Test")
        self.git("config", "user.email", "test@example.invalid")
        self.write("MODULE.bazel", 'module(name="donner", version="1.0.0")\n')
        self.write(".bcr/source.template.json", json.dumps({
            "url": "https://github.com/jwmcglynn/donner/releases/download/{TAG}/donner-{VERSION}.tar.gz",
            "strip_prefix": "donner-{VERSION}", "integrity": "",
        }))
        self.write("examples/bazel_consumer/MODULE.bazel", 'bazel_dep(name="donner", version="1.0.0")\n')
        for name in ["BUILD.bazel", "main.cc", ".bazelrc"]:
            self.write(f"examples/bazel_consumer/{name}", "fixture\n")
        self.git("add", "--", "MODULE.bazel", ".bcr/source.template.json", "examples")
        self.git("-c", "commit.gpgsign=false", "commit", "-qm", "fixture")
        self.commit = self.git("rev-parse", "HEAD")
        self.artifacts = self.base / "artifacts"

    def git(self, *args):
        return subprocess.check_output(["git", *args], text=True).strip()

    def write(self, name, value):
        path = self.repo / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(value)

    def test_archive_round_trip_and_retry_qualification_preserve_bytes(self):
        receipt = source.create_archive(self.artifacts)
        path = self.artifacts / receipt["archive"]
        original = path.read_bytes()
        self.assertEqual(source.verify_archive(self.artifacts, self.commit)["sha256"], receipt["sha256"])
        source.qualify_archive(self.artifacts, self.commit, "123", "2")
        verified = source.verify_archive(self.artifacts, self.commit, "123", "2")
        self.assertEqual(verified["producer_run_attempt"], "1")
        self.assertEqual(path.read_bytes(), original)
        with self.assertRaisesRegex(ValueError, "different CI"):
            source.verify_archive(self.artifacts, self.commit, "123", "3")
        with self.assertRaisesRegex(ValueError, "does not belong"):
            source.qualify_archive(self.artifacts, self.commit, "999", "2")

    def test_tampered_archive_and_unexpected_files_are_rejected(self):
        receipt = source.create_archive(self.artifacts)
        path = self.artifacts / receipt["archive"]
        original = path.read_bytes()
        path.write_bytes(original + b"tamper")
        with self.assertRaisesRegex(ValueError, "digest mismatch"):
            source.verify_archive(self.artifacts, self.commit)
        path.write_bytes(original)
        (self.artifacts / "unexpected").write_text("other")
        with self.assertRaisesRegex(ValueError, "unexpected files"):
            source.verify_archive(self.artifacts, self.commit)

    def test_matching_digest_cannot_hide_changed_source_contents(self):
        receipt = source.create_archive(self.artifacts)
        path = self.artifacts / receipt["archive"]
        members = []
        with tarfile.open(path) as archive:
            for member in archive.getmembers():
                data = archive.extractfile(member).read() if member.isfile() else None
                if member.name.endswith("main.cc"):
                    data = b"changed source\n"
                    member.size = len(data)
                members.append((member, data))
        with tarfile.open(path, "w:gz") as archive:
            for member, data in members:
                archive.addfile(member, io.BytesIO(data) if data is not None else None)
        receipt["sha256"] = source.digest(path)
        (self.artifacts / "donner-1.0.0.provenance.json").write_text(json.dumps(receipt))
        (self.artifacts / f"{path.name}.sha256").write_text(f"{receipt['sha256']}  {path.name}\n")
        with self.assertRaisesRegex(ValueError, "do not match Git"):
            source.verify_archive(self.artifacts, self.commit)

    def test_dirty_source_output_in_checkout_and_unsafe_symlink_fail(self):
        with self.assertRaisesRegex(ValueError, "outside the checkout"):
            source.create_archive(self.repo / "artifacts")
        self.write("untracked.cc", "not committed")
        with self.assertRaisesRegex(ValueError, "must be clean"):
            source.create_archive(self.artifacts)
        (self.repo / "untracked.cc").unlink()
        (self.repo / "link").symlink_to("../secret")
        self.git("add", "--", "link")
        self.git("-c", "commit.gpgsign=false", "commit", "-qm", "unsafe link")
        with self.assertRaisesRegex(ValueError, "link or special"):
            source.create_archive(self.artifacts)

    def test_template_url_stale_consumer_and_late_override_fail(self):
        self.write(".bcr/source.template.json", json.dumps({
            "url": "https://github.com/jwmcglynn/donner/archive/{TAG}.tar.gz",
            "strip_prefix": "donner-{VERSION}",
        }))
        with self.assertRaisesRegex(ValueError, "stable release"):
            source.verify_templates("1.0.0")
        self.write(".bcr/source.template.json", json.dumps({
            "url": "https://github.com/jwmcglynn/donner/releases/download/{TAG}/donner-{VERSION}.tar.gz",
            "strip_prefix": "donner-{VERSION}",
        }))
        self.write("examples/bazel_consumer/MODULE.bazel", 'bazel_dep(name="donner", version="0.9.0")')
        with self.assertRaisesRegex(ValueError, "version does not match"):
            source.verify_templates("1.0.0")
        for override in ["local_path_override", "git_override", "archive_override", "single_version_override"]:
            self.write("examples/bazel_consumer/MODULE.bazel",
                       f'bazel_dep(name="donner", version="1.0.0")\n{override}(module_name="donner")')
            with self.subTest(override=override), self.assertRaisesRegex(ValueError, "must not override"):
                source.verify_templates("1.0.0")

    def test_registry_regeneration_preserves_other_versions_and_upstream(self):
        upstream = self.base / "upstream"
        (upstream / "modules/rules_cc").mkdir(parents=True)
        (upstream / "modules/donner/0.9.0").mkdir(parents=True)
        (upstream / "modules/donner/1.0.0").mkdir()
        (upstream / "bazel_registry.json").write_text("{}")
        metadata = {"versions": ["0.9.0", "1.0.0"], "yanked_versions": {"0.9.0": "old", "1.0.0": "candidate"}}
        original = json.dumps(metadata)
        (upstream / "modules/donner/metadata.json").write_text(original)
        output = self.base / "registry"
        source.prepare_registry(upstream, output, "1.0.0")
        result = json.loads((output / "modules/donner/metadata.json").read_text())
        self.assertEqual(result, {"versions": ["0.9.0"], "yanked_versions": {"0.9.0": "old"}})
        self.assertFalse((output / "modules/donner/1.0.0").exists())
        self.assertTrue((output / "modules/rules_cc").is_dir())
        self.assertEqual((upstream / "modules/donner/metadata.json").read_text(), original)

    def test_consumer_uses_archive_and_disposable_registry(self):
        receipt = source.create_archive(self.artifacts)
        registry = self.base / "registry"
        entry = registry / "modules/donner/1.0.0"
        entry.mkdir(parents=True)
        original = {"url": source.source_url("1.0.0"), "integrity": "unchanged"}
        (entry / "source.json").write_text(json.dumps(original))
        output = self.base / "consumer-test"
        source.prepare_consumer(self.artifacts, registry, output, self.commit)
        rewritten = json.loads((output / "registry/modules/donner/1.0.0/source.json").read_text())
        self.assertEqual(rewritten["url"], (self.artifacts / receipt["archive"]).resolve().as_uri())
        self.assertEqual((output / "consumer/main.cc").read_text(), "fixture\n")
        self.assertEqual(json.loads((entry / "source.json").read_text()), original)


if __name__ == "__main__":
    unittest.main()
