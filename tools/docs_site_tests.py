"""Source identity, composition integrity and legacy-link regression coverage."""
import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest import mock

from tools import docs_site as site


class DocsSiteTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def source(self, name="release", version="0.8.0", extra=None):
        identity = {"version": version, "source_commit": "a" * 40, "run_id": 123, "attempt": 1}
        header = '<html><head><title>API</title></head><body><div id="top"><span id="projectnumber">&#160;' + version + '</span></div>'
        files = {"index.html": (header + '<h1 id="intro">Introduction</h1></body></html>').encode(),
                 "api/type.html": (header + '<h1 id="member">Type</h1></body></html>').encode(),
                 "search/data.js": b'const searchData = "' + name.encode() + b'";',
                 "reports/report.html": b'<html><head></head><body>Report</body></html>',
                 "docs-build.json": site.canonical({"source_commit": identity["source_commit"],
                                                    "source_run_id": 123, "source_attempt": 1})}
        files.update(extra or {})
        path = self.root / (name + ".tar")
        with tarfile.open(path, "w:") as tar:
            for key, data in files.items():
                info = tarfile.TarInfo(key); info.size = len(data)
                tar.addfile(info, io.BytesIO(data))
        identity["archive_sha256"] = site.bcr_source.digest(path)
        return path, identity

    def compose(self):
        sources = {"0.8.0": self.source(), "main": self.source("development", "0.9.0-pre")}
        output = self.root / "site"
        marker = site.compose(sources, "0.8.0", output)
        return output, marker

    def test_two_versions_have_isolated_search_and_stable_legacy_links(self):
        output, marker = self.compose()
        self.assertEqual(site.verify_site(output), marker)
        self.assertEqual((output / "search/data.js").read_bytes(), (output / "0.8.0/search/data.js").read_bytes())
        self.assertNotEqual((output / "main/search/data.js").read_bytes(), (output / "0.8.0/search/data.js").read_bytes())
        self.assertIn('/donner/0.8.0/api/type.html', (output / "api/type.html").read_text())
        self.assertEqual(site.Page((output / "main/index.html").read_text()).versions, ["main"])
        self.assertEqual(site.Page((output / "0.8.0/index.html").read_text()).versions, ["0.8.0"])
        self.assertIn('Documentation version', (output / "api/type.html").read_text())

    def test_absolute_report_links_stay_with_the_selected_version(self):
        text = '<a href="https://jwmcglynn.github.io/donner/reports/coverage/index.html">Coverage</a>'
        text += '<a href="/donner/0.8.0/index.html">Explicit release</a>'
        changed = site.version_links(text, "main", ["0.8.0", "main"], "/donner/")
        self.assertIn('href="/donner/main/reports/coverage/index.html"', changed)
        self.assertIn('href="/donner/0.8.0/index.html"', changed)

    def test_hostile_filename_cannot_inject_markup_into_root_canonical_link(self):
        name = 'bad"><script>alert(1)</script>.html'
        page = b'<html><head></head><body><span id="projectnumber">0.8.0</span></body></html>'
        sources = {"0.8.0": self.source(extra={name: page}), "main": self.source("main")}
        output = self.root / "site"
        site.compose(sources, "0.8.0", output)
        text = (output / name).read_text()
        self.assertNotIn('<script>alert(1)</script>', text)
        self.assertIn('%22%3E%3Cscript%3E', text)
        self.assertEqual(site.Page(text).versions, ["0.8.0"])

    def test_page_and_anchor_inventory_describes_real_nested_pages(self):
        output, _ = self.compose()
        pages = json.loads((output / "0.8.0/_docs/pages.json").read_bytes())["pages"]
        entry = pages["api/type.html"]
        raw = (output / "0.8.0" / entry["anchors"]).read_bytes()
        self.assertEqual(site.digest(raw), entry["sha256"])
        self.assertIn("member", json.loads(raw)["anchors"])
        self.assertNotIn("missing.html", pages)

    def test_every_source_and_view_has_an_integrity_binding(self):
        output, _ = self.compose()
        manifest = json.loads((output / "_docs/versions.json").read_bytes())
        self.assertEqual(set(manifest["original_content"]), {"0.8.0", "main"})
        self.assertEqual(manifest["versions"]["main"]["identity"]["version"], "0.9.0-pre")
        (output / "main/search/data.js").write_text("changed")
        with self.assertRaisesRegex(ValueError, "files changed"):
            site.verify_site(output)

    def test_deterministic_packing_and_archive_roundtrip(self):
        output, _ = self.compose()
        first, second = self.root / "one.tar", self.root / "two.tar"
        self.assertEqual(site.pack(output, first), site.pack(output, second))
        site.unpack(first, self.root / "restored")
        self.assertEqual(site.verify_site(self.root / "restored"), site.verify_site(output))

    def test_version_validation_rejects_blank_or_stale_output(self):
        for label in ("", "0.8.0-pre", "0.9.0"):
            with self.subTest(label=label):
                folder = self.root / (label or "blank"); folder.mkdir()
                (folder / "index.html").write_text('<span id="projectnumber">' + label + '</span>')
                with self.assertRaisesRegex(ValueError, "version differs"):
                    site.check_version(folder, "0.8.0")

    def test_source_identity_and_optional_version_record_are_bound(self):
        archive, identity = self.source(extra={"docs-version.json": site.canonical({"schema": 1,
            "semantic_version": "0.8.0-pre", "source_commit": "a" * 40})})
        with self.assertRaisesRegex(ValueError, "recorded docs version differs"):
            site.read_source(archive, identity, self.root / "wrong")

    def test_legacy_marker_is_accepted_but_wrong_source_is_rejected(self):
        archive, identity = self.source(); identity["source_commit"] = "b" * 40
        with self.assertRaisesRegex(ValueError, "marker does not match"):
            site.read_source(archive, identity, self.root / "wrong")

    def test_missing_stable_main_and_mislabeled_release_are_rejected(self):
        archive, identity = self.source()
        for sources in ({"0.8.0": (archive, identity)}, {"main": (archive, identity)},
                        {"0.8.0": (archive, {**identity, "version": "0.9.0"}), "main": (archive, identity)}):
            with self.subTest(keys=list(sources)), self.assertRaises(ValueError):
                site.compose(sources, "0.8.0", self.root / "bad")
            if (self.root / "bad").exists():
                import shutil
                shutil.rmtree(self.root / "bad")

    def test_archive_rejects_traversal_links_and_duplicate_files(self):
        for kind in ("traversal", "link", "duplicate"):
            path = self.root / (kind + ".tar")
            with tarfile.open(path, "w:") as tar:
                member = tarfile.TarInfo("../escape" if kind == "traversal" else "index.html")
                if kind == "link": member.type = tarfile.SYMTYPE; member.linkname = "/tmp/file"
                tar.addfile(member)
                if kind == "duplicate": tar.addfile(member)
            with self.subTest(kind=kind), self.assertRaisesRegex(ValueError, "unsafe|duplicate"):
                site.unpack(path, self.root / kind)

    def test_reserved_routes_and_output_reuse_are_rejected(self):
        archive, identity = self.source(extra={"main/index.html": b"nested"})
        with self.assertRaisesRegex(ValueError, "composition paths"):
            site.read_source(archive, identity, self.root / "nested")
        with self.assertRaisesRegex(ValueError, "already exist"):
            site.empty(self.root)

    def test_file_and_total_size_limits_are_enforced(self):
        archive, _ = self.source()
        with mock.patch.object(site, "MAX_FILE", 2), self.assertRaisesRegex(ValueError, "size limit"):
            site.unpack(archive, self.root / "limited")

    def test_navigation_inputs_cannot_escape_the_site(self):
        for path in ("../x", "/x", "a//b", "a/./b", "%2e%2e/x", "x?next=y", "x\\y", "x#y"):
            with self.subTest(path=path), self.assertRaises(ValueError): site.safe_path(path)
        self.assertEqual(site.safe_path("api/type.html"), "api/type.html")


if __name__ == "__main__": unittest.main()
