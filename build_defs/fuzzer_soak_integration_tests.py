"""Prove the generated soak target runs libFuzzer mutation, not file replay."""

import subprocess
import sys
import unittest


class FuzzerSoakIntegrationTest(unittest.TestCase):
    def test_seeded_mutation(self):
        result = subprocess.run(
            [SOAK, "-runs=128", "-seed=1"],
            capture_output=True, text=True, check=False, timeout=45,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("probe seed replayed", result.stderr)
        self.assertRegex(result.stderr, r"#128\s+DONE")
        self.assertNotIn("fuzzing was not performed", result.stderr)


if __name__ == "__main__":
    SOAK = sys.argv.pop(1)
    unittest.main()
