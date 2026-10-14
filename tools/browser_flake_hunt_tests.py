"""The browser flake-hunt script validates before it deletes, serves the package it was given,
summarizes Playwright's report and leaves no server behind.

The script runs for real against the real package server, with a stub `npx` in front of
Playwright: the stub records how it was called, downloads the served editor.wasm, writes the JSON
report the test supplies and exits with the status the test asks for.
"""

import json
import os
from pathlib import Path
import select
import shutil
import socket
import subprocess
import tempfile
import unittest
from urllib.parse import urlsplit

from python.runfiles import runfiles
from test_executable_fixtures import ExecutableFixtureDirectory

kAnsiRed = "\x1b[31m"
kAnsiReset = "\x1b[39m"


def report_with(results_by_title):
    """A Playwright JSON report with one nested describe suite and the given results per case."""
    specs = []
    for title, results in results_by_title.items():
        specs.append({
            "title": title,
            "tests": [{"projectName": "fixture-project", "results": results}],
        })
    return {
        "suites": [{
            "title": "fixture.spec.ts",
            "specs": [],
            "suites": [{"title": "group", "specs": specs, "suites": []}],
        }],
    }


class BrowserFlakeHuntTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.executables = ExecutableFixtureDirectory()
        cls.addClassCleanup(cls.executables.cleanup)
        cls.executables.write(
            "npx",
            '#!/bin/bash\n'
            'printf "%s\\n" "$@" > "$FIXTURE_RECORD/args"\n'
            'printf "%s\\n" "$DONNER_WASM_BASE_URL" > "$FIXTURE_RECORD/url"\n'
            'printf "%s\\n" "$DONNER_WASM_PACKAGE_DIR" > "$FIXTURE_RECORD/package-dir"\n'
            'curl -fsS -o "$FIXTURE_RECORD/served.wasm" "$DONNER_WASM_BASE_URL/editor.wasm"\n'
            'if [[ -n "$FIXTURE_HANG" ]]; then\n'
            '  echo $$ > "$FIXTURE_RECORD/npx.fifo"\n'
            '  exec sleep 300\n'
            'fi\n'
            'if [[ -n "$FIXTURE_REPORT" ]]; then\n'
            '  cp "$FIXTURE_REPORT" "$PLAYWRIGHT_JSON_OUTPUT_NAME"\n'
            'fi\n'
            'exit "$FIXTURE_STATUS"\n',
        )

    def setUp(self):
        self.root = Path(tempfile.mkdtemp(dir=os.environ.get("TEST_TMPDIR")))
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)
        resolver = runfiles.Create()
        (self.root / "tools/http").mkdir(parents=True)
        shutil.copyfile(
            resolver.Rlocation("donner/tools/browser-flake-hunt.sh"),
            self.root / "tools/browser-flake-hunt.sh",
        )
        shutil.copyfile(
            resolver.Rlocation("donner/tools/http/simple_webserver.py"),
            self.root / "tools/http/simple_webserver.py",
        )
        # Installed test dependencies, so the script does not run npm ci.
        (self.root / "donner/editor/wasm/tests/node_modules").mkdir(parents=True)
        self.package = self.root / "package-in"
        self.package.mkdir()
        (self.package / "index.html").write_text("<!doctype html>\n", encoding="utf-8")
        (self.package / "editor.wasm").write_bytes(b"\0asm fixture bytes")
        self.record = self.root / "record"
        self.record.mkdir()

    def environment(self, status=0, report_path="", hang=False):
        environment = dict(os.environ)
        environment.update({
            "PATH": f"{self.executables.path}{os.pathsep}{environment.get('PATH', '')}",
            "FIXTURE_RECORD": str(self.record),
            "FIXTURE_REPORT": report_path,
            "FIXTURE_STATUS": str(status),
            "FIXTURE_HANG": "1" if hang else "",
        })
        return environment

    def run_script(self, *args, status=0, report=None):
        report_path = ""
        if report is not None:
            report_path = str(self.root / "fixture-report.json")
            Path(report_path).write_text(json.dumps(report), encoding="utf-8")
        return subprocess.run(
            ["bash", str(self.root / "tools/browser-flake-hunt.sh"), *args],
            capture_output=True,
            text=True,
            env=self.environment(status, report_path),
            timeout=60,
        )

    def assert_refused(self, result, message):
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn(message, result.stderr)
        self.assertFalse((self.record / "args").exists(), "Playwright must not have started")

    def test_invalid_arguments_are_refused_before_anything_runs(self):
        out = str(self.root / "out")
        package = str(self.package)
        cases = [
            (["--package", package, "--out"], "--out needs a value"),
            (["--package", package, "--out", out, "--repeat", "0"], "--repeat must be"),
            (["--package", package, "--out", out, "--cpu-load", "010"], "--cpu-load must be"),
            (["--package", package, "--out", out, "--cpu-load", "-1"], "--cpu-load must be"),
            (["--package", package, "--out", "/"], "--out must not be the root directory"),
            (["--out", out], "--package and --out are required"),
        ]
        for args, message in cases:
            with self.subTest(args=args):
                self.assert_refused(self.run_script(*args), message)

    def test_a_package_inside_the_replaced_copy_is_kept(self):
        out = self.root / "out"
        nested = out / "package" / "build"
        nested.mkdir(parents=True)
        shutil.copytree(self.package, nested, dirs_exist_ok=True)
        result = self.run_script("--package", str(nested), "--out", str(out))
        self.assert_refused(result, "--package must not be in")
        self.assertTrue((nested / "editor.wasm").exists())

    def test_an_output_directory_inside_the_package_is_refused(self):
        result = self.run_script(
            "--package", str(self.package), "--out", str(self.package / "out")
        )
        self.assert_refused(result, "--out must not be in the package")

    def test_serves_the_package_and_summarizes_failures(self):
        report = report_with({
            "passes": [{"status": "passed"}, {"status": "passed"}],
            "flakes": [
                {"status": "passed"},
                {
                    "status": "failed",
                    "errors": [
                        {"message": f"{kAnsiRed}Error: the frame never came{kAnsiReset}\nmore"}
                    ],
                    "attachments": [{"path": "/results/case-repeat1/trace.zip"}],
                },
            ],
        })
        out = self.root / "out"
        result = self.run_script(
            "--package", str(self.package), "--out", str(out), "--repeat", "2", "--grep", "case",
            status=1, report=report,
        )
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)

        args = (self.record / "args").read_text(encoding="utf-8").splitlines()
        for expected in ("playwright", "test", "--repeat-each=2", "--retries=0", "--workers=1",
                         "--grep=case", "--headed"):
            self.assertIn(expected, args)
        self.assertEqual((self.record / "served.wasm").read_bytes(), b"\0asm fixture bytes")
        package_dir = Path((self.record / "package-dir").read_text(encoding="utf-8").strip())
        self.assertEqual(package_dir, out.resolve() / "package")
        self.assertEqual((package_dir / "editor.wasm").read_bytes(), b"\0asm fixture bytes")

        summary = (out / "summary.txt").read_text(encoding="utf-8")
        self.assertIn("runs: 4 in ", summary)
        self.assertIn("{'passed': 2}  fixture.spec.ts > group > passes [fixture-project]", summary)
        self.assertIn(
            "{'passed': 1, 'failed': 1}  fixture.spec.ts > group > flakes [fixture-project]",
            summary,
        )
        self.assertIn("failures: 1", summary)
        self.assertIn("    Error: the frame never came\n", summary)
        self.assertNotIn("\x1b", summary)
        self.assertIn("    output: /results/case-repeat1", summary)

    def test_the_package_server_is_stopped_when_the_script_ends(self):
        report = report_with({"passes": [{"status": "passed"}]})
        result = self.run_script(
            "--package", str(self.package), "--out", str(self.root / "out"), report=report
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        url = urlsplit((self.record / "url").read_text(encoding="utf-8").strip())
        with self.assertRaises(ConnectionRefusedError):
            socket.create_connection((url.hostname, url.port), timeout=5).close()

    def test_termination_stops_playwright_and_the_server(self):
        fifo = self.record / "npx.fifo"
        os.mkfifo(fifo)
        # Opened before the script starts, so the stub's write waits for this reader.
        reader = os.open(fifo, os.O_RDONLY | os.O_NONBLOCK)
        self.addCleanup(os.close, reader)
        script = subprocess.Popen(
            ["bash", str(self.root / "tools/browser-flake-hunt.sh"), "--package", str(self.package),
             "--out", str(self.root / "out")],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            env=self.environment(hang=True),
        )
        self.addCleanup(script.kill)
        # The stub writes its pid once Playwright would be running. The bound only turns a script
        # that never gets there into a failure instead of a hang.
        readable, _, _ = select.select([reader], [], [], 30)
        self.assertEqual(readable, [reader], "the stub never started")
        npx_pid = int(os.read(reader, 64).decode().strip())
        script.terminate()
        self.assertEqual(script.wait(timeout=30), 143)
        with self.assertRaises(ProcessLookupError):
            os.kill(npx_pid, 0)
        url = urlsplit((self.record / "url").read_text(encoding="utf-8").strip())
        with self.assertRaises(ConnectionRefusedError):
            socket.create_connection((url.hostname, url.port), timeout=5).close()

    def test_a_missing_report_fails_with_the_playwright_status(self):
        result = self.run_script(
            "--package", str(self.package), "--out", str(self.root / "out"), status=3
        )
        self.assertEqual(result.returncode, 3, result.stdout + result.stderr)
        self.assertIn("Playwright wrote no report", result.stderr)
        self.assertFalse((self.root / "out/summary.txt").exists())


if __name__ == "__main__":
    unittest.main()
