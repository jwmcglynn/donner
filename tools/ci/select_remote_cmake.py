"""Select Darwin CMake on the same source paths as the CMake pull-request workflow."""

import fnmatch
import os
import subprocess


def selected(path):
    # These match the pull_request.paths in cmake.yml. A change to the unified lane also
    # exercises the Darwin consumer on its own pull request.
    patterns = (".github/workflows/cmake.yml", ".github/workflows/main.yml", "CMakePresets.json",
                "examples/cmake_consumer/*", "tools/cmake/*", "tools/ci/cmake_*", "tools/ci/remote_cmake.py",
                "tools/ci/select_remote_cmake.py", "tools/ci/unified_ci.py", "MODULE.bazel",
                "third_party/bazel/non_bcr_deps.bzl", "tools/ci_macos_xcode.txt", "tools/ci_select_macos_xcode.sh")
    name = path.rsplit("/", 1)[-1]
    return (name == "CMakeLists.txt" or name.endswith((".cmake", ".cmake.in"))
            or any(fnmatch.fnmatchcase(path, p) for p in patterns))


if __name__ == "__main__":
    revisions = os.environ["BASE_SHA"] + "..." + os.environ["HEAD_SHA"]
    files = subprocess.check_output(["git", "diff", "--no-renames", "--name-only", "-z", revisions, "--"])
    with open(os.environ["GITHUB_OUTPUT"], "a") as output:
        print("run=" + str(any(selected(os.fsdecode(f)) for f in files.split(b"\0") if f)).lower(), file=output)
