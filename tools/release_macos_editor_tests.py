"""Contracts for a retained native editor artifact, separate from the Wasm editor."""

import unittest

from tools import bcr_preflight, release_candidate


class NativeEditorReleaseContractTest(unittest.TestCase):
    def test_native_editor_is_required_by_the_preflight_and_release_record(self):
        self.assertIn("editor-macos", release_candidate.RELEASE_ROLES)
        self.assertIn("editor-macos", release_candidate.PREFLIGHT_ROLES)
        self.assertIn("editor-macos", release_candidate.LOCK_ROLES)
        self.assertNotIn("editor-macos", release_candidate.CLI_PLATFORMS)

    def test_existing_macos_attempt_builds_and_packages_the_native_app(self):
        plan = bcr_preflight.cli_plan(bcr_preflight.Layout("/fixture"), "darwin-arm64",
                                      "a" * 40, "123", "2")
        commands = [item.argv for item in plan]
        self.assertTrue(any("//donner/editor:editor_macos_app" in argv for argv in commands))
        self.assertTrue(any("tools.release_macos_editor" in argv and "package" in argv
                            for argv in commands))


if __name__ == "__main__":
    unittest.main()
