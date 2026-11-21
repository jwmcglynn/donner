"""Tests for tools/bazelrc_configs.py."""

import unittest

from tools import bazelrc_configs

_RC = """\
# A comment line.
build --verbose_failures
common:lane --//flag=on
test:lane --test_tag_filters=a,-b
build:lane --jobs=4 --strategy_regexp="^Executing x$=local"
test:lane --config=nested
build:nested --copt=-O1
test:nested --test_env=X=1
fetch:lane --fetch_only
test:loop --config=loop
"""


class BazelrcConfigsTest(unittest.TestCase):
    def setUp(self):
        self.configs = bazelrc_configs.parse(_RC)

    def test_parse_reads_every_config_line_in_file_order(self):
        self.assertEqual(set(self.configs), {"lane", "nested", "loop"})
        self.assertEqual(self.configs["lane"][2],
                         ("build", ["--jobs=4", "--strategy_regexp=^Executing x$=local"]))

    def test_expansion_follows_bazels_section_order(self):
        # `common`, then `build`, then `test`, whatever the file order; nested configs in place.
        self.assertEqual(
            bazelrc_configs.expand("test", ["--keep_going", "--config=lane", "//a"],
                                   self.configs),
            ["--keep_going", "--//flag=on", "--jobs=4", "--strategy_regexp=^Executing x$=local",
             "--test_tag_filters=a,-b", "--copt=-O1", "--test_env=X=1", "//a"])

    def test_test_lines_do_not_apply_to_a_build(self):
        self.assertEqual(bazelrc_configs.expand("build", ["--config=lane"], self.configs),
                         ["--//flag=on", "--jobs=4", "--strategy_regexp=^Executing x$=local"])

    def test_fetch_reads_build_and_test_sections(self):
        # As `bazel fetch --announce_rc` reports: fetch inherits test, which inherits build.
        self.assertEqual(
            bazelrc_configs.expand("fetch", ["--config=lane"], self.configs)[-1],
            "--fetch_only")
        self.assertIn("--test_tag_filters=a,-b",
                      bazelrc_configs.expand("fetch", ["--config=lane"], self.configs))

    def test_query_reads_only_common(self):
        self.assertEqual(bazelrc_configs.expand("query", ["--config=lane"], self.configs),
                         ["--//flag=on"])

    def test_unknown_configs_and_the_spaced_form(self):
        self.assertEqual(
            bazelrc_configs.expand("build", ["--config=runner_only", "--config", "nested"],
                                   self.configs),
            ["--config=runner_only", "--copt=-O1"])

    def test_a_config_that_names_itself_is_an_error(self):
        with self.assertRaisesRegex(ValueError, "loop"):
            bazelrc_configs.expand("test", ["--config=loop"], self.configs)

    def test_commands_reads_each_bazel_command_of_a_script(self):
        script = (
            "# bazelisk test --config=lane is only a comment\n"
            "echo 'running bazel tests'\n"
            "tools/ci_bazel_test.sh bazelisk --nohome_rc test --config=lane \\\n"
            "    --test_tag_filters=c $TARGETS\n"
            "bazel build //a:b && bazelisk shutdown\n")
        commands = bazelrc_configs.commands(script, self.configs)
        self.assertEqual([command for command, _, _ in commands], ["test", "build", "shutdown"])
        command, options, words = commands[0]
        self.assertEqual(bazelrc_configs.last_value(options, "test_tag_filters"), "c")
        self.assertIn("--test_env=X=1", options)
        self.assertEqual(words, ["$TARGETS"])
        self.assertIsNone(bazelrc_configs.last_value(commands[1][1], "test_tag_filters"))

    def test_commands_expands_the_spaced_form_variables_and_chained_commands(self):
        script = ("x=$(bazelisk info output_base) && bazelisk test --config nested $FLAGS //a; "
                  "bazel build ${MORE} //b\n"
                  "echo 'no bazel here'\n")
        commands = bazelrc_configs.commands(
            script, self.configs, {"FLAGS": "--config=lane --keep_going", "MORE": "--jobs=2"})
        self.assertEqual([command for command, _, _ in commands], ["info", "test", "build"])
        _, options, words = commands[1]
        self.assertEqual(options[:2], ["--copt=-O1", "--test_env=X=1"])
        self.assertIn("--test_tag_filters=a,-b", options)
        self.assertEqual(options[-1], "--keep_going")
        self.assertEqual(words, ["//a"])
        self.assertEqual(commands[2][1], ["--jobs=2"])
        self.assertEqual(commands[2][2], ["//b"])


if __name__ == "__main__":
    unittest.main()
