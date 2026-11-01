"""Tests for complete license text sharing in aggregated notices."""

from __future__ import annotations

import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from python.runfiles import runfiles

from tools import render_notice


class RenderNoticeTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.manifest = self.root / "manifest.json"
        self.output = self.root / "NOTICE.txt"

    def license_file(self, name: str, contents: str) -> Path:
        path = self.root / name
        path.write_text(contents)
        return path

    @staticmethod
    def entry(name: str, path: Path, **metadata) -> dict:
        return {"label": "//licenses:" + name.lower(), "package_name": name,
                "license_text": str(path), **metadata}

    def render(self, entries: list[dict], lookup: dict[str, Path] | None = None) -> str:
        self.manifest.write_text(json.dumps({"variant": "fixture", "licenses": entries}))
        render_notice.render(self.manifest, self.output, lookup or {})
        return self.output.read_text()

    def test_identical_bodies_share_text_and_preserve_every_packages_metadata(self) -> None:
        body = "Permission to use the complete fixture.\nRetain this text."
        alpha = self.license_file("alpha.txt", body + "\n")
        zulu = self.license_file("zulu.txt", body + "\n")
        entries = [
            self.entry("Zulu", zulu, package_version="2.0", package_url="https://example.com/zulu",
                       license_kinds=["Apache-2.0", "MIT"],
                       copyright_notice="2026 Zulu Authors"),
            self.entry("Alpha", alpha, package_version="1.0",
                       package_url="https://example.com/alpha",
                       license_kinds=["MIT"], copyright_notice="2025 Alpha Authors"),
        ]

        result = self.render(entries)

        for name, version, url, kinds, copyright_notice in (
            ("Alpha", "1.0", "https://example.com/alpha", "MIT", "2025 Alpha Authors"),
            ("Zulu", "2.0", "https://example.com/zulu", "Apache-2.0, MIT", "2026 Zulu Authors"),
        ):
            self.assertIn(
                f"Package: {name}\nVersion: {version}\nURL: {url}\nLicense: {kinds}\n"
                f"Copyright: {copyright_notice}\n", result,
            )
        self.assertEqual(result.count(body), 1)
        self.assertIn("License text [1]:\n" + body, result)
        self.assertIn("License text: see [1] above.", result)
        self.assertLess(result.index(body), result.index("Package: Zulu\n"))

    def test_same_spdx_with_distinct_copyright_text_is_not_shared(self) -> None:
        alpha_body = "Copyright Alpha\nPermission to use."
        zulu_body = "Copyright Zulu\nPermission to use."
        result = self.render([
            self.entry("Alpha", self.license_file("a.txt", alpha_body), license_kinds=["MIT"]),
            self.entry("Zulu", self.license_file("z.txt", zulu_body), license_kinds=["MIT"]),
        ])

        self.assertIn(alpha_body, result)
        self.assertIn(zulu_body, result)
        self.assertNotIn("License text: see [", result)

    def test_shared_prefix_with_a_distinct_appended_notice_is_not_shared(self) -> None:
        common = "Complete license text.\n"
        first = common + "NOTICE: Alpha attribution."
        second = common + "NOTICE: Zulu attribution."
        result = self.render([
            self.entry("Alpha", self.license_file("a.txt", first), license_kinds=["Apache-2.0"]),
            self.entry("Zulu", self.license_file("z.txt", second), license_kinds=["Apache-2.0"]),
        ])

        self.assertIn(first, result)
        self.assertIn(second, result)
        self.assertNotIn("License text: see [", result)

    def test_sorted_first_package_retains_the_body_independent_of_manifest_order(self) -> None:
        body = "Deterministic complete license."
        alpha = self.entry("Alpha", self.license_file("a.txt", body))
        zulu = self.entry("Zulu", self.license_file("z.txt", body))

        first = self.render([zulu, alpha])
        second = self.render([alpha, zulu])

        self.assertEqual(first, second)
        self.assertEqual(first.count(body), 1)
        self.assertIn("License text [1]:\n" + body, first)
        self.assertIn("License text: see [1] above.", first)
        self.assertLess(first.index(body), first.index("License text: see ["))

    def test_numeric_references_disambiguate_colliding_names_and_versions(self) -> None:
        first_body = "Complete license A."
        second_body = "Complete license B."
        entries = [
            self.entry("Shared", self.license_file("a.txt", first_body),
                       label="//licenses:first", package_version="1.0"),
            self.entry("Shared", self.license_file("b.txt", second_body),
                       label="//licenses:second", package_version="1.0"),
            self.entry("Shared", self.license_file("c.txt", first_body),
                       label="//licenses:third", package_version="2.0"),
            self.entry("Shared", self.license_file("d.txt", second_body),
                       label="//licenses:fourth", package_version="2.0"),
        ]

        result = self.render(entries)

        sections = result.split("Package: Shared\n")[1:]
        self.assertEqual(len(sections), 4)
        self.assertIn("Version: 1.0\n", sections[0])
        self.assertIn("License text [1]:\n" + first_body, sections[0])
        self.assertIn("Version: 1.0\n", sections[1])
        self.assertIn("License text [2]:\n" + second_body, sections[1])
        self.assertIn("Version: 2.0\n", sections[2])
        self.assertIn("License text: see [1] above.", sections[2])
        self.assertIn("Version: 2.0\n", sections[3])
        self.assertIn("License text: see [2] above.", sections[3])
        self.assertEqual(result.count(first_body), 1)
        self.assertEqual(result.count(second_body), 1)

    def test_existing_newline_and_trailing_whitespace_processing_defines_equality(self) -> None:
        body = "Complete license.\nSecond line."
        first = self.root / "a.txt"
        first.write_bytes((body.replace("\n", "\r\n") + "\r\n").encode())
        second = self.license_file("z.txt", body + "\n \t\n")

        result = self.render([self.entry("Alpha", first), self.entry("Zulu", second)])

        self.assertEqual(result.count(body), 1)
        self.assertIn("License text [1]:\n" + body, result)
        self.assertIn("License text: see [1] above.", result)

    def test_different_internal_whitespace_is_not_normalized_for_sharing(self) -> None:
        first = "Complete license: retain  two spaces."
        second = "Complete license: retain two spaces."
        result = self.render([
            self.entry("Alpha", self.license_file("a.txt", first)),
            self.entry("Zulu", self.license_file("z.txt", second)),
        ])

        self.assertIn(first, result)
        self.assertIn(second, result)
        self.assertNotIn("License text: see [", result)

    def test_label_fallback_names_the_first_complete_license(self) -> None:
        body = "Complete fallback license."
        first = {"label": "//licenses:alpha", "license_text": "alpha-key"}
        second = {"label": "//licenses:zulu", "license_text": "zulu-key"}
        lookup = {"alpha-key": self.license_file("a.txt", body),
                  "zulu-key": self.license_file("z.txt", body)}

        result = self.render([second, first], lookup)

        self.assertIn("Package: //licenses:alpha\n", result)
        self.assertIn("Package: //licenses:zulu\n", result)
        self.assertEqual(result.count(body), 1)
        self.assertIn("License text [1]:\n" + body, result)
        self.assertIn("License text: see [1] above.", result)

    def test_missing_source_is_not_bypassed_for_a_matching_spdx(self) -> None:
        entries = [
            self.entry("Alpha", self.license_file("a.txt", "Complete license."),
                       license_kinds=["MIT"]),
            self.entry("Zulu", self.root / "missing.txt", license_kinds=["MIT"]),
        ]
        self.output.write_text("previous output")

        with self.assertRaisesRegex(SystemExit, "failed to read license text"):
            self.render(entries)

        self.assertEqual(self.output.read_text(), "previous output")

    def test_each_entry_reads_its_source_even_when_the_path_is_repeated(self) -> None:
        shared = self.license_file("shared.txt", "Complete shared license.")
        original_read = Path.read_text
        reads = 0

        def read_text(path, *args, **kwargs):
            nonlocal reads
            if path == shared:
                reads += 1
                if reads == 2:
                    raise PermissionError("fixture source became unreadable")
            return original_read(path, *args, **kwargs)

        self.output.write_text("previous output")
        with mock.patch.object(Path, "read_text", autospec=True, side_effect=read_text):
            with self.assertRaisesRegex(SystemExit, "failed to read license text"):
                self.render([self.entry("Alpha", shared), self.entry("Zulu", shared)])

        self.assertEqual(reads, 2)
        self.assertEqual(self.output.read_text(), "previous output")

    def test_sheenbidi_and_roboto_share_their_complete_retained_license(self) -> None:
        resolver = runfiles.Create()
        self.assertIsNotNone(resolver)
        sheen_path = resolver.Rlocation("donner/third_party/licenses/sheenbidi.LICENSE.txt")
        roboto_path = resolver.Rlocation("donner/third_party/licenses/roboto.LICENSE.txt")
        self.assertTrue(sheen_path and roboto_path)
        sheen = Path(sheen_path)
        roboto = Path(roboto_path)
        body = sheen.read_text().rstrip()
        self.assertEqual(body, roboto.read_text().rstrip())
        self.assertEqual(len(body.encode()), 11357)

        result = self.render([
            self.entry("SheenBidi", sheen, package_version="3.0.0",
                       package_url="https://github.com/Tehreer/SheenBidi",
                       license_kinds=["Apache-2.0"]),
            self.entry("Roboto", roboto, package_url="https://fonts.google.com/specimen/Roboto",
                       license_kinds=["Apache-2.0"]),
        ])

        self.assertEqual(result.count(body), 1)
        self.assertIn("License text [1]:\n" + body, result)
        self.assertIn("Package: SheenBidi\nVersion: 3.0.0\n", result)
        self.assertIn("Package: Roboto\n", result)
        self.assertIn("License text: see [1] above.", result)


if __name__ == "__main__":
    unittest.main()
