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


# Every kind of flag the asan and tsan configs may use. A flag of another kind fails the test, so a
# new kind is either mirrored in the variants or listed here with how it is handled.
_HANDLED = (
    "--config=latest_llvm",  # The variant inherits the invocation's toolchain instead.
    "--//build_defs:sanitizer=",  # The transition sets it to the variant's own sanitizer.
    "--features=",
    "--copt=",
    "--linkopt=",
    "--strip=never",
    "--test_env=",  # Compared below, with its deliberate differences.
)

# How each variant's test environment differs from its config's `--test_env` flags, deliberately.
_ENV_DIFFERENCES = {
    "asan": {
        # Added: LeakSanitizer needs ptrace at exit, which the self-hosted lane's remote test
        # sandbox blocks, so leak detection is off in the variant.
        "added": {"ASAN_OPTIONS": "detect_leaks=0"},
        # Dropped: the symbolizer path is passed through from the invocation's environment, which
        # a variant does not set, and the Vulkan loader option matters only to Geode tests, which
        # have no sanitizer variants.
        "dropped": {"ASAN_SYMBOLIZER_PATH", "VK_LOADER_DISABLE_DYNAMIC_LIBRARY_UNLOADING"},
    },
    "tsan": {"added": {}, "dropped": set()},
}


def _config_env(flags):
    """{name: value} of the `--test_env` flags; a pass-through flag maps to None."""
    env = {}
    for value in _values(flags, "test_env"):
        name, _, setting = value.partition("=")
        env[name] = setting if "=" in value else None
    return env


class SanitizerVariantsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.flags = _config_flags(_runfile(".bazelrc").read_text())
        cls.variants = _variants(_runfile("build_defs/sanitizer_variants.bzl").read_text())

    def test_every_variant_has_its_config(self):
        self.assertEqual(sorted(self.variants), sorted(self.flags))

    def test_every_config_flag_is_a_handled_kind(self):
        for name, flags in self.flags.items():
            for flag in flags:
                with self.subTest(sanitizer=name, flag=flag):
                    self.assertTrue(flag.startswith(_HANDLED), "unhandled flag kind: " + flag)

    def test_each_config_names_its_own_sanitizer(self):
        for name, flags in self.flags.items():
            with self.subTest(sanitizer=name):
                self.assertEqual([name], _values(flags, "//build_defs:sanitizer"))

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

    def test_variant_environments_differ_from_their_configs_only_as_listed(self):
        for name, variant in self.variants.items():
            with self.subTest(sanitizer=name):
                config = _config_env(self.flags[name])
                difference = _ENV_DIFFERENCES[name]
                kept = {key: value for key, value in config.items()
                        if key not in difference["dropped"]}
                self.assertEqual(set(config) & difference["dropped"], difference["dropped"],
                                 "a listed drop is no longer in the config")
                self.assertEqual(kept | difference["added"], variant.test_env)


if __name__ == "__main__":
    unittest.main()
