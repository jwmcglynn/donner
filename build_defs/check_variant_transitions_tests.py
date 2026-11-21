"""Tests for check_variant_transitions.py on synthetic and checked-in Starlark."""

from pathlib import Path
import unittest

from python.runfiles import runfiles

from build_defs import check_variant_transitions as check

_SETTINGS = ["//donner/svg/renderer:text", "//build_defs:metal_shader_validation"]


def _transition(name, outputs):
    return ("%s = transition(\n    implementation = _impl,\n    inputs = [],\n    outputs = %s,\n)\n"
            % (name, outputs))


class CheckVariantTransitionsTest(unittest.TestCase):
    def test_unreviewed_writer_is_named_with_its_settings(self):
        sources = {"pkg/a.bzl": _transition("_mine", '["//donner/svg/renderer:text"]')}
        self.assertEqual(check.check(sources, _SETTINGS), [
            "pkg/a.bzl: transition _mine writes //donner/svg/renderer:text without "
            "variant_transition. Declare it with variant_transition, or add it to REVIEWED_SITES in "
            "check_variant_transitions.py with the reason it stays outside."])

    def test_writes_through_a_constant_from_another_file(self):
        sources = {
            "pkg/options.bzl": 'OPTIONS = BASE + [\n    "//build_defs:metal_shader_validation",\n]\n'
                               'BASE = ["//command_line_option:copt"]\n',
            "pkg/a.bzl": _transition("_mine", "OPTIONS"),
        }
        problems = check.check(sources, _SETTINGS)
        self.assertEqual(len(problems), 1)
        self.assertIn("writes //build_defs:metal_shader_validation", problems[0])

    def test_inputs_comments_and_variant_transition_are_not_writes(self):
        sources = {"pkg/a.bzl": (
            "# _x = transition(outputs = [\"//donner/svg/renderer:text\"])\n"
            "_reader = transition(implementation = _impl, inputs = [\"//donner/svg/renderer:text\"],"
            " outputs = [\"//command_line_option:copt\"])\n"
            + _transition("_declared", '["//donner/svg/renderer:text"]').replace(
                " transition(", " variant_transition("))}
        self.assertEqual(check.check(sources, _SETTINGS), [])

    def test_reviewed_site_that_stopped_writing_is_stale(self):
        path, name = "tools/ci/shipped_product.bzl", "_shipped_configuration"
        self.assertIn((path, name), check.REVIEWED_SITES)
        sources = {path: _transition(name, '["//command_line_option:copt"]')}
        self.assertEqual(check.check(sources, _SETTINGS), [
            "%s: reviewed transition %s no longer writes a variant setting; remove it from "
            "REVIEWED_SITES." % (path, name)])

    def test_variant_settings_are_read_from_the_settings_file(self):
        text = Path(runfiles.Create().Rlocation("donner/build_defs/variant_settings.bzl")).read_text()
        settings = check.variant_settings(text)
        self.assertIn("//donner/svg/renderer:text_full", settings)
        self.assertIn("//build_defs:metal_shader_validation", settings)
        self.assertNotIn("//command_line_option:platforms", settings)


if __name__ == "__main__":
    unittest.main()
