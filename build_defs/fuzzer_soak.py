"""Run libFuzzer against a writable copy of the declared seed corpus."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main(arguments):
    binary, *arguments = arguments
    flags = [arg for arg in arguments if arg.startswith("-")]
    seeds = [arg for arg in arguments if not arg.startswith("-")]
    with tempfile.TemporaryDirectory(dir=os.environ.get("TEST_TMPDIR")) as directory:
        corpus = Path(directory) / "corpus"
        corpus.mkdir()
        for index, seed in enumerate(seeds):
            shutil.copyfile(seed, corpus / str(index))
        artifacts = Path(os.environ.get("TEST_UNDECLARED_OUTPUTS_DIR", directory)) / "findings"
        artifacts.mkdir(parents=True, exist_ok=True)
        result = subprocess.run(
            [binary, *flags, "-artifact_prefix=" + str(artifacts) + "/", str(corpus)],
            check=False,
        )
        return result.returncode if result.returncode >= 0 else 128 - result.returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
