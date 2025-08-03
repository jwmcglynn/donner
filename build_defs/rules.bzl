"""
Helper rules, such as for building fuzzers.
"""

load("@bazel_tools//tools/cpp:toolchain_utils.bzl", "find_cpp_toolchain")
load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

def fuzzer_compatible_with():
    """
    Returns a list of labels that the fuzzer rules are compatible with.
    """

    # Only run on Linux, or if --config=toolchains_llvm is used.
    # Since the macOS clang is missing libclang_rt.fuzzer_osx.a, we cannot use the built-in macOS toolchain
    return select({
        "@platforms//os:linux": [],
        "//build_defs:fuzzers_enabled": [],
        "//conditions:default": ["@platforms//:incompatible"],
    })

def donner_cc_library(name, copts = [], tags = [], visibility = None, **kwargs):
    """
    Create a cc_library with donner-specific defaults.

    Args:
      name: Rule name.
      copts: List of copts.
      tags: List of tags.
      visibility: Visibility.
      **kwargs: Additional arguments, matching the implementation of cc_library.
    """

    package_path = native.package_name().split("/")
    if len(package_path) == 0:
        fail("Invalid package path: " + package_path)

    if package_path[0] != "donner" and package_path[0] != "experimental":
        fail("donner_cc_library can only be used in donner or experimental packages")

    # Tag experimental libraries
    if package_path[0] == "experimental":
        tags = tags + ["experimental"]

        # Disallow public visibility, require all paths be under //experimental
        for matcher in visibility:
            if not matcher.startswith("//experimental"):
                fail("Invalid visibility, must be under //experimental: " + matcher)

    cc_library(
        name = name,
        include_prefix = "/".join(package_path),
        copts = copts + ["-I."],
        tags = tags,
        visibility = visibility,
        **kwargs
    )

def donner_cc_fuzzer(name, corpus, **kwargs):
    """
    Create a libfuzzer-based fuzz target.

    Args:
      name: Rule name.
      corpus: Path to a corpus directory, or a filegroup rule for the corpus.
      **kwargs: Additional arguments, matching the implementation of cc_test.
    """
    if not (corpus.startswith("//") or corpus.startswith(":")):
        corpus_name = name + "_corpus"
        corpus = native.glob([corpus + "/**"])
        native.filegroup(name = corpus_name, srcs = corpus)
    else:
        corpus_name = corpus

    cc_binary(
        name = name + "_bin",
        linkopts = ["-fsanitize=fuzzer"],
        linkstatic = 1,
        target_compatible_with = fuzzer_compatible_with(),
        tags = ["fuzz_target"],
        **kwargs
    )

    cc_test(
        name = name + "_10_seconds",
        linkopts = ["-fsanitize=fuzzer"],
        args = [
            "-max_total_time=10",
            "-timeout=2",
        ],
        linkstatic = 1,
        target_compatible_with = fuzzer_compatible_with(),
        size = "large",
        data = select({
            "@platforms//os:macos": ["@llvm_toolchain//:linker-components-aarch64-darwin"],
            "//conditions:default": [],
        }),
        tags = ["fuzz_target"],
        **kwargs
    )

    cc_test(
        name = name,
        linkopts = ["-fsanitize=fuzzer"],
        args = ["$(locations %s)" % corpus_name],
        linkstatic = 1,
        data = [corpus_name] + select({
            "@platforms//os:macos": ["@llvm_toolchain//:linker-components-aarch64-darwin"],
            "//conditions:default": [],
        }),
        target_compatible_with = fuzzer_compatible_with(),
        tags = ["fuzz_target"],
        **kwargs
    )

def _force_opt_transition_impl(_settings, _attr):
    return {
        "//command_line_option:compilation_mode": "opt",
    }

_force_opt_transition = transition(
    implementation = _force_opt_transition_impl,
    inputs = [],
    outputs = ["//command_line_option:compilation_mode"],
)

def _is_compilation_outputs_empty(compilation_outputs):
    return (len(compilation_outputs.pic_objects) == 0 and
            len(compilation_outputs.objects) == 0)

