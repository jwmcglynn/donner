"""Qualify a relocated CMake consumer's complete and missing-resource behavior."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def run(binary, working_directory):
    return subprocess.run(
        [str(binary)], cwd=working_directory,
        env={"PATH": os.defpath, "LC_ALL": "C"},
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30, check=False,
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--assets", type=Path, required=True)
    args = parser.parse_args()
    if not args.binary.is_file() or not (args.assets / "manifest.json").is_file():
        raise ValueError("A built consumer and generated resource tree are required")
    with tempfile.TemporaryDirectory(prefix="donner-cmake-resource-test-") as temporary:
        root = Path(temporary)
        unrelated = root / "unrelated"
        unrelated.mkdir()
        complete = root / "complete"
        complete.mkdir()
        executable = complete / args.binary.name
        shutil.copy2(args.binary, executable)
        shutil.copytree(args.assets, complete / "donner-assets", symlinks=True)
        result = run(executable, unrelated)
        if result.returncode != 0:
            raise RuntimeError(f"Relocated packaged consumer failed: {result.stdout!r}")
        missing = root / "missing"
        missing.mkdir()
        executable = missing / args.binary.name
        shutil.copy2(args.binary, executable)
        result = run(executable, unrelated)
        if result.returncode != 1 or b"Packaged resources unavailable:" not in result.stdout:
            raise RuntimeError(f"Missing assets did not fail explicitly: {result.stdout!r}")
    print("Relocated text rendering and explicit missing-resource refusal passed")


if __name__ == "__main__":
    main()
