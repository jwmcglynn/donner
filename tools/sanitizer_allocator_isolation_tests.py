"""Exercises sanitizer workflow commands to keep replacement allocators out."""

import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from python.runfiles import runfiles


class SanitizerAllocatorIsolationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        resolver = runfiles.Create()
        cls.workflow = Path(resolver.Rlocation("donner/.github/workflows/sanitizers.yml")).read_text(
            encoding="utf-8"
        )

    def _commands(self, sanitizer):
        marker = f"      - name: Test //donner/... with {sanitizer}\n        run: |\n"
        self.assertIn(marker, self.workflow)
        block = self.workflow.split(marker, 1)[1]
        lines = []
        for line in block.splitlines():
            if line.strip() and not line.startswith("          "):
                break
            lines.append(line[10:])
        script = "\n".join(lines)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = root / "commands.jsonl"
            stub = root / "bazelisk"
            stub.write_text(
                "#!/usr/bin/env python3\n"
                "import json, os, sys\n"
                "with open(os.environ['CAPTURE_COMMANDS'], 'a', encoding='utf-8') as output:\n"
                "    output.write(json.dumps(sys.argv[1:]) + '\\n')\n",
                encoding="utf-8",
            )
            stub.chmod(0o755)
            environment = dict(os.environ, CAPTURE_COMMANDS=str(capture))
            environment["PATH"] = str(root) + os.pathsep + environment["PATH"]
            result = subprocess.run(
                ["bash", "-euo", "pipefail", "-c", script],
                env=environment,
                capture_output=True,
                text=True,
                check=False,
                timeout=10,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            return [json.loads(line) for line in capture.read_text(encoding="utf-8").splitlines()]

    @staticmethod
    def _selects(arguments, target):
        patterns = [argument for argument in arguments if re.match(r"^-?//", argument)]

        def matches(pattern):
            return target.startswith(pattern[:-3]) if pattern.endswith("...") else target == pattern

        return any(matches(pattern) for pattern in patterns if not pattern.startswith("-")) and not any(
            matches(pattern[1:]) for pattern in patterns if pattern.startswith("-")
        )

    def test_replacement_allocator_binaries_are_excluded(self):
        targets = (
            "//donner/benchmarks:svg_parse_allocation_tests",
            "//donner/gpu:gpu_allocation_tests",
            "//donner/gpu/metal/tests:metal_buffer_bounds_tests",
        )
        for sanitizer in ("ASan", "UBSan"):
            commands = self._commands(sanitizer)
            self.assertGreater(len(commands), 0)
            for target in targets:
                with self.subTest(sanitizer=sanitizer, target=target):
                    self.assertEqual(
                        [command for command in commands if self._selects(command, target)],
                        [],
                        f"{sanitizer} must retain its allocator when running {target}",
                    )

    def test_regular_parser_and_gpu_tests_remain_selected(self):
        for sanitizer in ("ASan", "UBSan"):
            commands = self._commands(sanitizer)
            for target in ("//donner/svg/parser:svg_parser_tests", "//donner/gpu:device_tests"):
                with self.subTest(sanitizer=sanitizer, target=target):
                    self.assertEqual(sum(self._selects(command, target) for command in commands), 1)


if __name__ == "__main__":
    unittest.main()
