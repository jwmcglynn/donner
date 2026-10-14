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

from python.runfiles import runfiles

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

    def test_requires_root_license_and_notice(self):
        for present in ("LICENSE", "NOTICE"):
            entries = [("donner-1.0.0/MODULE.bazel", tarfile.REGTYPE),
                       (f"donner-1.0.0/{present}", tarfile.REGTYPE)]
            with self.subTest(present=present), self.archive(entries) as archive:
                with self.assertRaisesRegex(ValueError, "missing LICENSE or NOTICE"):
                    source.archive_members(archive, "donner-1.0.0")

    def test_rejects_the_generated_coverage_report(self):
        entries = [("donner-1.0.0/MODULE.bazel", tarfile.REGTYPE),
                   ("donner-1.0.0/LICENSE", tarfile.REGTYPE),
                   ("donner-1.0.0/NOTICE", tarfile.REGTYPE),
                   ("donner-1.0.0/docs/reports/coverage.zip", tarfile.REGTYPE)]
        with self.archive(entries) as archive:
            with self.assertRaisesRegex(ValueError, "generated report"):
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
        self.write("LICENSE", "fixture license\n")
        self.write("NOTICE", "fixture notice\n")
        self.write(".bcr/source.template.json", json.dumps({
            "url": "https://github.com/jwmcglynn/donner/releases/download/{TAG}/donner-{VERSION}.tar.gz",
            "strip_prefix": "donner-{VERSION}", "integrity": "",
        }))
        self.write("examples/bazel_consumer/MODULE.bazel", 'bazel_dep(name="donner", version="1.0.0")\n')
        for name in ["BUILD.bazel", "main.cc", ".bazelrc"]:
            self.write(f"examples/bazel_consumer/{name}", "fixture\n")
        self.git("add", "--", "MODULE.bazel", "LICENSE", "NOTICE",
                 ".bcr/source.template.json", "examples")
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

    def test_repository_attributes_omit_only_the_coverage_report(self):
        attributes = Path(runfiles.Create().Rlocation("donner/.gitattributes")).read_text()
        self.write(".gitattributes", attributes)
        kept = ["docs/reports/binary-size/report.txt", "docs/img/demo.gif",
                "third_party/resvg-test-suite/tests/shape.svg"]
        for name in kept:
            self.write(name, "kept\n")
        (self.repo / "docs/reports").mkdir(parents=True, exist_ok=True)
        (self.repo / "docs/reports/coverage.zip").write_bytes(b"PK\x05\x06" + bytes(18))
        self.git("add", "--", ".gitattributes", "docs", "third_party")
        self.git("-c", "commit.gpgsign=false", "commit", "-qm", "reports")
        commit = self.git("rev-parse", "HEAD")
        receipt = source.create_archive(self.artifacts)
        with tarfile.open(self.artifacts / receipt["archive"]) as archive:
            names = {name.split("/", 1)[1] for name in archive.getnames() if "/" in name}
        self.assertNotIn("docs/reports/coverage.zip", names)
        for name in ["LICENSE", "NOTICE", "MODULE.bazel", *kept]:
            self.assertIn(name, names)
        self.assertEqual(source.verify_archive(self.artifacts, commit)["files"], receipt["files"])

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


