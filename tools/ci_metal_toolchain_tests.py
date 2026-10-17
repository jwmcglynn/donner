"""Control the Metal dependency setup without downloading or installing software."""
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
            code, text, output = responses.pop(0)
            if output:
                Path(argv[-1]).write_bytes(b"air")
            return subprocess.CompletedProcess(argv, code, stdout=text, stderr="")
        with patch.object(setup.subprocess, "run", side_effect=run):
            result = setup.ensure_compiler(install)
        return result, calls

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

    def test_post_install_failure_does_not_pass(self):
        with self.assertRaisesRegex(RuntimeError, "still cannot compile"):
            self.run_setup([(1, "missing Metal Toolchain", False), (0, "", False), (1, "still missing", False)])

    def test_timeout_reaches_failure_status(self):
        with patch.object(setup.subprocess, "run", side_effect=subprocess.TimeoutExpired("probe", 30)):
            with self.assertRaises(subprocess.TimeoutExpired):
                setup.ensure_compiler(True)


if __name__ == "__main__":
    unittest.main()
