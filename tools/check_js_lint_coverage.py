#!/usr/bin/env python3
"""Fails when a tracked JavaScript file is missing from the JavaScript lint test.

`//donner/editor/wasm/tests:js_lint_test` runs CodeFactor's ESLint rules on the files in its
`data`: a glob of its own package plus explicit labels for files in other packages, because a
Bazel glob cannot reach into other packages. A new JavaScript file elsewhere would otherwise go
unlinted without any message. This check compares every tracked `*.js`, `*.jsx`, `*.es6` and
`*.mjs` file outside `third_party/` (the files CodeFactor analyzes) with the files the test
declares, and reports any file that is missing or any declared file that is not tracked.

It reads `git ls-files`, so the Lint lane runs it (tools/ci/lint_lane.sh) rather than
`bazel test //...`.

Usage: python3 tools/check_js_lint_coverage.py [--root DIR]
"""

import argparse
import ast
import fnmatch
from pathlib import Path, PurePosixPath
import subprocess
import sys

LINT_PACKAGE = "donner/editor/wasm/tests"
LINT_TARGET = "js_lint_test"
_SUFFIXES = (".js", ".jsx", ".es6", ".mjs")
_EXCLUDED_PREFIX = "third_party/"


class CoverageError(Exception):
    """The lint target's declaration could not be read."""


def tracked_sources(root):
    """Tracked JavaScript paths outside third_party/, as posix strings."""
    output = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z", "--", *("*" + s for s in _SUFFIXES)],
        check=True, capture_output=True).stdout
    paths = (item.decode("utf-8", "surrogateescape") for item in output.split(b"\0") if item)
    return {path for path in paths
            if path.endswith(_SUFFIXES) and not path.startswith(_EXCLUDED_PREFIX)}


def _lint_call(build_text):
    for node in ast.walk(ast.parse(build_text)):
        if not isinstance(node, ast.Call):
            continue
        for keyword in node.keywords:
            if (keyword.arg == "name" and isinstance(keyword.value, ast.Constant)
                    and keyword.value.value == LINT_TARGET):
                return node
    raise CoverageError(f"no target named {LINT_TARGET} in {LINT_PACKAGE}/BUILD.bazel")


def _is_string(node):
    return isinstance(node, ast.Constant) and isinstance(node.value, str)


def _is_glob(expression):
    return (isinstance(expression, ast.Call) and isinstance(expression.func, ast.Name)
            and expression.func.id == "glob" and len(expression.args) == 1
            and not expression.keywords)


def _list_labels(expression):
    for element in expression.elts:
        if not _is_string(element):
            raise CoverageError(f"unsupported data entry: {ast.dump(element)}")
        yield ("label", element.value)


def _data_items(expression):
    """Yields ("glob", patterns) and ("label", label) items from a `data` expression."""
    if isinstance(expression, ast.BinOp) and isinstance(expression.op, ast.Add):
        yield from _data_items(expression.left)
        yield from _data_items(expression.right)
    elif isinstance(expression, ast.List):
        yield from _list_labels(expression)
    elif _is_glob(expression):
        yield ("glob", _literal_patterns(expression.args[0]))
    else:
        raise CoverageError(f"unsupported data expression: {ast.dump(expression)}")


def _literal_patterns(expression):
    if not isinstance(expression, ast.List) or not all(map(_is_string, expression.elts)):
        raise CoverageError("glob() must take a literal list of patterns")
    return [element.value for element in expression.elts]


def _label_path(label):
    """The source path a file label names, or None for a label that is not a source file."""
    if label.startswith("//"):
        package, _, name = label[2:].partition(":")
    elif label.startswith(":"):
        package, name = LINT_PACKAGE, label[1:]
    else:
        package, name = LINT_PACKAGE, label
    if not name.endswith(_SUFFIXES):
        return None
    return str(PurePosixPath(package) / name) if package else name


def _glob_matches(patterns, tracked):
    """The tracked files directly in the lint package that a package glob of `patterns` names."""
    for path in tracked:
        parent, _, name = path.rpartition("/")
        if parent == LINT_PACKAGE and any(fnmatch.fnmatchcase(name, p) for p in patterns):
            yield path


def _lint_data(root):
    build = Path(root) / LINT_PACKAGE / "BUILD.bazel"
    call = _lint_call(build.read_text(encoding="utf-8"))
    data = next((k.value for k in call.keywords if k.arg == "data"), None)
    if data is None:
        raise CoverageError(f"{LINT_TARGET} declares no data")
    return data


def declared_sources(root, tracked):
    """The tracked files that js_lint_test's `data` includes."""
    declared = set()
    for kind, value in _data_items(_lint_data(root)):
        if kind == "glob":
            declared.update(_glob_matches(value, tracked))
        elif (path := _label_path(value)) is not None:
            declared.add(path)
    return declared


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    args = parser.parse_args(argv)
    try:
        tracked = tracked_sources(args.root)
        declared = declared_sources(args.root, tracked)
    except (CoverageError, OSError, SyntaxError, subprocess.CalledProcessError) as error:
        print(f"check_js_lint_coverage: {error}", file=sys.stderr)
        return 1
    missing = sorted(tracked - declared)
    untracked = sorted(declared - tracked)
    for path in missing:
        print(f"{path}: not linted; add it to //{LINT_PACKAGE}:{LINT_TARGET} data "
              "(export it from its package if needed)", file=sys.stderr)
    for path in untracked:
        print(f"{path}: listed in //{LINT_PACKAGE}:{LINT_TARGET} but not a tracked file",
              file=sys.stderr)
    if missing or untracked:
        return 1
    print(f"check_js_lint_coverage: {len(tracked)} JavaScript files, all linted")
    return 0


if __name__ == "__main__":
    sys.exit(main())
