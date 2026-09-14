"""Discovery preserves full-text editor fuzzers and excludes non-public entrypoints."""

from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from run_continuous_fuzz import discover_targets


class DiscoverTargetsTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.repo_root = Path(directory.name)

    def test_discovers_public_native_and_transitioned_fuzzers(self):
        rules = [
            ("cc_binary rule", "//donner/base:parser_fuzzer_bin", ["fuzz_target"]),
            *[
                (
                    "donner_multi_transitioned_binary rule",
                    f"//donner/editor/tests:{name}_bin",
                    ["fuzz_target"],
                )
                for name in (
                    "editor_state_machine_fuzzer",
                    "inspector_ui_fuzzer",
                    "viewport_svg_export_roundtrip_fuzzer",
                    "shape_clipboard_paste_fuzzer",
                )
            ],
            (
                'cc_binary rule',
                '//donner/editor/tests:inspector_ui_fuzzer_bin_impl',
                ['manual', 'fuzz_target'],
            ),
            (
                '_donner_multi_transitioned_test rule',
                '//donner/editor/tests:inspector_ui_fuzzer',
                ['fuzz_target'],
            ),
            (
                '_fuzzer_soak_test rule',
                '//donner/editor/tests:inspector_ui_fuzzer_soak',
                ['fuzz_target'],
            ),
            ("donner_multi_transitioned_binary rule", "//tools:unrelated_bin", []),
            (
                'other_donner_multi_transitioned_binary rule',
                '//tools:unrelated_fuzzer_bin',
                ['fuzz_target'],
            ),
            ("cc_binary_other rule", "//tools:other_fuzzer_bin", ["fuzz_target"]),
        ]

        def bazel_query(command, **kwargs):
            assert command[:2] == ["bazel", "query"]
            assert kwargs == {"cwd": self.repo_root, "capture_output": True, "text": True}
            query = command[2]
            assert 'attr(tags, "fuzz_target", //...)' in query
            kind_match = re.search(r'kind\("([^"\n]+)", //\.\.\.\)', query)
            assert kind_match is not None
            kind_pattern = re.compile(kind_match[1])
            selected = [
                label for kind, label, tags in rules
                if "fuzz_target" in tags and kind_pattern.search(kind)
            ]
            return subprocess.CompletedProcess(command, 0, stdout="\n".join(selected), stderr="")

        with mock.patch("run_continuous_fuzz.subprocess.run", side_effect=bazel_query):
            targets = discover_targets(self.repo_root)
        self.assertEqual([target.name for target in targets], [
            "editor_state_machine_fuzzer",
            "inspector_ui_fuzzer",
            "parser_fuzzer",
            "shape_clipboard_paste_fuzzer",
            "viewport_svg_export_roundtrip_fuzzer",
        ])


if __name__ == "__main__":
    unittest.main()