def _donner_perf_sensitive_cc_library_impl(ctx):
    cc_toolchain = find_cpp_toolchain(ctx)

    feature_configuration = cc_common.configure_features(
        ctx = ctx,
        cc_toolchain = cc_toolchain,
        requested_features = ctx.features,
        unsupported_features = ctx.disabled_features,
    )

    compilation_contexts = [dep[CcInfo].compilation_context for dep in ctx.attr.deps]
    compilation_context, compilation_outputs = cc_common.compile(
        name = ctx.label.name,
        actions = ctx.actions,
        feature_configuration = feature_configuration,
        cc_toolchain = cc_toolchain,
        srcs = ctx.attr.srcs,
        includes = ctx.attr.includes,
        defines = ctx.attr.defines,
        local_defines = ctx.attr.local_defines,
        public_hdrs = ctx.attr.hdrs,
        compilation_contexts = compilation_contexts,
    )

    linking_contexts = [dep[CcInfo].linking_context for dep in ctx.attr.deps]

    # Only create linking context if there are compiled artifacts
    if not _is_compilation_outputs_empty(compilation_outputs):
        linking_context, linking_outputs = cc_common.create_linking_context_from_compilation_outputs(
            actions = ctx.actions,
            feature_configuration = feature_configuration,
            cc_toolchain = cc_toolchain,
            compilation_outputs = compilation_outputs,
            user_link_flags = ctx.attr.linkopts,
            name = ctx.label.name,
            language = "c++",
        )

        if linking_outputs.library_to_link != None:
            linking_contexts.append(linking_context)

    cc_info = CcInfo(
        compilation_context = compilation_context,
        linking_context = cc_common.merge_linking_contexts(
            linking_contexts = linking_contexts,
        ),
    )

    return [cc_info]

_donner_perf_sensitive_cc_library = rule(
    implementation = _donner_perf_sensitive_cc_library_impl,
    toolchains = ["@bazel_tools//tools/cpp:toolchain_type"],
    attrs = {
        "srcs": attr.label_list(allow_files = [".c", ".cc", ".cpp", ".h"]),
        "hdrs": attr.label_list(allow_files = [".h"]),
        "deps": attr.label_list(cfg = _force_opt_transition),
        "includes": attr.string_list(default = []),  # Optional includes
        "defines": attr.string_list(default = []),  # Optional defines
        "local_defines": attr.string_list(default = []),  # Optional defines
        "copts": attr.string_list(default = []),  # Optional compile options
        "linkopts": attr.string_list(default = []),  # Optional link options
    },
    fragments = ["cpp"],
)

def donner_perf_sensitive_cc_library(name, allow_debug_builds_config = None, **kwargs):
    """
    Wrapper around a cc_library that is always compiled with optimizations.

    By default, this rule is always compiled in "opt" mode, regardless of the
    --compilation_mode flag. This is useful for performance-sensitive code that
    should not be run in debug builds, such as benchmarks or core rendering code.

    If `allow_debug_builds_config` is set, this creates a configurable target
    that will switch between the optimized and unconfigured versions of the library.
    This is useful for tests which may want to run in debug mode.

    Valid `allow_debug_builds_config` values are `config_setting` rules,
    e.g. something that is valid as a select() key.

    Args:
      name: Rule name.
      allow_debug_builds_config: A `selects.config_setting` that, if enabled,
        will allow this library to be built in debug mode.
      **kwargs: Additional arguments, matching the implementation of cc_library.
    """
    if allow_debug_builds_config != None:
        _donner_perf_sensitive_cc_library(
            name = name + "_opt",
            **kwargs
        )

        cc_library(
            name = name + "_unconfigured",
            **kwargs
        )

        cc_library(
            name = name,
            deps = select({
                allow_debug_builds_config: [":" + name + "_unconfigured"],
                "//conditions:default": [":" + name + "_opt"],
            }),
            visibility = ["//donner:__subpackages__"],
            tags = ["perf_sensitive"],
        )
    else:
        _donner_perf_sensitive_cc_library(
            name = name,
            tags = ["perf_sensitive"],
            **kwargs
        )
