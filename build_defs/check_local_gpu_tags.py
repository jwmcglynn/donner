#!/usr/bin/env python3
"""Checks every tracked BUILD file for the local GPU test scheduling tags.

A test that opens a GPU device takes one unit of the `local_gpu` resource
(`LOCAL_GPU_TEST_TAG` in build_defs/rules.bzl), so GPU-opening tests run one at a time but
alongside CPU-only tests. This check keeps the old whole-machine tags from coming back:

  - no `exclusive-if-local` tag anywhere;
  - `resources:local_gpu:<count>` only with a count of 1;
  - `exclusive` only on the reviewed targets in REVIEWED_EXCLUSIVE.

Usage: python3 build_defs/check_local_gpu_tags.py [BUILD file...]
With no arguments it checks every tracked BUILD file; run it from the repository root.
"""

import re
import subprocess
import sys

# Tests that must have the machine to themselves, by (BUILD file, target name), with the reason.
REVIEWED_EXCLUSIVE = {
    # Wall-clock benchmarks: concurrent tests would distort the measurement. All are manual.
    ("donner/benchmarks/BUILD.bazel", "dom_lifetime_perf_capture"),
    ("third_party/tiny-skia-cpp/tests/benchmarks/BUILD.bazel", "filter_perf_regression_test"),
    ("third_party/tiny-skia-cpp/tests/benchmarks/BUILD.bazel", "render_perf_regression_test"),
}
_TAG = re.compile(r'"(exclusive-if-local|exclusive|resources:local_gpu:[^"]*)"')
_NAME = re.compile(r'(?m)^\s*name = "([^"]+)"')


def _target_at(text, position):
    """Name of the last rule declared before `position`, or "" when there is none."""
    names = [match.group(1) for match in _NAME.finditer(text, 0, position)]
    return names[-1] if names else ""


def _tag_problem(path, target, tag):
    """The problem with one tag on a target, or None."""
    if tag == "exclusive-if-local":
        return ("%s takes the whole machine with exclusive-if-local; set opens_gpu_device or tag "
                "it resources:local_gpu:1" % target)
    if tag == "exclusive" and (path, target) not in REVIEWED_EXCLUSIVE:
        return ("%s is exclusive without a reviewed reason in build_defs/check_local_gpu_tags.py"
                % target)
    if tag.startswith("resources:local_gpu:") and tag != "resources:local_gpu:1":
        return "%s takes %s; GPU-opening tests take exactly one unit" % (target, tag)
    return None


def build_file_problems(files):
    """Problems of BUILD file texts, keyed by repository path."""
    problems = []
    for path, text in sorted(files.items()):
        for match in _TAG.finditer(text):
            problem = _tag_problem(path, _target_at(text, match.start()), match.group(1))
            if problem:
                line = text.count("\n", 0, match.start()) + 1
                problems.append("%s:%d: %s" % (path, line, problem))
    return problems


def _tracked_build_files():
    listing = subprocess.run(["git", "ls-files", "-z"], check=True, capture_output=True).stdout
    return [path for path in listing.decode("utf-8").split("\0")
            if path.rsplit("/", 1)[-1] in ("BUILD", "BUILD.bazel")]


def main(argv):
    files = {}
    for path in argv[1:] or _tracked_build_files():
        with open(path, encoding="utf-8") as handle:
            files[path] = handle.read()
    problems = build_file_problems(files)
    for problem in problems:
        print(problem)
    print("Checked %d BUILD files: %d problem(s)." % (len(files), len(problems)))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
