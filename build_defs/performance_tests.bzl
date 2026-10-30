"""Configured-action tests for native speed policies and their explicit escape hatches."""

load("@bazel_skylib//lib:unittest.bzl", "analysistest", "asserts")
load("@rules_cc//cc:defs.bzl", "cc_library")
load("//build_defs:performance.bzl", "native_speed_copts")
load("//build_defs:rules.bzl", "donner_perf_sensitive_cc_library")

_OPTIMIZATION_OPTIONS = ["-O0", "-O1", "-O2", "-O3", "-Os", "-Oz"]
_SpeedOptionsInfo = provider(fields = ["options"])

def _speed_options_probe_impl(ctx):
    return [_SpeedOptionsInfo(options = ctx.attr.options)]

_speed_options_probe = rule(
    implementation = _speed_options_probe_impl,
    attrs = {"options": attr.string_list()},
)

def _compile_policy_test_impl(ctx):
    env = analysistest.begin(ctx)
    actions = [a for a in analysistest.target_actions(env) if a.mnemonic == "CppCompile"]
    asserts.equals(env, 1, len(actions))
    for action in actions:
        options = [arg for arg in action.argv if arg in _OPTIMIZATION_OPTIONS]
        asserts.true(env, len(options) > 0)
        if options:
            asserts.equals(env, ctx.attr.expected, options[-1])
        asserts.true(env, "-DKEEP_CALLER_OPTION=1" in action.argv)
    return analysistest.end(env)

def _selection_test_impl(ctx):
    env = analysistest.begin(ctx)
    options = analysistest.target_under_test(env)[_SpeedOptionsInfo].options
    asserts.equals(env, ctx.attr.expected_options, options)
    return analysistest.end(env)

_os_compile_test = analysistest.make(
    _compile_policy_test_impl,
    attrs = {"expected": attr.string()},
    config_settings = {"//command_line_option:copt": ["-Os"]},
)

_oz_compile_test = analysistest.make(
    _compile_policy_test_impl,
    attrs = {"expected": attr.string()},
    config_settings = {"//command_line_option:copt": ["-Oz"]},
)

_debug_compile_test = analysistest.make(
    _compile_policy_test_impl,
    attrs = {"expected": attr.string()},
    config_settings = {
        "//command_line_option:compilation_mode": "dbg",
        "//command_line_option:copt": ["-Oz"],
    },
)

_native_selection_test = analysistest.make(
    _selection_test_impl,
    attrs = {"expected_options": attr.string_list()},
)

_wasm_selection_test = analysistest.make(
    _selection_test_impl,
    attrs = {"expected_options": attr.string_list()},
    config_settings = {
        "//command_line_option:platforms": ["//build_defs:optimization_test_wasm"],
    },
)

_debug_selection_test = analysistest.make(
    _selection_test_impl,
    attrs = {"expected_options": attr.string_list()},
    config_settings = {"//command_line_option:compilation_mode": "dbg"},
)

def performance_tests():
    """Registers tests of actual compiler actions, dependency isolation and Wasm selection."""
    native.platform(
        name = "optimization_test_wasm",
        constraint_values = ["@platforms//cpu:wasm32"],
    )
    caller = ["-DKEEP_CALLER_OPTION=1"]
    cc_library(
        name = "speed_dependency_fixture",
        srcs = ["performance_fixture.cc"],
        copts = caller,
        testonly = True,
        tags = ["manual"],
    )
    donner_perf_sensitive_cc_library(
        name = "speed_o2_fixture",
        srcs = ["performance_fixture.cc"],
        copts = caller + ["-O3"],
        deps = [":speed_dependency_fixture"],
        testonly = True,
    )
    donner_perf_sensitive_cc_library(
        name = "speed_o3_fixture",
        srcs = ["performance_fixture.cc"],
        copts = caller,
        optimization_level = "O3",
        testonly = True,
    )
    donner_perf_sensitive_cc_library(
        name = "speed_debug_fixture",
        srcs = ["performance_fixture.cc"],
        copts = caller,
        allow_debug_builds_config = ":debug_compilation",
        testonly = True,
    )
    _speed_options_probe(
        name = "speed_selection_fixture",
        options = native_speed_copts(),
        testonly = True,
    )
    _speed_options_probe(
        name = "speed_debug_selection_fixture",
        options = native_speed_copts(allow_debug = True),
        testonly = True,
    )
    _os_compile_test(name = "speed_o2_under_os_test", target_under_test = ":speed_o2_fixture", expected = "-O2")
    _oz_compile_test(name = "speed_o2_under_oz_test", target_under_test = ":speed_o2_fixture", expected = "-O2")
    _oz_compile_test(name = "speed_o3_under_oz_test", target_under_test = ":speed_o3_fixture", expected = "-O3")
    _os_compile_test(name = "speed_dependency_not_upgraded_test", target_under_test = ":speed_dependency_fixture", expected = "-Os")
    _debug_compile_test(name = "speed_debug_escape_test", target_under_test = ":speed_debug_fixture_unconfigured", expected = "-Oz")
    _native_selection_test(name = "speed_native_selection_test", target_under_test = ":speed_selection_fixture", expected_options = ["-O2"])
    _wasm_selection_test(name = "speed_wasm_unchanged_test", target_under_test = ":speed_selection_fixture", expected_options = [])
    _debug_selection_test(name = "speed_native_debug_selection_test", target_under_test = ":speed_debug_selection_fixture", expected_options = [])
