"""Check seed isolation and failure propagation around the real fuzzer process."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import fuzzer_soak


class FuzzerSoakTest(unittest.TestCase):
    def test_same_basename_seeds_are_copied_without_modifying_sources(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            seeds = []
            for directory, content in [("a space", b"first"), ("b", b"second")]:
                parent = root / directory
                parent.mkdir()
                seed = parent / "seed"
                seed.write_bytes(content)
                seeds.append(str(seed))

            def run(command, check):
                self.assertFalse(check)
                self.assertEqual(command[:2], ["fuzzer", "-runs=128"])
                corpus = Path(command[-1])
                self.assertEqual(sorted(p.read_bytes() for p in corpus.iterdir()),
                                 [b"first", b"second"])
                (corpus / "new-finding").write_bytes(b"generated")
                return subprocess.CompletedProcess(command, 70)

            with patch("fuzzer_soak.subprocess.run", side_effect=run):
                self.assertEqual(fuzzer_soak.main(["fuzzer", "-runs=128", *seeds]), 70)
            self.assertEqual([Path(seed).read_bytes() for seed in seeds], [b"first", b"second"])
            self.assertEqual(sorted((root / "a space").iterdir()), [Path(seeds[0])])

    def test_empty_corpus_and_signal_exit_preserve_findings(self):
        with tempfile.TemporaryDirectory() as outputs:
            def run(command, check):
                self.assertEqual(list(Path(command[-1]).iterdir()), [])
                prefix = next(arg.split("=", 1)[1] for arg in command
                              if arg.startswith("-artifact_prefix="))
                Path(prefix + "crash").write_bytes(b"repro")
                return subprocess.CompletedProcess(command, -6)

            with patch.dict(os.environ, {"TEST_UNDECLARED_OUTPUTS_DIR": outputs}):
                with patch("fuzzer_soak.subprocess.run", side_effect=run):
                    self.assertEqual(fuzzer_soak.main(["fuzzer"]), 134)
            self.assertEqual((Path(outputs) / "findings/crash").read_bytes(), b"repro")


if __name__ == "__main__":
    unittest.main()
