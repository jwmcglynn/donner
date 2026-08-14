import os
from pathlib import Path
import re
import unittest


class PackageGlobSafetyTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        runfiles = Path(os.environ["TEST_SRCDIR"]) / os.environ["TEST_WORKSPACE"]
        cls.root_build = (runfiles / "BUILD.bazel").read_text()
        cls.package_bzl = (runfiles / "build_defs/package.bzl").read_text()

    def root_inventory_globs(self):
        match = re.search(
            r"donner_package\(\s*gpu_inventory_globs\s*=\s*\[(.*?)\]\s*,?\s*\)",
            self.root_build,
            re.DOTALL,
        )
        self.assertIsNotNone(match, "root donner_package() override is missing")
        return re.findall(r'"([^"]+)"', match.group(1))

    def test_root_inventory_globs_do_not_recurse(self):
        recursive = [
            pattern
            for pattern in self.root_inventory_globs()
            if pattern == "**" or pattern.startswith("**/")
        ]
        self.assertEqual(
            [],
            recursive,
            "workspace-root recursive globs follow Bazel output symlinks before "
            "exclude patterns are applied",
        )

    def test_macro_rejects_recursive_workspace_root_overrides(self):
        self.assertIn('native.package_name() == ""', self.package_bzl)
        self.assertIn('pattern.startswith("**/")', self.package_bzl)


if __name__ == "__main__":
    unittest.main()
