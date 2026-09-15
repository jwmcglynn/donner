"""Preserve the accumulated corpus if publishing a minimized generation fails."""

import sys
from pathlib import Path
from unittest import mock

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from manage_corpus import minimize_target
from run_continuous_fuzz import FuzzerTarget


def test_copy_failure_preserves_previous_corpus(tmp_path):
    binary = tmp_path / "fuzzer"
    binary.write_bytes(b"fuzzer")
    persistent = tmp_path / "persistent"
    old = persistent / "parser_fuzzer" / "old-seed"
    old.parent.mkdir(parents=True)
    old.write_bytes(b"preserved")
    run = tmp_path / "run"
    run.mkdir()
    (run / "new").write_bytes(b"new")
    target = FuzzerTarget(label="//donner/test:parser_fuzzer_bin", name="parser_fuzzer", binary_path=binary)

    def merge(command, **kwargs):
        (Path(command[2]) / "new-seed").write_bytes(b"new minimized seed")
        return mock.Mock(returncode=0, stderr="")

    with mock.patch("manage_corpus.subprocess.run", side_effect=merge), mock.patch("manage_corpus.shutil.copy2", side_effect=OSError("disk full")):
        with pytest.raises(OSError):
            minimize_target(target, run, persistent)
    assert old.read_bytes() == b"preserved"
