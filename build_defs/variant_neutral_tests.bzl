"""Analysis tests for variant_neutral.bzl and the variant transition check."""

load("@bazel_skylib//lib:unittest.bzl", "analysistest", "asserts", "unittest")
load("@rules_cc//cc:defs.bzl", "cc_library")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")
load("//build_defs:variant_neutral.bzl", "variant_neutral_srcs")
load("//build_defs:variant_settings.bzl", "unclassified_variant_outputs")

# Resolve in this repository before Skylib creates its analysis-test transition.
_SHARE = str(Label("//build_defs:share_variant_neutral_compiles"))
_BACKEND = str(Label("//donner/svg/renderer:renderer_backend"))
_TEXT_FULL = str(Label("//donner/svg/renderer:text_full"))
_TEXT_FULL_ENABLED = "//donner/svg/renderer:text_full_enabled"

def _compiles(env):
    target = analysistest.target_under_test(env)
    asserts.true(env, CcInfo in target, "expected a C++ library")
    return [
        action.outputs.to_list()[0].basename.replace(".pic.o", ".o")
        for action in analysistest.target_actions(env)
        if action.mnemonic == "CppCompile"
    ]

def _links_neutral_objects_impl(ctx):
    env = analysistest.begin(ctx)
    asserts.equals(env, [], _compiles(env), "own compiles")
    return analysistest.end(env)

def _compiles_in_place_impl(ctx):
    env = analysistest.begin(ctx)
    asserts.equals(env, ["Probe.o"], sorted({name: True for name in _compiles(env)}.keys()))
    return analysistest.end(env)

def _compiles_own_sources_impl(ctx):
    env = analysistest.begin(ctx)
    asserts.true(env, len(_compiles(env)) > 0, "expected the library to compile its own sources")
    return analysistest.end(env)

def _rejected_impl(ctx):
    env = analysistest.begin(ctx)
    asserts.expect_failure(env, ctx.attr.expected_message)
    return analysistest.end(env)

_SHARED_TEXT_FULL = {_SHARE: True, _TEXT_FULL: True}
_SHARED_GEODE = {_SHARE: True, _BACKEND: "geode"}
_MESSAGE = {"expected_message": attr.string(mandatory = True)}

_links_neutral_objects_test = analysistest.make(
    _links_neutral_objects_impl,
    config_settings = _SHARED_TEXT_FULL,
)
_links_neutral_objects_geode_test = analysistest.make(
    _links_neutral_objects_impl,
    config_settings = _SHARED_GEODE,
)
_compiles_in_place_test = analysistest.make(
    _compiles_in_place_impl,
    config_settings = {_SHARE: False, _TEXT_FULL: True},
)
_links_neutral_objects_by_default_test = analysistest.make(
    _links_neutral_objects_impl,
    config_settings = {_TEXT_FULL: True},
)
_compiles_own_sources_when_off_test = analysistest.make(
    _compiles_own_sources_impl,
    config_settings = {_SHARE: False, _TEXT_FULL: True},
)
_rejected_text_full_test = analysistest.make(
    _rejected_impl,
    attrs = _MESSAGE,
    expect_failure = True,
    config_settings = _SHARED_TEXT_FULL,
)
_rejected_geode_test = analysistest.make(
    _rejected_impl,
    attrs = _MESSAGE,
    expect_failure = True,
    config_settings = _SHARED_GEODE,
)
_rejected_default_test = analysistest.make(
    _rejected_impl,
    attrs = _MESSAGE,
    expect_failure = True,
    config_settings = {_SHARE: True},
)

def _unclassified_outputs_test_impl(ctx):
    env = unittest.begin(ctx)
    asserts.equals(env, [], unclassified_variant_outputs([
        _TEXT_FULL.removeprefix("@@"),
        "//command_line_option:platforms",
    ]))
    asserts.equals(env, ["//build_defs:new_variant"], unclassified_variant_outputs([
        "//donner/svg/renderer:text",
        "//build_defs:new_variant",
    ]))
    return unittest.end(env)

_unclassified_outputs_test = unittest.make(_unclassified_outputs_test_impl)

def _tier_variable_impl(ctx):
    return [platform_common.TemplateVariableInfo({"VARIANT_NEUTRAL_TEST_TIER": ctx.attr.value})]

_tier_variable = rule(
    implementation = _tier_variable_impl,
    attrs = {"value": attr.string(mandatory = True)},
)

def _fixture(name, **kwargs):
    common = {"tags": ["manual"], "testonly": True}
    cc_library(
        name = name,
        srcs = variant_neutral_srcs(
            name = name,
            srcs = ["testdata/variant_neutral/Probe.cc"],
            hdrs = ["testdata/variant_neutral/Probe.h"],
            **(kwargs | common)
        ),
        hdrs = ["testdata/variant_neutral/Probe.h"],
        **(kwargs | common)
    )

def _tier_select(full, other):
    return select({_TEXT_FULL_ENABLED: full, "//conditions:default": other})

def _generated_header(name, cmd, by_file = False, include_prefix = None):
    common = {"tags": ["manual"], "testonly": True}
    native.genrule(name = name, outs = [name + ".h"], cmd = cmd, **common)

    # Naming the output file rather than the rule hides the writing action from the comparison.
    header = name + ".h" if by_file else ":" + name
    cc_library(name = name + "_reader", hdrs = [header], include_prefix = include_prefix, **common)

