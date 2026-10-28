"""A module that depends on Donner can load the packages of the Geode production closure.

Bazel resolves a dependency's load statements against that dependency's own non-development
repositories. A BUILD or .bzl file in the Geode production closure that loads a repository
declared with `dev_dependency = True` therefore loads in Donner's own workspace but fails to load
for every module that depends on Donner and enables Geode. The input is the `buildfiles` query
over one target of each package the Geode production closure adds to the default renderer's, which
lists those packages' BUILD files and every .bzl file they load, transitively.
"""

from pathlib import Path
import sys
import unittest

from tools import bcr_dependency_closure as closure


# Build files the query must report, so a broken query or package list cannot pass vacuously.
REQUIRED_BUILD_FILES = {
    "//donner/gpu:BUILD.bazel",
    "//donner/gpu/metal:BUILD.bazel",
    "//donner/gpu/shader:BUILD.bazel",
    "//donner/gpu/shader/wgsl:BUILD.bazel",
    "//donner/gpu/vulkan:BUILD.bazel",
    "//donner/svg/renderer:BUILD.bazel",
    "//donner/svg/renderer/geode:BUILD.bazel",
    "//build_defs:rules.bzl",
}

# Development-only rule sets that Donner's test targets use, which the MODULE.bazel scan must find.
EXPECTED_DEVELOPMENT_RULE_SETS = {"aspect_rules_js"}


def _argument(name):
    prefix = f"--{name}="
    return Path(next(arg[len(prefix):] for arg in sys.argv[1:] if arg.startswith(prefix)))


def _build_files():
    text = _argument("build-files").read_text(encoding="utf-8")
    return {line.strip() for line in text.splitlines() if line.strip()}


def _development_repositories():
    return closure.dev_repository_names(_argument("module").read_text(encoding="utf-8"))


class GeodeConsumerLoadsTest(unittest.TestCase):
    def test_query_reports_every_geode_production_package(self):
        self.assertLessEqual(REQUIRED_BUILD_FILES, _build_files(),
                             "the query no longer covers the Geode production packages")

    def test_module_scan_finds_the_development_rule_sets(self):
        self.assertLessEqual(EXPECTED_DEVELOPMENT_RULE_SETS, _development_repositories())

    def test_no_build_file_loads_a_development_only_repository(self):
        development = _development_repositories()
        findings = sorted(label for label in _build_files()
                          if closure.repository_parts(label) & development)
        self.assertEqual(
            findings, [],
            "the Geode production packages load development-only repositories; move the targets "
            "that need them into a test-only package")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
