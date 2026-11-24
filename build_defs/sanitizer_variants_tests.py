"""Checks that each sanitizer variant builds with the flags of its .bazelrc config.

build_defs/sanitizer_variants.bzl lists, for each sanitizer variant, the compile options, link
options and features its transition adds. They must be the ones `--config=asan` and `--config=tsan`
apply, so a variant run in the native CI invocation and a local `--config` run build the same code.
"""

import os
import re
import shlex
import types
import unittest
from pathlib import Path

_RC_LINE = re.compile(r"^build:(asan|tsan)\s+(.*?)\s*$")


def _runfile(path):
    return Path(os.environ["TEST_SRCDIR"]) / os.environ["TEST_WORKSPACE"] / path


def _config_flags(rc_text):
    """{config: [flags]} for the `build:asan` and `build:tsan` lines, in file order."""
    flags = {}
    for line in rc_text.splitlines():
        match = _RC_LINE.match(line)
        if match:
            flags.setdefault(match.group(1), []).extend(shlex.split(match.group(2), comments=True))
    return flags


def _values(flags, option):
    prefix = "--%s=" % option
    return [flag[len(prefix):] for flag in flags if flag.startswith(prefix)]


def _variants(bzl_text):
    """SANITIZER_VARIANTS from the .bzl file, which is also valid Python with `struct` defined."""
    scope = {"struct": lambda **fields: types.SimpleNamespace(**fields)}
    exec(compile(bzl_text, "sanitizer_variants.bzl", "exec"), scope)  # noqa: S102
    return scope["SANITIZER_VARIANTS"]


class SanitizerVariantsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.flags = _config_flags(_runfile(".bazelrc").read_text())
        cls.variants = _variants(_runfile("build_defs/sanitizer_variants.bzl").read_text())

    def test_every_variant_has_its_config(self):
        self.assertEqual(sorted(self.variants), sorted(self.flags))

    def test_variants_compile_and_link_as_their_configs_do(self):
        for name, variant in self.variants.items():
            with self.subTest(sanitizer=name):
                flags = self.flags[name]
                self.assertEqual(variant.copts, _values(flags, "copt"))
                self.assertEqual(variant.linkopts, _values(flags, "linkopt"))
                self.assertEqual(variant.features, _values(flags, "features"))
                self.assertIn("--strip=never", flags, "the transition sets strip=never too")
                self.assertIn("--config=latest_llvm", flags,
                              "the variant inherits the invocation's toolchain instead")


if __name__ == "__main__":
    unittest.main()
