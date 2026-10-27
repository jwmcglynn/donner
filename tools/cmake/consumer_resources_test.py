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


def require_resource_refusal(result, context):
    if result.returncode != 1 or b"Packaged resources unavailable:" not in result.stdout:
        raise RuntimeError(f"{context} did not fail explicitly: {result.stdout!r}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--cmake", required=True)
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
        stale = complete / "donner-assets" / "assets" / ("0" * 64 + ".otf")
        stale.write_bytes(b"obsolete asset from a previous build")
        result = run(executable, unrelated)
        require_resource_refusal(result, "Stale resources")
        subprocess.run(
            [args.cmake, f"-DSOURCE={args.assets.resolve()}",
             f"-DDESTINATION={complete / 'donner-assets'}", "-P",
             str(Path(__file__).with_name("stage_consumer_resources.cmake"))],
            timeout=30, check=True,
        )
        result = run(executable, unrelated)
        if stale.exists() or result.returncode != 0:
            raise RuntimeError(f"Incremental resource replacement failed: {result.stdout!r}")
        missing = root / "missing"
        missing.mkdir()
        executable = missing / args.binary.name
        shutil.copy2(args.binary, executable)
        result = run(executable, unrelated)
        require_resource_refusal(result, "Missing resources")
    print("Relocated rendering, incremental asset replacement and missing-resource refusal passed")


if __name__ == "__main__":
    main()
