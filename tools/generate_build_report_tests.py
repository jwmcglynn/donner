import io
import importlib.util
import json
import tempfile
from pathlib import Path
import sys
import unittest
import zipfile
from unittest import mock


MODULE_PATH = Path(__file__).with_name("generate_build_report.py")
MODULE_SPEC = importlib.util.spec_from_file_location("generate_build_report", MODULE_PATH)
assert MODULE_SPEC is not None
assert MODULE_SPEC.loader is not None
generate_build_report = importlib.util.module_from_spec(MODULE_SPEC)
sys.modules[MODULE_SPEC.name] = generate_build_report
MODULE_SPEC.loader.exec_module(generate_build_report)
TESTS_ARGS = tuple(generate_build_report._TESTS_COMMAND_ARGS)


def failed_result(target, testlogs):
    """A FAILED summary record whose single run logged under ``testlogs``."""
    log = testlogs / generate_build_report._target_to_testlogs_subpath(target) / "test.log"
    return generate_build_report.TestCaseResult(target, "FAILED", "in 0.3s", (str(log),))


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
                "deps(//donner/editor:editor)",
                "--output=starlark",
                "--starlark:expr=target.label",
            ): generate_build_report.CommandResult(
                label="editor",
                args=(),
                returncode=0,
                stdout="\n".join(
                    [
                        "@+_repo_rules+entt//:entt",
                        "@+_repo_rules+tiny-skia-cpp//src:tiny_skia",
                        "@imgui+//:imgui",
                        "@glfw+//:glfw",
                    ]
                ),
                stderr="",
                duration_sec=1.5,
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
        self.assertIn("### editor (tiny-skia + imgui/glfw/tracy + editor fonts)", section.content)

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
                ("bazel", "build", "//third_party/licenses:notice_editor"):
                    self._notice_build_result("notice-editor"),
            }
            for configs in ([], ["--config=text-full"]):
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
            results[
                (
                    "bazel",
                    "cquery",
                    "deps(//donner/editor:editor)",
                    "--output=starlark",
                    "--starlark:expr=target.label",
                )
            ] = generate_build_report.CommandResult(
                label="editor-cquery",
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
                ("tools/feature_loc.py",): generate_build_report.CommandResult(
                    label="feature-loc",
                    args=("tools/feature_loc.py",),
                    returncode=0,
                    stdout="| Feature | Product LOC |\n| Text | 42 |",
                    stderr="",
                    duration_sec=0.2,
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
                TESTS_ARGS: generate_build_report.CommandResult(
                    label="tests",
                    args=TESTS_ARGS,
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
        self.assertIn("- Feature LOC: success (0.2s)", report)
        self.assertIn("- Tests: success (9.0s)", report)
        self.assertIn("- Public Targets: success (1.2s)", report)
        self.assertIn("## Local Changes", report)
        self.assertIn("## Feature LOC", report)
        self.assertIn("| Text | 42 |", report)
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

    def test_coverage_section_truncates_large_failure_output(self):
        limit = generate_build_report._MAX_COMMAND_OUTPUT_CHARS
        stderr = (
            "coverage-error-start\n"
            + ("x" * (limit + 1024))
            + "\ncoverage-error-end"
        )
        runner = FakeRunner(
            {
                ("tools/coverage.sh", "--quiet"): generate_build_report.CommandResult(
                    label="code-coverage",
                    args=("tools/coverage.sh", "--quiet"),
                    returncode=1,
                    stdout="",
                    stderr=stderr,
                    duration_sec=120.0,
                )
            }
        )
        section = generate_build_report.make_code_coverage_section(
            runner,
            None,
            generate_build_report._resolve_link_targets(
                generate_build_report.LINK_MODE_DOCS
            ),
        )

        self.assertEqual(section.status, "failed")
        self.assertIn("coverage-error-start", section.content)
        self.assertIn("coverage-error-end", section.content)
        self.assertIn("omitted", section.content)
        self.assertLess(len(section.content), len(stderr))

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

    def test_parse_bazel_test_results_extracts_per_target_status(self):
        stdout = "\n".join(
            [
                "INFO: Analyzed 3 targets.",
                "//donner/base:base_tests                                  PASSED in 1.2s",
                "//donner/svg:svg_tests                          (cached) PASSED in 0.0s",
                "//donner/svg/renderer/tests:renderer_geode_tests          SKIPPED",
                "//donner/css:css_tests                                    FAILED in 0.3s",
                "Executed 2 out of 3 tests: 2 tests pass and 1 fails.",
            ]
        )
        results = generate_build_report.parse_bazel_test_results(stdout)
        by_target = {r.target: r for r in results}

        self.assertEqual(by_target["//donner/base:base_tests"].status, "PASSED")
        self.assertEqual(by_target["//donner/base:base_tests"].detail, "in 1.2s")
        self.assertEqual(by_target["//donner/svg:svg_tests"].status, "PASSED")
        self.assertEqual(
            by_target["//donner/svg/renderer/tests:renderer_geode_tests"].status,
            "SKIPPED",
        )
        self.assertEqual(by_target["//donner/css:css_tests"].status, "FAILED")
        # The "Executed ... tests" trailer and INFO lines are not targets.
        self.assertEqual(len(results), 4)

    def test_parse_bazel_test_results_prefers_multiword_status(self):
        # "FAILED TO BUILD" must not be parsed as "FAILED" + detail "TO BUILD".
        stdout = "//donner/x:y                          FAILED TO BUILD"
        results = generate_build_report.parse_bazel_test_results(stdout)
        self.assertEqual(len(results), 1)
        self.assertEqual(results[0].status, "FAILED TO BUILD")
        self.assertEqual(results[0].detail, "")

    def test_render_test_results_table_collapses_no_status_rows(self):
        table = generate_build_report._render_test_results_table(
            [
                generate_build_report.TestCaseResult(
                    "//donner/css:css_tests", "FAILED TO BUILD", ""
                ),
                generate_build_report.TestCaseResult(
                    "//donner/base:base_tests", "NO STATUS", ""
                ),
                generate_build_report.TestCaseResult(
                    "//donner/svg:svg_tests", "NO STATUS", ""
                ),
            ]
        )

        self.assertIn("3 targets: 1 failed to build, 2 no status.", table)
        self.assertIn("| `//donner/css:css_tests` | FAILED TO BUILD |", table)
        self.assertNotIn("| `//donner/base:base_tests` | NO STATUS |", table)
        self.assertIn("Omitted 2 no status target rows", table)

    def test_tests_section_renders_structured_table_and_raw_block(self):
        stdout = "\n".join(
            [
                "//donner/base:base_tests          PASSED in 1.2s",
                "//donner/css:css_tests            FAILED in 0.3s",
                "//donner/svg:svg_tests            SKIPPED",
            ]
        )
        runner = FakeRunner(
            {
                TESTS_ARGS: generate_build_report.CommandResult(
                    label="tests",
                    args=TESTS_ARGS,
                    returncode=3,
                    stdout=stdout,
                    stderr="",
                    duration_sec=12.0,
                )
            }
        )
        section = generate_build_report.make_tests_section(
            runner,
            None,  # no reports_root → no image harvest
            bazel_testlogs=None,
        )

        self.assertEqual(section.status, "failed")
        self.assertEqual(runner.calls[0], ("tests", TESTS_ARGS))
        self.assertIn("--test_tag_filters=-lint", TESTS_ARGS)
        self.assertIn("--remote_executor=", TESTS_ARGS)
        self.assertIn("### Test results", section.content)
        self.assertIn("| Target | Result |", section.content)
        self.assertIn("| `//donner/css:css_tests` | FAILED in 0.3s |", section.content)
        self.assertIn("3 targets: 1 failed, 1 passed, 1 skipped.", section.content)
        # Failures sort to the top of the table.
        failed_idx = section.content.index("//donner/css:css_tests")
        passed_idx = section.content.index("//donner/base:base_tests")
        self.assertLess(failed_idx, passed_idx)
        # The raw bazel output is preserved in a collapsible block.
        self.assertIn("Raw <code>bazel test</code> output", section.content)
        # Without a reports_root we still name the failed target.
        self.assertIn("### Failed image comparisons", section.content)
        self.assertIn("`//donner/css:css_tests`", section.content)

    def test_tests_section_parses_results_from_stderr(self):
        # Real `bazel test` writes its per-target summary to stderr, leaving stdout
        # empty. The structured table and failed-target harvest must still populate.
        stderr = "\n".join(
            [
                "//donner/base:base_tests          PASSED in 1.2s",
                "//donner/css:css_tests            FAILED in 0.3s",
            ]
        )
        runner = FakeRunner(
            {
                TESTS_ARGS: generate_build_report.CommandResult(
                    label="tests",
                    args=TESTS_ARGS,
                    returncode=3,
                    stdout="",
                    stderr=stderr,
                    duration_sec=12.0,
                )
            }
        )
        section = generate_build_report.make_tests_section(
            runner,
            None,  # no reports_root → no image harvest
            bazel_testlogs=None,
        )

        self.assertEqual(section.status, "failed")
        self.assertIn("| `//donner/css:css_tests` | FAILED in 0.3s |", section.content)
        self.assertIn("2 targets: 1 failed, 1 passed.", section.content)
        # The stderr-only failure is still surfaced for image harvesting.
        self.assertIn("### Failed image comparisons", section.content)
        self.assertIn("`//donner/css:css_tests`", section.content)

    def test_documentation_section_links_to_doxygen(self):
        links = generate_build_report._resolve_link_targets(
            generate_build_report.LINK_MODE_DOCS
        )
        section = generate_build_report.make_documentation_section(links)
        self.assertEqual(section.status, "success")
        self.assertIn("API documentation", section.content)
        self.assertIn(generate_build_report.DOCS_SITE_BASE_URL, section.content)

    def test_resolve_link_targets_includes_doxygen(self):
        local = generate_build_report._resolve_link_targets(
            generate_build_report.LINK_MODE_LOCAL
        )
        self.assertEqual(local.doxygen_index, "generated-doxygen/html/index.html")
        docs = generate_build_report._resolve_link_targets(
            generate_build_report.LINK_MODE_DOCS
        )
        self.assertTrue(docs.doxygen_index.startswith("https://"))

    def test_harvest_failed_image_artifacts_copies_loose_pngs(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            testlogs = tmp_path / "testlogs"
            outputs = (
                testlogs
                / "donner/svg/renderer/tests/renderer_geode_golden_tests/test.outputs"
            )
            outputs.mkdir(parents=True)
            (outputs / "expected_Foo.png").write_bytes(b"\x89PNG-golden")
            (outputs / "actual_Foo.png").write_bytes(b"\x89PNG-actual")
            (outputs / "diff_Foo.png").write_bytes(b"\x89PNG-diff")
            (outputs / "test.log").write_text("ignore me")

            destination = tmp_path / "docs" / "reports" / "test-failures"
            harvested, notes = generate_build_report._harvest_failed_image_artifacts(
                [
                    failed_result(
                        "//donner/svg/renderer/tests:renderer_geode_golden_tests", testlogs
                    )
                ],
                destination,
                testlogs,
            )

            target = "//donner/svg/renderer/tests:renderer_geode_golden_tests"
            self.assertEqual(notes, {})
            self.assertIn(target, harvested)
            rels = harvested[target]
            self.assertEqual(len(rels), 3)
            self.assertTrue(
                any(r.endswith("diff_Foo.png") for r in rels), rels
            )
            # Non-image artifacts are not copied.
            self.assertFalse(any("test.log" in r for r in rels))
            # The files actually landed under destination.
            for rel in rels:
                self.assertTrue((destination / rel).is_file())

    def test_harvest_failed_image_artifacts_reads_outputs_zip(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            testlogs = tmp_path / "testlogs"
            outputs = testlogs / "donner/css/tests/css_tests/test.outputs"
            outputs.mkdir(parents=True)
            import zipfile as _zip

            with _zip.ZipFile(outputs / "outputs.zip", "w") as zf:
                zf.writestr("diff_Bar.png", b"\x89PNG-diff")
                zf.writestr("expected_Bar.png", b"\x89PNG-golden")
                zf.writestr("some_other_log.txt", b"not an image")

            destination = tmp_path / "out"
            harvested, notes = generate_build_report._harvest_failed_image_artifacts(
                [failed_result("//donner/css/tests:css_tests", testlogs)],
                destination,
                testlogs,
            )
            target = "//donner/css/tests:css_tests"
            self.assertEqual(notes, {})
            self.assertIn(target, harvested)
            self.assertEqual(len(harvested[target]), 2)
            for rel in harvested[target]:
                self.assertTrue((destination / rel).is_file())
            # The transient _unzip staging dir is cleaned up.
            self.assertFalse(
                (destination / "donner/css/tests/css_tests/_unzip").exists()
            )

    def test_render_failed_image_comparisons_embeds_images(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            testlogs = tmp_path / "testlogs"
            outputs = testlogs / "donner/svg/tests/svg_tests/test.outputs"
            outputs.mkdir(parents=True)
            (outputs / "diff_Case.png").write_bytes(b"\x89PNG")

            reports_root = tmp_path / "docs" / "reports"
            reports_root.mkdir(parents=True)
            block = generate_build_report._render_failed_image_comparisons(
                [failed_result("//donner/svg/tests:svg_tests", testlogs)],
                reports_root,
                bazel_testlogs=testlogs,
            )
            self.assertIn("### Failed image comparisons", block)
            self.assertIn("#### `//donner/svg/tests:svg_tests`", block)
            self.assertIn(
                "![diff_Case.png](reports/test-failures/"
                "donner/svg/tests/svg_tests/diff_Case.png)",
                block,
            )

    def test_render_failed_image_comparisons_omits_when_no_artifacts(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            testlogs = tmp_path / "testlogs"
            # A failed target with no image outputs at all.
            (testlogs / "donner/base/base_tests/test.outputs").mkdir(parents=True)
            reports_root = tmp_path / "reports"
            reports_root.mkdir()
            block = generate_build_report._render_failed_image_comparisons(
                [failed_result("//donner/base:base_tests", testlogs)],
                reports_root,
                bazel_testlogs=testlogs,
            )
            self.assertEqual(block, "")

    def test_command_runner_reports_progress(self):
        runner = generate_build_report.CommandRunner(
            progress_interval_sec=0.01,
            status_stream=io.StringIO(),
        )
        result = runner.run(
            "python-sleep", [sys.executable, "-c", "import time; time.sleep(0.03)"]
        )

        self.assertTrue(result.success)
        self.assertGreaterEqual(result.duration_sec, 0.02)
        status_output = runner.status_stream.getvalue()
        self.assertIn("[python-sleep] Running:", status_output)
        self.assertIn("[python-sleep] Still running", status_output)
        self.assertIn("[python-sleep] Completed", status_output)


# `bazel test` summaries adapted from Bazel 9.3 on Linux for failing targets that write
# image-comparison PNGs, with the testlogs root replaced by <testlogs>. Undeclared outputs are
# loose files under each run's test.outputs. A sharded target lists one log per shard, and
# --runs_per_test and --flaky_test_attempts add run directories and attempt logs.
_UNSHARDED_SUMMARY = """\
INFO: Build completed, 1 test FAILED, 26 total actions
//donner/example:image_tests                                             FAILED in 0.4s
  <testlogs>/donner/example/image_tests/test.log

Executed 1 out of 1 test: 1 fails locally.
"""

_SHARDED_SUMMARY = """\
FAIL: //donner/example:sharded_image_tests (shard 1 of 2) (Exit 1) (see <testlogs>/donner/example/sharded_image_tests/shard_1_of_2/test.log)
FAIL: //donner/example:sharded_image_tests (shard 2 of 2) (Exit 1) (see <testlogs>/donner/example/sharded_image_tests/shard_2_of_2/test.log)
FAILED: //donner/example:sharded_image_tests (Summary)
      <testlogs>/donner/example/sharded_image_tests/shard_1_of_2/test.log
      <testlogs>/donner/example/sharded_image_tests/shard_2_of_2/test.log
INFO: Build completed, 1 test FAILED, 26 total actions
//donner/example:sharded_image_tests                                     FAILED in 2 out of 2 in 0.4s
  Stats over 2 runs: max = 0.4s, min = 0.4s, avg = 0.4s, dev = 0.0s
  <testlogs>/donner/example/sharded_image_tests/shard_1_of_2/test.log
  <testlogs>/donner/example/sharded_image_tests/shard_2_of_2/test.log

Executed 1 out of 1 test: 1 fails locally.
"""

_SHARDED_RUNS_SUMMARY = """\
INFO: Build completed, 1 test FAILED, 7 total actions
//donner/example:sharded_image_tests                                     FAILED in 4 out of 4 in 0.4s
  Stats over 4 runs: max = 0.4s, min = 0.4s, avg = 0.4s, dev = 0.0s
  <testlogs>/donner/example/sharded_image_tests/shard_1_of_2_run_2_of_2/test.log
  <testlogs>/donner/example/sharded_image_tests/shard_2_of_2_run_2_of_2/test.log
  <testlogs>/donner/example/sharded_image_tests/shard_2_of_2_run_1_of_2/test.log
  <testlogs>/donner/example/sharded_image_tests/shard_1_of_2_run_1_of_2/test.log

Executed 1 out of 1 test: 1 fails locally.
"""

_SHARDED_ATTEMPTS_SUMMARY = """\
INFO: Build completed, 1 test FAILED, 4 total actions
//donner/example:sharded_image_tests                                     FAILED in 4 out of 4 in 0.4s
  Stats over 4 runs: max = 0.4s, min = 0.3s, avg = 0.4s, dev = 0.0s
  <testlogs>/donner/example/sharded_image_tests/shard_1_of_2/test.log
  <testlogs>/donner/example/sharded_image_tests/shard_1_of_2/test_attempts/attempt_1.log
  <testlogs>/donner/example/sharded_image_tests/shard_2_of_2/test.log
  <testlogs>/donner/example/sharded_image_tests/shard_2_of_2/test_attempts/attempt_1.log

Executed 1 out of 1 test: 1 fails locally.
"""

_TIMEOUT_SUMMARY = """\
INFO: Build completed, 1 test FAILED, 5 total actions
//donner/example:image_tests                                            TIMEOUT in 5.3s
  <testlogs>/donner/example/image_tests/test.log

Executed 1 out of 1 test: 1 fails remotely.
"""

# A summary line with no log listed under it.
_NO_LOG_SUMMARY = """\
//donner/example:image_tests                                             {status} in 0.4s
"""

# A test rule with its own configuration transition logs under that configuration's testlogs
# directory (here a name taken from a real output tree), not the one `bazel info bazel-testlogs`
# reports.
_TRANSITIONED_SUMMARY = """\
//donner/example:image_tests                                             FAILED in 1.0s
  <bazel-out>/aarch64-fastbuild-ST-641315aedb22/testlogs/donner/example/image_tests/test.log
"""

_UNLOCATED_NOTE = (
    ": failure images, if any, could not be located; the test summary listed no log for it "
    "under this output tree."
)
_NO_TESTLOGS_NOTE = (
    ": failure images, if any, could not be located; the Bazel testlogs directory could not "
    "be determined."
)
_NO_OUTPUTS_NOTE = (
    ": failure images, if any, could not be read; a failed run left no test outputs directory."
)
_OUTSIDE_RUN_NOTE = (
    ": some test outputs were not read because they resolve outside the run's testlogs "
    "directory."
)

_SHARDED_TARGET_DIR = "donner/example/sharded_image_tests"
_SHARDED_REPORT_PREFIX = "reports/test-failures/" + _SHARDED_TARGET_DIR


class FailureImageLayoutTests(unittest.TestCase):
    """The tests section finds failure images in every testlogs layout Bazel writes."""

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.bazel_out = Path(tmp.name) / "bazel-out"
        self.testlogs = self.bazel_out / "aarch64-fastbuild" / "testlogs"
        self.reports_root = Path(tmp.name) / "docs" / "reports"
        self.reports_root.mkdir(parents=True)

    def write_run(self, run_dir, images, outputs_name="test.outputs", log_name="test.log"):
        """Lay out one run directory the way Bazel 9 leaves it after a failure."""
        directory = self.testlogs / run_dir
        outputs = directory / outputs_name
        outputs.mkdir(parents=True)
        (directory / log_name).write_text("failure log\n")
        if log_name == "test.log":
            (directory / "test.xml").write_text("<testsuites/>\n")
            (directory / "test.outputs_manifest").mkdir()
            (directory / "test.outputs_manifest" / "MANIFEST").write_text("")
        for name, payload in images.items():
            (outputs / name).write_bytes(payload)

    def render_tests_section(self, summary, bazel_testlogs=None):
        stderr = summary.replace("<testlogs>", str(self.testlogs)).replace(
            "<bazel-out>", str(self.bazel_out)
        )
        runner = FakeRunner(
            {
                TESTS_ARGS: generate_build_report.CommandResult(
                    label="tests",
                    args=TESTS_ARGS,
                    returncode=3,
                    stdout="",
                    stderr=stderr,
                    duration_sec=1.0,
                )
            }
        )
        return generate_build_report.make_tests_section(
            runner, self.reports_root, bazel_testlogs=bazel_testlogs or self.testlogs
        )

    @staticmethod
    def embedded_images(section):
        return [line for line in section.content.splitlines() if line.startswith("![")]

    @staticmethod
    def unlocated_notes(section):
        return [line for line in section.content.splitlines() if line.startswith("- `")]

    def staged_bytes(self, href):
        return (self.reports_root.parent / href).read_bytes()

    def write_two_failed_shards(self, run_suffix=""):
        for shard in (1, 2):
            self.write_run(
                f"{_SHARDED_TARGET_DIR}/shard_{shard}_of_2{run_suffix}",
                {
                    "diff_Same.png": f"shard {shard}{run_suffix} diff".encode(),
                    f"actual_Shard{shard}.png": f"shard {shard}{run_suffix} actual".encode(),
                },
            )

    def expected_shard_images(self, run_suffix=""):
        lines = []
        for shard in (1, 2):
            run = f"shard_{shard}_of_2{run_suffix}"
            for name in (f"actual_Shard{shard}.png", "diff_Same.png"):
                lines.append(f"![{run}/{name}]({_SHARDED_REPORT_PREFIX}/{run}/{name})")
        return lines

    def test_unsharded_target_images_are_embedded(self):
        self.write_run(
            "donner/example/image_tests",
            {"diff_Case.png": b"diff", "expected_Case.png": b"golden"},
        )

        section = self.render_tests_section(_UNSHARDED_SUMMARY)

        self.assertEqual(
            self.embedded_images(section),
            [
                "![diff_Case.png](reports/test-failures/donner/example/image_tests/diff_Case.png)",
                "![expected_Case.png](reports/test-failures/donner/example/image_tests/"
                "expected_Case.png)",
            ],
        )

    def test_every_failed_shard_contributes_its_images(self):
        self.write_two_failed_shards()

        section = self.render_tests_section(_SHARDED_SUMMARY)

        self.assertEqual(self.embedded_images(section), self.expected_shard_images())
        # Same-named images from different shards keep their own bytes.
        self.assertEqual(
            [
                self.staged_bytes(f"{_SHARDED_REPORT_PREFIX}/shard_{shard}_of_2/diff_Same.png")
                for shard in (1, 2)
            ],
            [b"shard 1 diff", b"shard 2 diff"],
        )

    def test_stale_unsharded_results_beside_shards_are_ignored(self):
        # An earlier unsharded run (for example with --test_sharding_strategy=disabled) leaves
        # test.xml, test.log and test.outputs at the target root beside the shard directories.
        self.write_run(
            _SHARDED_TARGET_DIR,
            {"diff_Same.png": b"stale diff", "diff_Stale.png": b"stale"},
        )
        self.write_two_failed_shards()

        section = self.render_tests_section(_SHARDED_SUMMARY)

        self.assertEqual(self.embedded_images(section), self.expected_shard_images())
        self.assertFalse((self.reports_root / "test-failures" / _SHARDED_TARGET_DIR /
                          "diff_Stale.png").exists())

    def test_runs_per_test_directories_contribute_their_images(self):
        for run in (1, 2):
            self.write_two_failed_shards(f"_run_{run}_of_2")

        section = self.render_tests_section(_SHARDED_RUNS_SUMMARY)

        self.assertEqual(
            self.embedded_images(section),
            sorted(
                self.expected_shard_images("_run_1_of_2")
                + self.expected_shard_images("_run_2_of_2"),
                key=lambda line: line.split("](")[1],
            ),
        )

    def test_retried_attempts_contribute_their_images(self):
        self.write_two_failed_shards()
        for shard in (1, 2):
            self.write_run(
                f"{_SHARDED_TARGET_DIR}/shard_{shard}_of_2/test_attempts",
                {"diff_Same.png": f"shard {shard} attempt 1".encode()},
                outputs_name="attempt_1.outputs",
                log_name="attempt_1.log",
            )

        section = self.render_tests_section(_SHARDED_ATTEMPTS_SUMMARY)

        expected = []
        for shard in (1, 2):
            run = f"shard_{shard}_of_2"
            attempt = f"{run}/test_attempts/attempt_1"
            for image in (f"{run}/actual_Shard{shard}.png", f"{run}/diff_Same.png",
                          f"{attempt}/diff_Same.png"):
                expected.append(f"![{image}]({_SHARDED_REPORT_PREFIX}/{image})")
        self.assertEqual(self.embedded_images(section), expected)
        self.assertEqual(
            self.staged_bytes(f"{_SHARDED_REPORT_PREFIX}/shard_2_of_2/test_attempts/attempt_1/"
                              "diff_Same.png"),
            b"shard 2 attempt 1",
        )

    def test_logs_outside_the_target_directory_are_ignored(self):
        self.write_run("donner/other/other_tests", {"diff_Other.png": b"other"})
        summary = _UNSHARDED_SUMMARY.replace(
            "<testlogs>/donner/example/image_tests/test.log",
            "<testlogs>/donner/example/image_tests/../../other/other_tests/test.log",
        )

        section = self.render_tests_section(summary)

        self.assertEqual(self.embedded_images(section), [])
        self.assertEqual(
            self.unlocated_notes(section), ["- `//donner/example:image_tests`" + _UNLOCATED_NOTE]
        )
        self.assertFalse((self.reports_root / "test-failures").exists())

    def test_tests_command_pins_the_summary_format_it_parses(self):
        # Explicit flags win over a user rc that changes the summary, its colors or its paths.
        self.assertEqual(
            [arg for arg in TESTS_ARGS if arg.split("=")[0] in (
                "--test_summary", "--color", "--noprint_relative_test_log_paths")],
            ["--test_summary=short", "--color=no", "--noprint_relative_test_log_paths"],
        )

    def test_failed_target_without_listed_logs_is_named(self):
        self.write_run("donner/example/image_tests", {"diff_Case.png": b"diff"})

        section = self.render_tests_section(_NO_LOG_SUMMARY.format(status="FAILED"))

        self.assertEqual(self.embedded_images(section), [])
        self.assertEqual(
            self.unlocated_notes(section), ["- `//donner/example:image_tests`" + _UNLOCATED_NOTE]
        )

    def test_timed_out_target_without_listed_logs_is_named(self):
        self.write_run("donner/example/image_tests", {"diff_Case.png": b"diff"})

        section = self.render_tests_section(_NO_LOG_SUMMARY.format(status="TIMEOUT"))

        self.assertEqual(
            self.unlocated_notes(section), ["- `//donner/example:image_tests`" + _UNLOCATED_NOTE]
        )

    def test_timed_out_target_images_are_embedded(self):
        self.write_run("donner/example/image_tests", {"diff_Case.png": b"diff"})

        section = self.render_tests_section(_TIMEOUT_SUMMARY)

        self.assertEqual(
            self.embedded_images(section),
            ["![diff_Case.png](reports/test-failures/donner/example/image_tests/diff_Case.png)"],
        )
        self.assertEqual(self.unlocated_notes(section), [])

    def test_transitioned_configuration_log_is_harvested(self):
        configuration = "aarch64-fastbuild-ST-641315aedb22"
        target_logs = self.bazel_out / configuration / "testlogs" / "donner/example/image_tests"
        outputs = target_logs / "test.outputs"
        outputs.mkdir(parents=True)
        (target_logs / "test.log").write_text("failure log\n")
        (outputs / "diff_Case.png").write_bytes(b"transitioned diff")

        section = self.render_tests_section(_TRANSITIONED_SUMMARY)

        image = f"donner/example/image_tests/{configuration}/diff_Case.png"
        self.assertEqual(
            self.embedded_images(section),
            [f"![{configuration}/diff_Case.png](reports/test-failures/{image})"],
        )
        self.assertEqual(self.unlocated_notes(section), [])
        self.assertEqual(self.staged_bytes(f"reports/test-failures/{image}"), b"transitioned diff")

    def test_outputs_resolving_outside_the_run_are_not_copied(self):
        outside = self.testlogs.parent / "outside"
        outside.mkdir(parents=True)
        (outside / "diff_Leak.png").write_bytes(b"leak")
        shard_1 = self.testlogs / _SHARDED_TARGET_DIR / "shard_1_of_2"
        shard_1.mkdir(parents=True)
        (shard_1 / "test.log").write_text("failure log\n")
        (shard_1 / "test.outputs").symlink_to(outside, target_is_directory=True)
        self.write_run(f"{_SHARDED_TARGET_DIR}/shard_2_of_2", {"diff_Ok.png": b"ok"})
        (self.testlogs / _SHARDED_TARGET_DIR / "shard_2_of_2/test.outputs/diff_Link.png").symlink_to(
            outside / "diff_Leak.png"
        )

        section = self.render_tests_section(_SHARDED_SUMMARY)

        self.assertEqual(
            self.embedded_images(section),
            [f"![shard_2_of_2/diff_Ok.png]({_SHARDED_REPORT_PREFIX}/shard_2_of_2/diff_Ok.png)"],
        )
        self.assertEqual(
            self.unlocated_notes(section),
            ["- `//donner/example:sharded_image_tests`" + _OUTSIDE_RUN_NOTE],
        )

    def test_outputs_zip_resolving_outside_the_run_is_not_read(self):
        outside = self.testlogs.parent / "outside"
        outside.mkdir(parents=True)
        with zipfile.ZipFile(outside / "outputs.zip", "w") as archive:
            archive.writestr("diff_Zip.png", b"leak")
        self.write_run("donner/example/image_tests", {})
        outputs = self.testlogs / "donner/example/image_tests/test.outputs"
        (outputs / "outputs.zip").symlink_to(outside / "outputs.zip")

        section = self.render_tests_section(_UNSHARDED_SUMMARY)

        self.assertEqual(self.embedded_images(section), [])
        self.assertEqual(
            self.unlocated_notes(section), ["- `//donner/example:image_tests`" + _OUTSIDE_RUN_NOTE]
        )

    def test_failed_run_without_test_outputs_is_named(self):
        run = self.testlogs / "donner/example/image_tests"
        run.mkdir(parents=True)
        (run / "test.log").write_text("failure log\n")

        section = self.render_tests_section(_UNSHARDED_SUMMARY)

        self.assertEqual(
            self.unlocated_notes(section), ["- `//donner/example:image_tests`" + _NO_OUTPUTS_NOTE]
        )

    def test_failed_run_without_images_is_not_named(self):
        self.write_run("donner/example/image_tests", {})

        section = self.render_tests_section(_UNSHARDED_SUMMARY)

        self.assertNotIn("### Failed image comparisons", section.content)

    def test_unresolved_testlogs_names_every_failed_target_with_that_cause(self):
        self.write_run("donner/example/image_tests", {"diff_Case.png": b"diff"})

        with mock.patch.object(generate_build_report, "_resolve_bazel_testlogs", return_value=None):
            section = generate_build_report.make_tests_section(
                FakeRunner(
                    {
                        TESTS_ARGS: generate_build_report.CommandResult(
                            "tests", TESTS_ARGS, 3, "",
                            _UNSHARDED_SUMMARY.replace("<testlogs>", str(self.testlogs)), 1.0,
                        )
                    }
                ),
                self.reports_root,
                bazel_testlogs=None,
            )

        self.assertEqual(self.embedded_images(section), [])
        self.assertEqual(
            self.unlocated_notes(section), ["- `//donner/example:image_tests`" + _NO_TESTLOGS_NOTE]
        )

    def test_testlogs_of_an_unexpected_shape_is_not_used(self):
        logs = self.bazel_out / "aarch64-fastbuild" / "logs"
        outputs = logs / "donner/example/image_tests/test.outputs"
        outputs.mkdir(parents=True)
        (outputs.parent / "test.log").write_text("failure log\n")
        (outputs / "diff_Case.png").write_bytes(b"diff")

        section = self.render_tests_section(
            _UNSHARDED_SUMMARY.replace("<testlogs>", str(logs)), bazel_testlogs=logs
        )

        self.assertEqual(self.embedded_images(section), [])
        self.assertEqual(
            self.unlocated_notes(section), ["- `//donner/example:image_tests`" + _NO_TESTLOGS_NOTE]
        )

    def test_parse_records_the_logs_listed_under_each_target(self):
        def parse(summary):
            return [
                (r.target, r.status, r.logs)
                for r in generate_build_report.parse_bazel_test_results(
                    summary.replace("<testlogs>", "/out/testlogs")
                )
            ]

        # The per-shard FAIL lines and the mid-run "(Summary)" block do not add logs, even
        # when another target's summary block came before them.
        self.assertEqual(
            parse(_UNSHARDED_SUMMARY + _SHARDED_SUMMARY),
            [
                (
                    "//donner/example:image_tests",
                    "FAILED",
                    ("/out/testlogs/donner/example/image_tests/test.log",),
                ),
                (
                    "//donner/example:sharded_image_tests",
                    "FAILED",
                    (
                        f"/out/testlogs/{_SHARDED_TARGET_DIR}/shard_1_of_2/test.log",
                        f"/out/testlogs/{_SHARDED_TARGET_DIR}/shard_2_of_2/test.log",
                    ),
                )
            ],
        )
        self.assertEqual(
            parse(_SHARDED_ATTEMPTS_SUMMARY + _UNSHARDED_SUMMARY),
            [
                (
                    "//donner/example:image_tests",
                    "FAILED",
                    ("/out/testlogs/donner/example/image_tests/test.log",),
                ),
                (
                    "//donner/example:sharded_image_tests",
                    "FAILED",
                    tuple(
                        f"/out/testlogs/{_SHARDED_TARGET_DIR}/{run}"
                        for run in (
                            "shard_1_of_2/test.log",
                            "shard_1_of_2/test_attempts/attempt_1.log",
                            "shard_2_of_2/test.log",
                            "shard_2_of_2/test_attempts/attempt_1.log",
                        )
                    ),
                ),
            ],
        )

if __name__ == "__main__":
    unittest.main()
