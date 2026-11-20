#!/usr/bin/env python3
"""Checks that the text tier macros stay private to the libraries that implement text.

`DONNER_TEXT_ENABLED`, `DONNER_TEXT_FULL` and `DONNER_TEXT_WOFF2_ENABLED` follow the
`//donner/svg/renderer:text` and `:text_full` settings. Set through `defines` (or a `-D` copt),
a macro reaches the compile command of every dependent library, so each text configuration
recompiles them all even when they never test it; tested in a header, it changes declarations
between translation units that see it and ones that do not. This check allows the macros only in
the `local_defines` of the reviewed targets in REVIEWED_LOCAL_DEFINES and in those targets' own
sources. It rejects them in any header, in any other C or C++ source, in `.bazelrc` files and in
every other BUILD or .bzl form, and it rejects reviewed entries that no longer match a target
setting a macro. Everything else asks `donner/svg/text/TextTier.h` at run time.

Usage: python3 build_defs/check_text_tier_defines.py [FILE...]
With no arguments it checks every tracked file it governs; run it from the repository root.
Given files, it checks those, and the reviewed entries of any given BUILD file.
"""

import ast
import os
import posixpath
import re
import subprocess
import sys

# Targets whose own sources test a text macro, by (BUILD file, target name), with the reason.
REVIEWED_LOCAL_DEFINES = {
    # The runtime query itself.
    ("donner/svg/text/BUILD.bazel", "text_tier"),
    # Text implementation: the code compiled into each tier differs.
    ("donner/svg/text/BUILD.bazel", "text_backend_simple"),
    ("donner/svg/text/BUILD.bazel", "text_engine"),
    ("donner/svg/components/shape/BUILD.bazel", "shape_system"),
    ("donner/svg/resources/BUILD.bazel", "font_manager"),
    ("donner/svg/renderer/BUILD.bazel", "render_snapshot"),
    ("donner/svg/renderer/BUILD.bazel", "renderer_driver"),
    ("donner/svg/renderer/BUILD.bazel", "rendering_context"),
    ("donner/svg/renderer/BUILD.bazel", "renderer_geode"),
    ("donner/svg/renderer/BUILD.bazel", "renderer_tiny_skia"),
    # The Wasm bridges export `donner_prepare_resources` only when text is built in.
    ("donner/svg/renderer/wasm/BUILD.bazel", "donner_wasm_bin"),
    ("donner/svg/renderer/wasm/BUILD.bazel", "donner_wasm_geode_bin"),
    # Tests and fuzzers that call a library only the full text configuration links.
    ("donner/svg/text/BUILD.bazel", "font_decoder_fuzzer"),
    ("donner/svg/renderer/tests/BUILD.bazel", "font_synthesis_snapshot_tests"),
}
_MACRO = re.compile(r"DONNER_TEXT_(?:ENABLED|FULL|WOFF2_ENABLED)(?![A-Za-z0-9_])")


def _macro_strings(node):
    """String constants under `node` that name a text tier macro."""
    return [child for child in ast.walk(node)
            if isinstance(child, ast.Constant) and isinstance(child.value, str)
            and _MACRO.search(child.value)]


def _target_name(call):
    for keyword in call.keywords:
        if keyword.arg == "name" and isinstance(keyword.value, ast.Constant):
            return str(keyword.value.value)
    return ""


def _problem(path, call, attribute):
    """The problem with a text macro in `attribute` of `call`, or None."""
    target = _target_name(call)
    if attribute == "local_defines" and (path, target) in REVIEWED_LOCAL_DEFINES:
        return None
    if attribute == "local_defines":
        return ("%s sets a text tier macro without a reviewed reason in "
                "build_defs/check_text_tier_defines.py; query donner/svg/text/TextTier.h at run "
                "time instead" % (target or "a rule"))
    return ("%s passes a text tier macro through `%s`, which reaches dependents' compile commands; "
            "use `local_defines` on a reviewed text implementation target, or query "
            "donner/svg/text/TextTier.h at run time" % (target or "a rule", attribute))


def _attribute_problems(path, tree, seen):
    """Problems of macros passed as rule attributes; records each macro constant in `seen`."""
    problems = []
    for call in (node for node in ast.walk(tree) if isinstance(node, ast.Call)):
        for keyword in call.keywords:
            constants = _macro_strings(keyword.value)
            seen.update(id(constant) for constant in constants)
            problem = _problem(path, call, keyword.arg or "**kwargs") if constants else None
            problems += ["%s:%d: %s" % (path, constant.lineno, problem)
                         for constant in constants if problem]
    return problems


