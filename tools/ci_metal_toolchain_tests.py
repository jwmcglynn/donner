"""Control the Metal dependency setup without downloading or installing software."""
import json
import subprocess
import unittest
from pathlib import Path
from unittest.mock import patch

from tools import ci_metal_toolchain as setup


class MetalSetupTests(unittest.TestCase):
    def run_setup(self, responses, install=True):
        calls = []
        def run(argv, **kwargs):
            calls.append((argv, kwargs))
            code, text, output, *errors = responses.pop(0)
            if output:
                Path(argv[-1]).write_bytes(b"air")
            return subprocess.CompletedProcess(argv, code, stdout=text, stderr=errors[0] if errors else "")
        with patch.object(setup.subprocess, "run", side_effect=run):
            result = setup.ensure_compiler(install)
        return result, calls

    def test_component_receipt_omits_paths_and_private_fields(self):
        data = {"status": "installed", "buildVersion": "27A266a",
                "toolchainIdentifier": "com.apple.dt.toolchain.Metal.32023.921.5",
                "assetPath": "/private/asset", "toolchainSearchPath": "/private/mount",
                "other": "private-extra-field"}
        response = subprocess.CompletedProcess([], 0, json.dumps(data), "")
        with patch.object(setup.subprocess, "run", return_value=response) as run:
            self.assertEqual(setup.component_status(), {
                "status": "installed", "buildVersion": "27A266a",
                "toolchainIdentifier": "com.apple.dt.toolchain.Metal.32023.921.5"})
        self.assertEqual(run.call_args.args[0],
                         ["/usr/bin/xcodebuild", "-showComponent", "MetalToolchain", "-json"])
        self.assertEqual(run.call_args.kwargs["timeout"], 30)

    def test_component_receipt_rejects_unrecognized_field_values(self):
        response = subprocess.CompletedProcess([], 0, json.dumps({
            "status": "private-user", "buildVersion": "/private/build",
            "toolchainIdentifier": "private-host"}), "")
        with patch.object(setup.subprocess, "run", return_value=response):
            self.assertEqual(setup.component_status(), {"status": "unrecognized"})

    def test_component_receipt_failures_never_echo_raw_output(self):
        cases = [(subprocess.CompletedProcess([], 4, "/private/output", "/private/error"),
                  {"status": "query-failed", "exit_code": 4}),
                 (subprocess.CompletedProcess([], 0, "not JSON /private/output", ""),
                  {"status": "invalid-response"}),
                 (subprocess.TimeoutExpired("query", 30, output=b"/private/partial"),
                  {"status": "query-timeout", "timeout_seconds": 30})]
        for response, expected in cases:
            with self.subTest(expected=expected):
                effect = response if isinstance(response, Exception) else None
                with patch.object(setup.subprocess, "run", return_value=response, side_effect=effect):
                    self.assertEqual(setup.component_status(), expected)

    def test_existing_compiler_does_not_install(self):
        installed, calls = self.run_setup([(0, "", True)])
        self.assertFalse(installed)
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0][1]["timeout"], 30)

    def test_missing_component_is_prepared_then_compiled(self):
        installed, calls = self.run_setup([(1, "missing Metal Toolchain", False), (0, "", False), (0, "", True)])
        self.assertTrue(installed)
        self.assertEqual(calls[1][0], ["/usr/bin/xcodebuild", "-downloadComponent", "MetalToolchain"])
        self.assertEqual([item[1]["timeout"] for item in calls], [30, 600, 30])

    def test_check_only_never_installs(self):
        with self.assertRaisesRegex(RuntimeError, "probe failed"):
            self.run_setup([(1, "missing Metal Toolchain", False)], install=False)

    def test_unrelated_probe_error_does_not_install(self):
        with self.assertRaisesRegex(RuntimeError, "other failure"):
            self.run_setup([(1, "other failure", False)])

    def test_exit_zero_without_air_does_not_pass(self):
        with self.assertRaisesRegex(RuntimeError, "probe failed"):
            self.run_setup([(0, "", False)])

    def test_failed_download_does_not_pass(self):
        with self.assertRaisesRegex(RuntimeError, "download failed"):
            self.run_setup([(1, "missing Metal Toolchain", False), (1, "unavailable", False)])

    def test_post_install_failure_keeps_exit_status_and_both_streams(self):
        with self.assertRaisesRegex(RuntimeError, "still cannot compile") as caught:
            self.run_setup([(1, "missing Metal Toolchain", False), (0, "", False),
                            (23, "compiler stdout", False, "compiler stderr")])
        for expected in ("exit=23", "AIR=missing", "compiler stdout", "compiler stderr"):
            self.assertIn(expected, str(caught.exception))

    def test_post_install_exit_zero_without_air_reports_missing_output(self):
        with self.assertRaisesRegex(RuntimeError, "exit=0, AIR=missing"):
            self.run_setup([(1, "missing Metal Toolchain", False), (0, "", False), (0, "", False)])

    def test_timeout_retains_partial_compiler_output(self):
        error = subprocess.TimeoutExpired("probe", 30, output=b"partial stdout", stderr=b"partial stderr")
        with patch.object(setup.subprocess, "run", side_effect=error):
            with self.assertRaisesRegex(RuntimeError, "timed out after 30s") as caught:
                setup.ensure_compiler(True)
        self.assertIn("partial stdout", str(caught.exception))
        self.assertIn("partial stderr", str(caught.exception))

    def test_download_timeout_retains_partial_output(self):
        responses = [subprocess.CompletedProcess([], 1, "missing Metal Toolchain", ""),
                     subprocess.TimeoutExpired("download", 600, stderr=b"catalog timeout detail")]
        with patch.object(setup.subprocess, "run", side_effect=responses):
            with self.assertRaisesRegex(RuntimeError, "download timed out after 600s: catalog timeout detail"):
                setup.ensure_compiler(True)


if __name__ == "__main__":
    unittest.main()
