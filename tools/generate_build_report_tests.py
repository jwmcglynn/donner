import io
import importlib.util
import json
import tempfile
from pathlib import Path
import sys
import unittest


MODULE_PATH = Path(__file__).with_name("generate_build_report.py")
MODULE_SPEC = importlib.util.spec_from_file_location("generate_build_report", MODULE_PATH)
assert MODULE_SPEC is not None
assert MODULE_SPEC.loader is not None
generate_build_report = importlib.util.module_from_spec(MODULE_SPEC)
sys.modules[MODULE_SPEC.name] = generate_build_report
MODULE_SPEC.loader.exec_module(generate_build_report)


class FakeRunner:
    def __init__(self, results):
        self.results = results
        self.calls = []

    def run(self, label, args):
        key = tuple(args)
        self.calls.append((label, key))
        return self.results[key]


class GenerateBuildReportTests(unittest.TestCase):
    def test_normalize_external_dependency_repo(self):
        self.assertEqual(
            generate_build_report._normalize_external_dependency_repo(
                "@+_repo_rules+tiny-skia-cpp//src:tiny_skia_lib_native"
            ),
            "tiny-skia-cpp",
        )
        self.assertEqual(
            generate_build_report._normalize_external_dependency_repo(
                "@non_bcr_deps+harfbuzz//:harfbuzz"
            ),
            "harfbuzz",
        )
        self.assertIsNone(
            generate_build_report._normalize_external_dependency_repo(
                "@bazel_tools//tools:host_platform"
            )
        )
        self.assertIsNone(
            generate_build_report._normalize_external_dependency_repo(
                "@@rules_cc++cc_configure_extension+local_config_cc//:local"
            )
        )

    def _notice_build_result(self, label: str) -> "generate_build_report.CommandResult":
        # The external-dependencies section also runs `bazel build
        # //third_party/licenses:notice_*` to materialize each variant's
        # license manifest. Tests don't have a live Bazel workspace, so we
        # mock these calls as successful: the subsequent manifest file read
        # will fail gracefully (empty lookup) because the JSON doesn't exist.
        return generate_build_report.CommandResult(
            label=label,
            args=(),
            returncode=0,
            stdout="",
            stderr="",
            duration_sec=0.1,
        )

    def test_external_dependencies_section_uses_expected_categories(self):
        results = {
            ("bazel", "build", "//third_party/licenses:notice_default"):
                self._notice_build_result("notice-default"),
            ("bazel", "build", "//third_party/licenses:notice_text_full"):
                self._notice_build_result("notice-text-full"),
            ("bazel", "build", "//third_party/licenses:notice_skia_text_full"):
                self._notice_build_result("notice-skia-text-full"),
            ("bazel", "build", "//third_party/licenses:notice_editor"):
                self._notice_build_result("notice-editor"),
            (
                "bazel",
                "cquery",
                "deps(//examples:svg_to_png)",
                "--output=starlark",
                "--starlark:expr=target.label",
            ): generate_build_report.CommandResult(
                label="default",
                args=(),
                returncode=0,
                stdout="\n".join(
                    [
                        "@+_repo_rules+entt//:entt",
                        "@+_repo_rules2+stb//:image_write",
                        "@+_repo_rules+tiny-skia-cpp//src:tiny_skia",
                    ]
                ),
                stderr="",
                duration_sec=1.0,
            ),
            (
                "bazel",
                "cquery",
                "deps(//examples:svg_to_png)",
                "--output=starlark",
                "--starlark:expr=target.label",
                "--config=text-full",
            ): generate_build_report.CommandResult(
                label="text-full",
                args=(),
                returncode=0,
                stdout="\n".join(
                    [
                        "@+_repo_rules+entt//:entt",
                        "@non_bcr_deps+harfbuzz//:harfbuzz",
                        "@non_bcr_deps+woff2//:woff2",
                    ]
                ),
                stderr="",
                duration_sec=2.0,
            ),
            (
                "bazel",
                "cquery",
                "deps(//examples:svg_to_png)",
                "--output=starlark",
                "--starlark:expr=target.label",
                "--config=skia",
                "--config=text-full",
            ): generate_build_report.CommandResult(
                label="skia-text-full",
                args=(),
                returncode=0,
                stdout="\n".join(
                    [
                        "@+_repo_rules+entt//:entt",
                        "@non_bcr_deps+skia//:skia",
                        "@non_bcr_deps+harfbuzz//:harfbuzz",
                    ]
                ),
                stderr="",
                duration_sec=3.0,
            ),
        }
        section = generate_build_report.make_external_dependencies_section(
            FakeRunner(results),
            bazel_bin=Path("/nonexistent-bazel-bin"),
        )

        self.assertEqual(section.status, "success")
        self.assertIn("### Default (tiny-skia)", section.content)
        self.assertIn(
            "Generated with: `bazel cquery 'deps(//examples:svg_to_png)'`",
            section.content,
        )
        self.assertIn("- tiny-skia-cpp", section.content)
        self.assertIn("### tiny-skia + text-full", section.content)
        self.assertIn("- woff2", section.content)
        self.assertIn("### skia + text-full", section.content)
        self.assertIn("- skia", section.content)
        self.assertIn("### editor (skia + text-full + imgui/glfw/tracy)", section.content)

    def test_external_dependencies_section_annotates_with_licenses(self):
        """When a license manifest is available, each dep is annotated with
        its SPDX kind and a link to the upstream license URL."""
        with tempfile.TemporaryDirectory() as tmp:
            bazel_bin = Path(tmp)
            manifest_dir = bazel_bin / "third_party/licenses"
            manifest_dir.mkdir(parents=True)
            manifest_payload = {
                "variant": "Default (tiny-skia)",
                "licenses": [
                    {
                        "package_name": "EnTT",
                        "license_kinds": ["MIT"],
                        "package_url": "https://github.com/skypjack/entt",
                        "label": "@@//third_party/licenses:entt",
                        "license_text": "../+_repo_rules+entt/LICENSE",
                    },
                ],
            }
            (manifest_dir / "notice_default.json").write_text(
                json.dumps(manifest_payload)
            )
            # Provide only the default-variant manifest; the other variants
            # will legitimately return empty lookups because their JSON files
            # don't exist, which is fine for this test.
            results = {
                ("bazel", "build", "//third_party/licenses:notice_default"):
                    self._notice_build_result("notice-default"),
                ("bazel", "build", "//third_party/licenses:notice_text_full"):
                    self._notice_build_result("notice-text-full"),
                ("bazel", "build", "//third_party/licenses:notice_skia_text_full"):
                    self._notice_build_result("notice-skia-text-full"),
                ("bazel", "build", "//third_party/licenses:notice_editor"):
                    self._notice_build_result("notice-editor"),
            }
            for configs in ([], ["--config=text-full"],
                            ["--config=skia", "--config=text-full"]):
                key = (
                    "bazel",
                    "cquery",
                    "deps(//examples:svg_to_png)",
                    "--output=starlark",
                    "--starlark:expr=target.label",
                    *configs,
                )
                results[key] = generate_build_report.CommandResult(
                    label="cquery",
                    args=(),
                    returncode=0,
                    stdout="@+_repo_rules+entt//:entt",
                    stderr="",
                    duration_sec=0.1,
                )

            section = generate_build_report.make_external_dependencies_section(
                FakeRunner(results),
                bazel_bin=bazel_bin,
            )
            self.assertIn(
                "- [entt](https://github.com/skypjack/entt) — MIT",
                section.content,
            )

    def test_create_build_report_renders_summary_and_dirty_status(self):
        runner = FakeRunner(
            {
                ("tools/cloc.sh",): generate_build_report.CommandResult(
                    label="lines-of-code",
                    args=("tools/cloc.sh",),
                    returncode=0,
                    stdout="Lines of source code: 1.0k",
                    stderr="",
                    duration_sec=0.5,
                ),
                (
                    "bazel",
                    "query",
                    "kind(library, set(//donner/... //:*)) intersect attr(visibility, public, //...)",
                ): generate_build_report.CommandResult(
                    label="public-targets",
                    args=(),
                    returncode=0,
                    stdout="//:donner",
                    stderr="",
                    duration_sec=1.25,
                ),
                ("bazel", "test", "//donner/..."): generate_build_report.CommandResult(
                    label="tests",
                    args=("bazel", "test", "//donner/..."),
                    returncode=0,
                    stdout="//donner/... tests passed",
                    stderr="",
                    duration_sec=9.0,
                ),
            }
        )
        metadata = generate_build_report.ReportMetadata(
            command_line="tools/generate_build_report.py --public-targets",
            platform="Linux x86_64",
            git_revision="deadbeef",
            git_status=" M docs/build_report.md\n M tools/generate_build_report.py",
        )

        report = generate_build_report.create_build_report(
            generate_build_report.ReportOptions(public_targets=True, tests=True),
            runner=runner,
            metadata=metadata,
            command_line=metadata.command_line,
        )

        self.assertIn("## Summary", report)
        self.assertIn("- Working tree: dirty (2 paths)", report)
        self.assertIn("- Lines of Code: success (0.5s)", report)
        self.assertIn("- Tests: success (9.0s)", report)
        self.assertIn("- Public Targets: success (1.2s)", report)
        self.assertIn("## Local Changes", report)
        self.assertIn("## Tests", report)
        self.assertIn("## Public Targets", report)
        self.assertIn("//:donner", report)

    def test_binary_size_section_preserves_script_markdown(self):
        runner = FakeRunner(
            {
                ("tools/binary_size.sh",): generate_build_report.CommandResult(
                    label="binary-size",
                    args=("tools/binary_size.sh",),
                    returncode=0,
                    stdout="```\nTotal binary size of foo\n1.0M\tfoo\n```",
                    stderr="",
                    duration_sec=2.0,
                )
            }
        )
        section = generate_build_report.make_binary_size_section(
            runner,
            None,
            generate_build_report._resolve_link_targets(generate_build_report.LINK_MODE_LOCAL),
        )

        self.assertEqual(section.status, "success")
        self.assertIn("Generated with: `tools/binary_size.sh`", section.content)
        self.assertIn("Total binary size of foo", section.content)
        self.assertNotIn("$ tools/binary_size.sh", section.content)

    def test_resolve_link_targets_modes(self):
        local = generate_build_report._resolve_link_targets(
            generate_build_report.LINK_MODE_LOCAL
        )
        self.assertEqual(local.coverage_index, "coverage-report/index.html")
        self.assertEqual(
            local.binary_size_report,
            "build-binary-size/binary_size_report.html",
        )

        docs = generate_build_report._resolve_link_targets(
            generate_build_report.LINK_MODE_DOCS
        )
        # Coverage is shipped as a zip and unpacked by tools/build_docs.sh,
        # so the checked-in markdown points at the deployed docs-site URL.
        self.assertTrue(docs.coverage_index.startswith("https://"))
        self.assertTrue(docs.coverage_index.endswith("/reports/coverage/index.html"))
        # Binary-size is a tiny directory of pre-rendered HTML/SVG — ship
        # in-tree and link relatively so the GitHub web view renders it.
        self.assertEqual(
            docs.binary_size_report,
            "reports/binary-size/binary_size_report.html",
        )

        site = generate_build_report._resolve_link_targets(
            generate_build_report.LINK_MODE_SITE
        )
        self.assertTrue(site.coverage_index.startswith("https://"))
        self.assertTrue(site.coverage_index.endswith("/reports/coverage/index.html"))
        self.assertTrue(site.binary_size_report.startswith("https://"))

        with self.assertRaises(ValueError):
            generate_build_report._resolve_link_targets("bogus")

    def test_default_link_mode_prefers_docs_for_checked_in_report(self):
        self.assertEqual(
            generate_build_report._default_link_mode(Path("docs/build_report.md")),
            generate_build_report.LINK_MODE_DOCS,
        )
        self.assertEqual(
            generate_build_report._default_link_mode(Path("/tmp/report.md")),
            generate_build_report.LINK_MODE_LOCAL,
        )
        self.assertEqual(
            generate_build_report._default_link_mode(None),
            generate_build_report.LINK_MODE_LOCAL,
        )

    def test_coverage_section_hyperlinks_to_resolved_target(self):
        runner = FakeRunner(
            {
                ("tools/coverage.sh", "--quiet"): generate_build_report.CommandResult(
                    label="code-coverage",
                    args=("tools/coverage.sh", "--quiet"),
                    returncode=0,
                    stdout="Overall coverage rate:\n  lines.......: 85.2%",
                    stderr="",
                    duration_sec=120.0,
                )
            }
        )
        section = generate_build_report.make_code_coverage_section(
            runner,
            None,  # don't archive artifacts
            generate_build_report._resolve_link_targets(
                generate_build_report.LINK_MODE_DOCS
            ),
        )

        self.assertEqual(section.status, "success")
        self.assertIn(
            f"[coverage-report/index.html]({generate_build_report.DOCS_SITE_BASE_URL}"
            "/reports/coverage/index.html)",
            section.content,
        )
        self.assertIn("lines.......: 85.2%", section.content)

    def test_binary_size_section_local_mode_copies_bargraph_next_to_save(self):
        """In local mode with --save, the bargraph SVG must land beside
        the saved markdown so the image link resolves from any viewer."""
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            import os
            prior_cwd = Path.cwd()
            os.chdir(tmp)
            try:
                # Stage a fake build-binary-size/ at the faux workspace
                # root so the section copy has a source file to grab.
                fake_bs = tmp_path / "build-binary-size"
                fake_bs.mkdir()
                (fake_bs / "binary_size_bargraph.svg").write_text("<svg/>")

                save_dir = tmp_path / "elsewhere"
                save_dir.mkdir()

                runner = FakeRunner(
                    {
                        ("tools/binary_size.sh",): generate_build_report.CommandResult(
                            label="binary-size",
                            args=("tools/binary_size.sh",),
                            returncode=0,
                            stdout="total: 1.0M",
                            stderr="",
                            duration_sec=1.0,
                        )
                    }
                )
                section = generate_build_report.make_binary_size_section(
                    runner,
                    None,  # reports_root — not used in local mode
                    generate_build_report._resolve_link_targets(
                        generate_build_report.LINK_MODE_LOCAL
                    ),
                    local_asset_dir=save_dir,
                )

                # The bargraph must have been copied next to save_dir …
                self.assertTrue((save_dir / "binary_size_bargraph.svg").is_file())
                # … and the image link must be the bare basename so it
                # resolves from wherever the markdown is later viewed.
                self.assertIn(
                    "![Binary size bar graph](binary_size_bargraph.svg)",
                    section.content,
                )
                # Sanity: when an asset dir was provided, the local
                # workspace-relative bargraph path must not be the link.
                self.assertNotIn(
                    "![Binary size bar graph](build-binary-size/",
                    section.content,
                )
            finally:
                os.chdir(prior_cwd)

    def test_archive_coverage_reports_produces_zip(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            # Stage a fake coverage tree alongside where the generator
            # expects to find it (repo-relative "coverage-report/").
            import os
            prior_cwd = Path.cwd()
            os.chdir(tmp)
            try:
                fake_cov = tmp_path / "coverage-report"
                (fake_cov / "sub").mkdir(parents=True)
                (fake_cov / "index.html").write_text("<html>root</html>")
                (fake_cov / "sub" / "leaf.html").write_text("<html>leaf</html>")

                reports_root = tmp_path / "docs" / "reports"
                ok = generate_build_report._archive_coverage_reports(reports_root)
                self.assertTrue(ok)

                archive = reports_root / generate_build_report.COVERAGE_ARCHIVE_NAME
                self.assertTrue(archive.is_file())

                import zipfile
                with zipfile.ZipFile(archive) as zf:
                    names = sorted(zf.namelist())
                # Top-level dir is renamed to `coverage/` so that
                # `unzip -d reports/` produces reports/coverage/...
                self.assertIn("coverage/index.html", names)
                self.assertIn("coverage/sub/leaf.html", names)
            finally:
                os.chdir(prior_cwd)

    def test_command_runner_reports_progress(self):
        runner = generate_build_report.CommandRunner(
            progress_interval_sec=0.01,
            status_stream=io.StringIO(),
        )
        result = runner.run("python-sleep", ["python3", "-c", "import time; time.sleep(0.03)"])

        self.assertTrue(result.success)
        self.assertGreaterEqual(result.duration_sec, 0.02)
        status_output = runner.status_stream.getvalue()
        self.assertIn("[python-sleep] Running:", status_output)
        self.assertIn("[python-sleep] Still running", status_output)
        self.assertIn("[python-sleep] Completed", status_output)


if __name__ == "__main__":
    unittest.main()
