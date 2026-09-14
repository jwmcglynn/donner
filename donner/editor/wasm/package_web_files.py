"""Copy explicitly declared application files and assets into a collision-checked web bundle."""

import argparse
from pathlib import Path, PurePosixPath
import shutil


def _add_input(inputs: dict[str, Path], relative: str, source: Path) -> None:
    path = PurePosixPath(relative)
    if (
        not path.parts
        or path.is_absolute()
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


def package_files(output: Path, files: list[Path], assets: list[tuple[str, Path]]) -> None:
    """Validate the complete destination map before writing any bundle content."""
    inputs: dict[str, Path] = {}
    for source in files:
        _add_input(inputs, source.name, source)
    for relative, source in assets:
        _add_input(inputs, relative, source)
    for relative in inputs:
        for parent in PurePosixPath(relative).parents:
            if parent.as_posix() in inputs:
                raise ValueError(f"File/directory collision in web package: {relative}")
    _copy_inputs(output, inputs)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--file", type=Path, action="append", default=[])
    parser.add_argument("--asset", nargs=2, metavar=("RELATIVE", "SOURCE"), action="append", default=[])
    arguments = parser.parse_args()
    assets = [(relative, Path(source)) for relative, source in arguments.asset]
    package_files(arguments.output, arguments.file, assets)


if __name__ == "__main__":
    main()
