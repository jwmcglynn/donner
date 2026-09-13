"""Exercises sanitizer workflow commands to keep replacement allocators out."""

import ast
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
        cls.repo = Path(resolver.Rlocation("donner/tools/ci/test_with_exclusions.py")).parents[2]
        cls.suites = {}
        tree = ast.parse(Path(resolver.Rlocation("donner/tools/ci/BUILD.bazel")).read_text())
        for statement in tree.body:
            if isinstance(statement, ast.Expr) and isinstance(statement.value, ast.Call):
                call = statement.value
                if isinstance(call.func, ast.Name) and call.func.id == "test_suite":
                    attributes = {item.arg: ast.literal_eval(item.value) for item in call.keywords}
                    cls.suites["//tools/ci:" + attributes["name"]] = attributes["tests"]
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
                "    output.write(json.dumps(sys.argv[1:]) + '\\n')\n"
                "if sys.argv[1] == 'query':\n"
                "    print('\\n'.join(json.loads(os.environ['QUERY_SUITES'])[sys.argv[-1]]))\n",
                encoding="utf-8",
            )
            stub.chmod(0o755)
            query_suites = {f"tests({name})": self._expand_suite(name) for name in self.suites}
            environment = dict(os.environ, CAPTURE_COMMANDS=str(capture),
                               QUERY_SUITES=json.dumps(query_suites))
            environment["PATH"] = str(root) + os.pathsep + environment["PATH"]
            result = subprocess.run(
                ["bash", "-euo", "pipefail", "-c", script],
                env=environment,
                cwd=self.repo,
                capture_output=True,
                text=True,
                check=False,
                timeout=10,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            commands = [json.loads(line) for line in capture.read_text(encoding="utf-8").splitlines()]
            return [command for command in commands if command[0] == "test"]

    @classmethod
    def _expand_suite(cls, label):
        if label.startswith(":"):
            label = "//tools/ci" + label
        if label not in cls.suites:
            return [label]
        return [target for member in cls.suites[label] for target in cls._expand_suite(member)]

    @classmethod
    def _selects(cls, arguments, target):
        patterns = [argument for argument in arguments if re.match(r"^-?//", argument)]

        def matches(pattern):
            if not pattern.endswith("/..."):
                return target in cls._expand_suite(pattern)
            package = target.split(":", 1)[0]
            root = pattern[:-4]
            return package == root or package.startswith(root + "/")

        return any(matches(pattern) for pattern in patterns if not pattern.startswith("-")) and not any(
            matches(pattern[1:]) for pattern in patterns if pattern.startswith("-")
        )

    def test_recursive_exclusions_cover_the_package_and_descendants(self):
        arguments = ["//donner/...", "-//donner/gpu/..."]
        selection = {
            target: self._selects(arguments, target)
            for target in (
                "//donner/gpu:gpu_tests",
                "//donner/gpu/metal/tests:metal_buffer_bounds_tests",
                "//donner/gpu_tools:tests",
                "//donner/svg/parser:parser_tests",
            )
        }
        self.assertEqual(
            selection,
            {
                "//donner/gpu:gpu_tests": False,
                "//donner/gpu/metal/tests:metal_buffer_bounds_tests": False,
                "//donner/gpu_tools:tests": True,
                "//donner/svg/parser:parser_tests": True,
            },
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
            for target in ("//donner/svg/parser:parser_tests", "//donner/gpu:gpu_tests"):
                with self.subTest(sanitizer=sanitizer, target=target):
                    self.assertEqual(sum(self._selects(command, target) for command in commands), 1)


if __name__ == "__main__":
    unittest.main()
