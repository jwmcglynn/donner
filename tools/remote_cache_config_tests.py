import re
import unittest

from python.runfiles import runfiles


class RemoteCacheConfigTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        resolver = runfiles.Create()
        bazelrc_path = resolver.Rlocation("donner/.bazelrc")
        bazelversion_path = resolver.Rlocation("donner/.bazelversion")
        assert bazelrc_path is not None
        assert bazelversion_path is not None

        with open(bazelrc_path, encoding="utf-8") as bazelrc:
            cls.bazelrc_lines = {
                line.strip()
                for line in bazelrc
                if line.strip() and not line.lstrip().startswith("#")
            }
        with open(bazelversion_path, encoding="utf-8") as bazelversion:
            cls.bazel_version = bazelversion.read().strip()

    def test_pinned_bazel_supports_lost_input_rewinding(self):
        match = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)", self.bazel_version)
        self.assertIsNotNone(match, self.bazel_version)
        self.assertGreaterEqual(tuple(map(int, match.groups())), (8, 7, 0))

    def test_builds_rewind_evicted_remote_cache_inputs(self):
        self.assertIn("build --rewind_lost_inputs", self.bazelrc_lines)

    def test_whole_build_retry_is_bounded(self):
        self.assertIn(
            "build --experimental_remote_cache_eviction_retries=1",
            self.bazelrc_lines,
        )

    def test_remote_python_launchers_are_runfiles_aware(self):
        self.assertIn(
            "common --@rules_python//python/config_settings:bootstrap_impl=script",
            self.bazelrc_lines,
        )

    def test_macos_downloads_cached_browser_trees_in_full(self):
        # A lazily fetched cached browser tree loses files beneath its directory
        # symlinks before a local macOS test can stage it. This holds for a disk
        # cache alone, so it cannot live only in the remote cache fragment.
        flag = "build:macos --remote_download_regex="
        patterns = [line[len(flag):] for line in self.bazelrc_lines if line.startswith(flag)]
        pattern = r".*/rules_playwright[+][^/]*/browsers/.*"
        self.assertIn(pattern, patterns)
        # Bazel matches the whole exec path of each output.
        browser_root = ("bazel-out/darwin_arm64-opt-exec-ST-d57f47055a04/bin/external/"
                        "rules_playwright++playwright+playwright/browsers/mac14-arm64/")
        for path in ("chromium-1248/chrome-mac-arm64/Google Chrome for Testing.app/Contents/"
                     "Frameworks/Google Chrome for Testing Framework.framework/Versions/"
                     "Current/Resources/en.lproj/locale.pak",
                     "webkit_mac14-arm64_special-2370/WebCore.framework/Versions/Current/"
                     "Resources/modern-media-controls/controls/button.js"):
            self.assertIsNotNone(re.fullmatch(pattern, browser_root + path), path)
        for path in ("bazel-out/darwin_arm64-fastbuild/bin/donner/editor/wasm/editor.wasm",
                     "bazel-out/darwin_arm64-fastbuild/testlogs/donner/editor/wasm/tests/"
                     "mac_browser_archive_tests/test.log",
                     "external/rules_playwright+/playwright/private/unzip_browser.bzl"):
            self.assertIsNone(re.fullmatch(pattern, path), path)

    def test_coverage_skips_host_incompatible_explicit_targets(self):
        self.assertIn(
            "coverage --skip_incompatible_explicit_targets",
            self.bazelrc_lines,
        )


if __name__ == "__main__":
    unittest.main()
