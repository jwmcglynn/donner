import os
import subprocess
import unittest

from python.runfiles import runfiles


class BinarySizeToolTest(unittest.TestCase):
    """Smoke test for the binary-size report script.

    binary_size.sh needs Bazel, bloaty, and emcc to run end-to-end, so this
    test does not execute it. It confirms the script parses cleanly with
    `bash -n`, which catches syntax regressions and, just as importantly, gives
    the size tooling a real Bazel test target so a change to it maps to a test
    under affected-target CI rather than to a non-test filegroup.
    """

    def test_script_parses(self):
        r = runfiles.Create()
        location = r.Rlocation("donner/tools/binary_size.sh")
        self.assertIsNotNone(location, "binary_size.sh not found in runfiles")
        self.assertTrue(os.path.exists(location), location)
        result = subprocess.run(
            ["bash", "-n", location],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
