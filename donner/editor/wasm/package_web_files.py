"""Copy application files and immutable asset trees into one collision-checked web bundle."""

import argparse
import os
from pathlib import Path, PurePosixPath
import shutil


def _add_input(inputs: dict[str, Path], relative: str, source: Path) -> None:
    path = PurePosixPath(relative)
    if (
        path.is_absolute()
        or ".." in path.parts
        or path.as_posix() != relative
        or "\\" in relative
        or any(ord(character) < 32 for character in relative)
    ):
        raise ValueError(f"Invalid web package path: {relative!r}")
    if relative in inputs:
        raise ValueError(f"Duplicate web package path: {relative}")
    if not source.is_file():
        raise ValueError(f"Web package input is not a file: {source}")
    inputs[relative] = source


def _add_tree(inputs: dict[str, Path], tree: Path) -> None:
    if not tree.is_dir():
        raise ValueError(f"Web package asset tree is not a directory: {tree}")
    for parent, directories, names in os.walk(tree, followlinks=False):
        directories.sort()
        for name in sorted(directories + names):
            source = Path(parent) / name
            if source.is_symlink():
                raise ValueError(f"Symlink in web package asset tree: {source}")
        for name in sorted(names):
            source = Path(parent) / name
            _add_input(inputs, source.relative_to(tree).as_posix(), source)


def _copy_inputs(output: Path, inputs: dict[str, Path]) -> None:
    if output.is_symlink():
        raise ValueError("Web package output must not be a symlink")
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise ValueError("Web package output must be empty")
    for relative, source in sorted(inputs.items()):
        destination = output / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        if destination.exists() or destination.is_symlink():
            raise ValueError(f"Web package output already exists: {relative}")
        shutil.copyfile(source, destination)


def package_files(output: Path, files: list[Path], trees: list[Path]) -> None:
    """Validate the complete destination map before writing any bundle content."""
    inputs: dict[str, Path] = {}
    for source in files:
        _add_input(inputs, source.name, source)
    for tree in trees:
        _add_tree(inputs, tree)
    for relative in inputs:
        for parent in PurePosixPath(relative).parents:
            if parent.as_posix() in inputs:
                raise ValueError(f"File/directory collision in web package: {relative}")
    _copy_inputs(output, inputs)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--file", type=Path, action="append", default=[])
    parser.add_argument("--tree", type=Path, action="append", default=[])
    arguments = parser.parse_args()
    package_files(arguments.output, arguments.file, arguments.tree)


if __name__ == "__main__":
    main()
