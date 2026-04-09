import io
import importlib.util
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

    def test_external_dependencies_section_uses_expected_categories(self):
        results = {
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
        section = generate_build_report.make_external_dependencies_section(FakeRunner(results))

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
        section = generate_build_report.make_binary_size_section(runner, None)

        self.assertEqual(section.status, "success")
        self.assertIn("Generated with: `tools/binary_size.sh`", section.content)
        self.assertIn("Total binary size of foo", section.content)
        self.assertNotIn("$ tools/binary_size.sh", section.content)

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