def file_problems(path, text):
    """Problems of one BUILD or .bzl file text."""
    try:
        tree = ast.parse(text, filename=path)
    except SyntaxError as error:
        return ["%s:%s: cannot parse: %s" % (path, error.lineno, error.msg)]
    seen = set()
    problems = _attribute_problems(path, tree, seen)
    # Anything else, such as `kwargs["defines"] = [...]` in a macro, is not a reviewed form.
    problems += ["%s:%d: text tier macro outside a reviewed `local_defines`; query "
                 "donner/svg/text/TextTier.h at run time" % (path, constant.lineno)
                 for constant in _macro_strings(tree) if id(constant) not in seen]
    return problems


_HEADER_SUFFIXES = (".h", ".hh", ".hpp", ".inc")
_SOURCE_SUFFIXES = (".c", ".cc", ".cpp", ".m", ".mm")


def _rule_call(tree, name):
    """The rule call named `name` in a parsed BUILD file, or None."""
    for node in ast.walk(tree):
        if isinstance(node, ast.Call) and _target_name(node) == name:
            return node
    return None


def _keyword_strings(call, attribute):
    return [child.value for keyword in call.keywords if keyword.arg == attribute
            for child in ast.walk(keyword.value)
            if isinstance(child, ast.Constant) and isinstance(child.value, str)]


def reviewed_targets(read):
    """Sources of the reviewed targets, and problems of entries that match no macro user.

    `read` maps a repository path to its text, or None when the file does not exist.
    """
    sources, stale = set(), []
    for build, name in sorted(REVIEWED_LOCAL_DEFINES):
        text = read(build)
        call = _rule_call(ast.parse(text, filename=build), name) if text is not None else None
        defines = _keyword_strings(call, "local_defines") if call is not None else []
        if not any(_MACRO.search(value) for value in defines):
            stale.append("%s: reviewed entry %s sets no text tier macro in `local_defines`; remove "
                         "it from REVIEWED_LOCAL_DEFINES" % (build, name))
            continue
        package = posixpath.dirname(build)
        sources.update(posixpath.join(package, src) for src in _keyword_strings(call, "srcs")
                       if not src.startswith((":", "/", "@")))
    return sources, stale


def _text_problems(path, text, reviewed_sources):
    """Problems of a header, C or C++ source, or .bazelrc file text."""
    if path.endswith(_SOURCE_SUFFIXES) and path in reviewed_sources:
        return []
    if path.endswith(_HEADER_SUFFIXES):
        reason = "a header must not depend on a text tier macro; use donner/svg/text/TextTier.h"
    elif path.endswith(_SOURCE_SUFFIXES):
        reason = ("only the sources of a reviewed target may test a text tier macro; query "
                  "donner/svg/text/TextTier.h at run time")
    else:
        reason = "a text tier macro must not be set from a .bazelrc file"
    return ["%s:%d: %s" % (path, number, reason)
            for number, line in enumerate(text.splitlines(), 1)
            if _MACRO.search(line) and not (_is_bazelrc(path) and line.lstrip().startswith("#"))]


def _is_build_graph_file(path):
    name = path.rsplit("/", 1)[-1]
    return name in ("BUILD", "BUILD.bazel") or name.endswith(".bzl")


def _is_bazelrc(path):
    name = path.rsplit("/", 1)[-1]
    return name.endswith(".bazelrc") or name.startswith(".bazelrc.")


def is_governed(path):
    """Whether this check reads `path`."""
    if _is_build_graph_file(path):
        return not path.startswith("third_party/")
    return path.endswith(_HEADER_SUFFIXES + _SOURCE_SUFFIXES) or _is_bazelrc(path)


def problems_of(files, read, full=True):
    """Problems of the given file texts, keyed by repository path.

    `read` maps any repository path to its text or None. With `full`, every reviewed entry is
    checked; otherwise only the entries of BUILD files among `files`.
    """
    sources, stale = reviewed_targets(read)
    if not full:
        stale = [problem for problem in stale if problem.split(":", 1)[0] in files]
    problems = list(stale)
    for path, text in sorted(files.items()):
        if _is_build_graph_file(path):
            problems += file_problems(path, text)
        else:
            problems += _text_problems(path, text, sources)
    return problems


def _tracked_files():
    listing = subprocess.run(["git", "ls-files", "-z"], check=True, capture_output=True).stdout
    return [path for path in listing.decode("utf-8").split("\0") if path and is_governed(path)]


def _read(path):
    if not os.path.isfile(path):
        return None
    with open(path, encoding="utf-8", errors="replace") as handle:
        return handle.read()


def main(argv):
    paths = [path for path in argv[1:] if is_governed(path)] if argv[1:] else _tracked_files()
    files = {path: _read(path) for path in paths}
    files = {path: text for path, text in files.items() if text is not None}
    problems = problems_of(files, _read, full=not argv[1:])
    for problem in problems:
        print(problem)
    print("Checked %d files: %d problem(s)." % (len(files), len(problems)))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
