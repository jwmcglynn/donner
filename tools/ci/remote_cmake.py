#!/usr/bin/env python3
"""Prepare an injected Bazel repository for offline Linux and macOS CMake consumer tests."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile


def run(args, cwd):
    subprocess.run(args, cwd=cwd, check=True)


def archive_tree(bundle, directory, prefix):
    """Store stable inputs only, without Git metadata or workspace output links."""
    for parent, dirs, files in os.walk(directory):
        dirs[:] = sorted(d for d in dirs if d != ".git")
        for name in sorted(dirs + files):
            path = Path(parent) / name
            relative = path.relative_to(directory)
            if name == ".git" or path.is_dir() and not path.is_symlink():
                continue
            if path.is_symlink():
                if relative == Path("external") and os.readlink(path) == "bazel-out/../../../external":
                    continue
                if path.readlink().is_absolute() or directory not in path.resolve().parents:
                    raise ValueError(f"Source link leaves its input tree: {relative}")
            info = bundle.gettarinfo(str(path), str(Path(prefix) / relative))
            info.uid = info.gid = info.mtime = 0
            info.uname = info.gname = ""
            if info.isfile():
                with path.open("rb") as contents:
                    bundle.addfile(info, contents)
            elif info.issym():
                bundle.addfile(info)
            else:
                raise ValueError(f"Unsupported input: {relative}")


def prepare(repo, destination):
    root = subprocess.check_output(["git", "rev-parse", "--show-toplevel"], cwd=repo, text=True).strip()
    if Path(root).resolve() != repo.resolve():
        raise ValueError("CMake inputs require the Donner checkout root")
    destination = destination.resolve()
    if destination.exists():
        raise FileExistsError(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=destination.parent) as scratch:
        work = Path(scratch)
        src, deps, result = work / "src", work / "deps", work / "result"
        src.mkdir()
        result.mkdir()
        # Export exactly the commit CI checked out. Ignore local files and Bazel output symlinks.
        source_tar = work / "source.tar"
        run(["git", "archive", "--output", str(source_tar), "HEAD"], repo)
        run(["tar", "-xf", str(source_tar)], src)
        # The tracked convenience link points into a Bazel output base, outside the source tree.
        external = src / "external"
        if external.is_symlink():
            external.unlink()
        output_base = work / "generator"
        startup = f"--output_base={output_base}"
        try:
            generator = [sys.executable, "tools/cmake/gen_cmakelists.py", f"--bazel-startup-arg={startup}"]
            run([*generator, "--check"], src)
            run(generator, src)
        finally:
            run(["bazel", startup, "shutdown"], src)
        for link in src.glob("bazel-*"):
            if link.is_symlink():
                link.unlink()
        run(["cmake", "-S", "examples/cmake_consumer", "-B", str(work / "prepare"), "-G", "Ninja",
             f"-DDONNER_SOURCE_DIR={src}", f"-DFETCHCONTENT_BASE_DIR={deps}"], src)
        with tarfile.open(result / "inputs.tar", "w", dereference=False) as bundle:
            archive_tree(bundle, src, "src")
            for tree in sorted(deps.glob("*-src")):
                archive_tree(bundle, tree, f"deps/{tree.name}")
        shutil.copy2(repo / "tools/ci/cmake_remote_worker.sh", result / "worker.sh")
        (result / "MODULE.bazel").write_text('module(name = "donner_cmake_inputs")\n')
        (result / "BUILD.bazel").write_text(
            'load("@@//tools/ci:cmake_remote.bzl", "cmake_remote_consumers")\ncmake_remote_consumers()\n')
        result.rename(destination)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    prepare(Path(__file__).resolve().parents[2], args.destination)