class ValidatedEntryTest(unittest.TestCase):
    """The entry BCR Preflight generated and admission-validated, as retained for submission."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        base = Path(self.temp.name)
        self.root = base / "source"
        (self.root / ".bcr").mkdir(parents=True)
        (self.root / "MODULE.bazel").write_text('module(name="donner", version="1.0.0")\n')
        (self.root / ".bcr/presubmit.yml").write_text("tasks: {}\n")
        self.template = {"homepage": "https://github.com/jwmcglynn/donner",
                         "maintainers": [{"github": "jwmcglynn"}],
                         "repository": ["github:jwmcglynn/donner"],
                         "versions": [], "yanked_versions": {}}
        (self.root / ".bcr/metadata.template.json").write_text(json.dumps(self.template))
        self.entry = base / "entry"
        module, presubmit, source_json, metadata = (self.entry / "registry" / path
                                                    for path in source.entry_paths("1.0.0"))
        module.parent.mkdir(parents=True)
        module.write_bytes((self.root / "MODULE.bazel").read_bytes())
        presubmit.write_bytes((self.root / ".bcr/presubmit.yml").read_bytes())
        source_json.write_text(json.dumps(source.expected_source_json("1.0.0", "c" * 64),
                                          indent=4))
        self.metadata = metadata
        self.write_metadata(dict(self.template, versions=["0.9.0", "1.0.0"]))
        self.report = {"validator_revision": "f" * 40, "module": "donner@1.0.0",
                       "upstream_exit_code": 42,
                       "results": [{"result": "GOOD", "message": "ok"},
                                   {"result": "NEED_BCR_MAINTAINER_REVIEW", "message": "new"}]}
        self.write_report(self.report)

    def write_metadata(self, value):
        self.metadata.write_text(json.dumps(value, indent=4))

    def write_report(self, value):
        (self.entry / "admission.json").write_text(json.dumps(value))

    def verify(self, sha256="c" * 64):
        source.verify_validated_entry(self.entry, self.root, "1.0.0", sha256)

    def test_retained_preflight_entry_is_accepted(self):
        self.assertEqual([path.as_posix() for path in source.entry_paths("1.0.0")], [
            "modules/donner/1.0.0/MODULE.bazel", "modules/donner/1.0.0/presubmit.yml",
            "modules/donner/1.0.0/source.json", "modules/donner/metadata.json"])
        self.verify()
        self.write_report(dict(self.report, upstream_exit_code=0))
        self.verify()

    def test_failed_or_foreign_admission_report_is_rejected(self):
        cases = {
            "failed": {"upstream_exit_code": 1},
            "boolean code": {"upstream_exit_code": False},
            "other version": {"module": "donner@0.9.0"},
            "other module": {"module": "other@1.0.0"},
            "short revision": {"validator_revision": "f" * 12},
            "no results": {"results": []},
            "failed result": {"results": [{"result": "FAILED", "message": "bad"}]},
        }
        for case, change in cases.items():
            with self.subTest(case=case):
                self.write_report(dict(self.report, **change))
                with self.assertRaisesRegex(ValueError, "admission report"):
                    self.verify()
        (self.entry / "admission.json").write_text("passed\n")
        with self.assertRaisesRegex(ValueError, "admission report is not JSON"):
            self.verify()

    def test_entry_must_name_the_archive_and_carry_the_source_files(self):
        with self.assertRaisesRegex(ValueError, "approved source archive"):
            self.verify("d" * 64)
        module = self.entry / "registry" / source.entry_paths("1.0.0")[0]
        module.write_text("module(name='donner')\n")
        with self.assertRaisesRegex(ValueError, "approved module files"):
            self.verify()
        module.unlink()
        module.symlink_to(self.root / "MODULE.bazel")
        with self.assertRaisesRegex(ValueError, "missing, linked or too large"):
            self.verify()

    def test_entry_metadata_must_match_the_maintainer_template(self):
        for change in ({"maintainers": []}, {"versions": ["0.9.0"]},
                       {"yanked_versions": {"1.0.0": "withdrawn"}}):
            with self.subTest(change=change):
                self.write_metadata({**self.template, "versions": ["0.9.0", "1.0.0"], **change})
                with self.assertRaisesRegex(ValueError, "registry metadata"):
                    self.verify()


class RegistryVersionOrderTest(unittest.TestCase):
    """BCR presubmit requires `metadata.json` to list versions in ascending Bazel order."""

    TEMPLATE = {"homepage": "https://github.com/jwmcglynn/donner",
                "maintainers": [{"github": "jwmcglynn"}],
                "repository": ["github:jwmcglynn/donner"],
                "versions": [], "yanked_versions": {}}

    def test_derived_metadata_inserts_the_version_in_order(self):
        upstream = {**self.TEMPLATE, "versions": ["0.8.0", "0.9.0", "0.10.0"]}
        for version, expected in (
                ("0.8.1", ["0.8.0", "0.8.1", "0.9.0", "0.10.0"]),
                ("0.9.1", ["0.8.0", "0.9.0", "0.9.1", "0.10.0"]),
                ("0.11.0", ["0.8.0", "0.9.0", "0.10.0", "0.11.0"])):
            with self.subTest(version=version):
                derived = json.loads(source.derive_metadata(upstream, self.TEMPLATE, version))
                self.assertEqual(derived["versions"], expected)

    def test_key_follows_bazel_version_comparison(self):
        ordered = ["0.8.0", "0.8.1", "0.9.0", "0.10.0", "1.0.0-rc.2", "1.0.0-rc.10", "1.0.0-rc1",
                   "1.0.0", "1.0.0.1", "1.0.0.bcr.1", "1.0.1", "v1.0.0"]
        self.assertEqual(sorted(reversed(ordered), key=source.registry_version_key), ordered)
        self.assertEqual(source.registry_version_key("1.0.0+build.5"),
                         source.registry_version_key("1.0.0"))
        for version in ("", "1..0", ".1", "1.0.0-", "1.0.0-rc..1", "1.0 0", "1.0.0\n"):
            with self.subTest(version=version), \
                    self.assertRaisesRegex(ValueError, "invalid version list"):
                source.registry_version_key(version)


if __name__ == "__main__":
    unittest.main()