def variant_neutral_test_suite(name):
    """Fixtures for each way a variant can reach a compile, and tests that the guard rejects them.

    Args:
      name: Name of the test suite.
    """
    common = {"tags": ["manual"], "testonly": True}

    # A dependency whose public define follows the text tier.
    cc_library(
        name = "_variant_neutral_define_reader",
        hdrs = ["testdata/variant_neutral/Reader.h"],
        defines = _tier_select(["VARIANT_NEUTRAL_TEST_TEXT_FULL"], []),
        **common
    )

    # A dependency that declares a different header in the full text tier.
    cc_library(
        name = "_variant_neutral_header_reader",
        hdrs = _tier_select(
            ["testdata/variant_neutral/ReaderFull.h"],
            ["testdata/variant_neutral/Reader.h"],
        ),
        **common
    )

    # A header generated differently per text tier, with the same path in every configuration.
    _generated_header(
        name = "_variant_neutral_generated_header",
        cmd = _tier_select(
            "echo '#define VARIANT_NEUTRAL_TEST_TIER 2' > $@",
            "echo '#define VARIANT_NEUTRAL_TEST_TIER 1' > $@",
        ),
    )

    # A generated header the comparison cannot reach and the allowlist does not name, seen through
    # _virtual_includes.
    _generated_header(
        name = "_variant_neutral_unlisted_header",
        cmd = "echo '#define VARIANT_NEUTRAL_TEST_UNLISTED 1' > $@",
        by_file = True,
        include_prefix = "variant_neutral_test",
    )

    # A make variable that follows the text tier. The library's copts attribute is the same
    # string in every configuration; only the expanded compile command differs.
    _tier_variable(name = "_variant_neutral_tier_variable", value = _tier_select("2", "1"), **common)

    _fixture(name = "_variant_neutral_clean")

    # A library that is only compatible in the full text tier still shares its compile there.
    _fixture(
        name = "_variant_neutral_tier_compatible",
        target_compatible_with = _tier_select([], ["@platforms//:incompatible"]),
    )
    _fixture(name = "_variant_neutral_dep_define", deps = [":_variant_neutral_define_reader"])
    _fixture(name = "_variant_neutral_dep_header", deps = [":_variant_neutral_header_reader"])
    _fixture(name = "_variant_neutral_generated", deps = [":_variant_neutral_generated_header_reader"])
    _fixture(name = "_variant_neutral_unlisted", deps = [":_variant_neutral_unlisted_header_reader"])
    _fixture(
        name = "_variant_neutral_own_select",
        local_defines = select({
            "//donner/svg/renderer:renderer_backend_geode": ["VARIANT_NEUTRAL_TEST_GEODE"],
            "//conditions:default": [],
        }),
    )
    _fixture(
        name = "_variant_neutral_make_variable",
        copts = ["-DVARIANT_NEUTRAL_TEST_TIER=$(VARIANT_NEUTRAL_TEST_TIER)"],
        toolchains = [":_variant_neutral_tier_variable"],
    )

    accepted = {
        "variant_neutral_links_neutral_objects_test": (_links_neutral_objects_test, "clean"),
        "variant_neutral_links_neutral_objects_geode_test": (_links_neutral_objects_geode_test, "clean"),
        "variant_neutral_compiles_in_place_without_flag_test": (_compiles_in_place_test, "clean"),
        "variant_neutral_accepts_unused_select_test": (_links_neutral_objects_geode_test, "dep_define"),
        "variant_neutral_accepts_unused_make_variable_test": (_links_neutral_objects_geode_test, "make_variable"),
        "variant_neutral_keeps_variant_compatibility_test": (_links_neutral_objects_test, "tier_compatible"),
    }

    # Real libraries: donner_cc_library shares by default, and the flag turns sharing off.
    defaults = {
        "variant_neutral_donner_library_shares_by_default_test": (
            _links_neutral_objects_by_default_test,
            "//donner/base:base",
        ),
        "variant_neutral_flag_off_compiles_in_place_test": (
            _compiles_own_sources_when_off_test,
            "//donner/base:base",
        ),
    }
    rejected = {
        "variant_neutral_rejects_dep_define_test": (
            _rejected_text_full_test,
            "dep_define",
            "deps define: VARIANT_NEUTRAL_TEST_TEXT_FULL",
        ),
        "variant_neutral_rejects_generated_header_test": (
            _rejected_text_full_test,
            "generated",
            "echo '#define VARIANT_NEUTRAL_TEST_TIER 2'",
        ),
        "variant_neutral_rejects_own_select_test": (
            _rejected_geode_test,
            "own_select",
            "local_defines: VARIANT_NEUTRAL_TEST_GEODE",
        ),
        "variant_neutral_rejects_compile_command_test": (
            _rejected_text_full_test,
            "make_variable",
            "argv: -DVARIANT_NEUTRAL_TEST_TIER=2",
        ),
        "variant_neutral_rejects_dep_header_test": (
            _rejected_text_full_test,
            "dep_header",
            "deps header: build_defs/testdata/variant_neutral/ReaderFull.h",
        ),
        "variant_neutral_rejects_unlisted_generated_header_test": (
            _rejected_default_test,
            "unlisted",
            "_variant_neutral_unlisted_header, which is written outside the deps",
        ),
    }
    for test_name, (make_test, fixture) in accepted.items():
        make_test(name = test_name, target_under_test = ":_variant_neutral_" + fixture)
    for test_name, (make_test, target) in defaults.items():
        make_test(name = test_name, target_under_test = target)
    for test_name, (make_test, fixture, message) in rejected.items():
        make_test(name = test_name, target_under_test = ":_variant_neutral_" + fixture, expected_message = message)
    _unclassified_outputs_test(name = "variant_transition_outputs_classified_test")
    native.test_suite(
        name = name,
        tests = [":" + test_name for test_name in list(accepted) + list(defaults) + list(rejected)] + [
            ":variant_transition_outputs_classified_test",
        ],
    )
