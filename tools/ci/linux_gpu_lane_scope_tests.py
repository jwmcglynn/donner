"""Pins the Linux GPU lane's reviewed scope to the BUILD files it names.

The selector keeps tests by label, so a renamed suite or an emptied package would silently shrink
the lane. These tests read the BUILD files and fail when a scope target is no longer a GPU-opening
Geode test wrapper, when a scope package holds fewer of them than when the scope was reviewed, or
when a reviewed test for another OS stops being one.
"""

import ast
from pathlib import Path
import unittest

from python.runfiles import runfiles

import linux_gpu_lane as lane

# GPU-opening Geode test wrappers in //donner/svg/renderer/geode when the scope was reviewed. A
# change that lowers the count must lower this number in review.
MINIMUM_PACKAGE_TESTS = 15


def _calls(package):
    """{name: (function name, call node)} for each top-level call in a package's BUILD file."""
    path = runfiles.Create().Rlocation("donner/%s/BUILD.bazel" % package.removeprefix("//"))
    calls = {}
    for node in ast.parse(Path(path).read_text(encoding="utf-8")).body:
        if isinstance(node, ast.Expr) and isinstance(node.value, ast.Call):
            keywords = {keyword.arg: keyword.value for keyword in node.value.keywords}
            name = keywords.get("name")
            if isinstance(name, ast.Constant):
                calls[name.value] = (getattr(node.value.func, "id", None), node.value, keywords)
    return calls


def _gpu_wrapper(call):
    function, _, keywords = call
    opens = keywords.get("opens_gpu_device")
    backend = keywords.get("renderer_backend")
    return (function == "donner_multi_transitioned_test" and isinstance(opens, ast.Constant)
            and opens.value is True and isinstance(backend, ast.Constant)
            and backend.value == "geode")


class LinuxGpuLaneScopeTest(unittest.TestCase):
    def test_each_scope_target_is_a_gpu_opening_geode_wrapper(self):
        for label in lane.SCOPE_TARGETS:
            with self.subTest(label=label):
                package, name = label.split(":", 1)
                calls = _calls(package)
                self.assertIn(name, calls)
                self.assertTrue(_gpu_wrapper(calls[name]), ast.dump(calls[name][1])[:300])

    def test_each_scope_package_keeps_its_gpu_opening_wrappers(self):
        for package in lane.SCOPE_PACKAGES:
            with self.subTest(package=package):
                wrappers = sorted(name for name, call in _calls(package).items()
                                  if _gpu_wrapper(call))
                self.assertGreaterEqual(len(wrappers), MINIMUM_PACKAGE_TESTS, wrappers)

    def test_each_other_os_test_builds_only_for_another_os(self):
        for label in lane.OTHER_OS_TESTS:
            with self.subTest(label=label):
                package, name = label.split(":", 1)
                calls = _calls(package)
                self.assertTrue(_gpu_wrapper(calls[name]))
                dep = calls[name][2]["dep"].value.lstrip(":")
                compatible = ast.unparse(calls[dep][2]["target_compatible_with"])
                self.assertIn("@platforms//os:macos", compatible)
                self.assertIn("@platforms//:incompatible", compatible)


if __name__ == "__main__":
    unittest.main()
