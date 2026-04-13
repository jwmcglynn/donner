"""
Helper rules, such as for building fuzzers.
"""

load("@bazel_tools//tools/cpp:toolchain_utils.bzl", "find_cpp_toolchain")
load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")
load("@rules_python//python:defs.bzl", "py_test")

# Script that enforces banned source patterns (no `long long`, no
# `std::aligned_storage`, no user-defined literal operators). See
# docs/coding_style.md "Language and Library Features".
_BANNED_PATTERNS_SCRIPT = "//build_defs:check_banned_patterns.py"

def _banned_patterns_lint_test(name, srcs, hdrs, tags = [], **_kwargs):
    """Emit a py_test that runs check_banned_patterns.py on srcs + hdrs.

    One lint test is emitted per donner_cc_{library,test,binary} via this
    helper, so `bazel test //...` catches new banned patterns automatically.
    The test is tagged `lint` so it can be filtered if desired.

    Args:
      name: Parent target name. The lint test is named `{name}_lint`.
      srcs: Source files to lint.
      hdrs: Header files to lint.
      tags: Tags from the parent rule. `manual` is propagated so libraries
        tagged manual don't pull their lint into `bazel test //...`.
    """

    # select()-valued srcs/hdrs can't be enumerated at load time; skip linting
    # them here. Those files are still linted whenever another target references
    # them as a plain list.
    if type(srcs) != "list" or type(hdrs) != "list":
        return

    # Only lint files we own in this package (string entries). Label-form
    # srcs (e.g. ":generated_header") come from other rules and are skipped.
    lintable = [f for f in (srcs + hdrs) if type(f) == "string" and not f.startswith(":") and not f.startswith("//")]
    if not lintable:
        return

    propagated_tags = ["lint", "banned_patterns"]
    if "manual" in tags:
        propagated_tags.append("manual")

    py_test(
        name = name + "_lint",
        srcs = [_BANNED_PATTERNS_SCRIPT],
        main = "check_banned_patterns.py",
        args = ["$(rootpath {})".format(f) for f in lintable],
        data = lintable,
        tags = propagated_tags,
        size = "small",
    )

def llvm21_macos_workaround_linkopts():
    """
    Returns linkopts needed for LLVM 21 __hash_memory symbol workaround on macOS.

    See: https://github.com/llvm/llvm-project/issues/155606
    The fuzzer runtime and other LLVM 21 compiled code needs symbols from libc++ 21,
    but the linker finds the system libc++. We explicitly link against LLVM 21's static libraries.
    """
    return select({
        "//build_defs:llvm_latest_macos": [
            "-nostdlib++",
            "external/toolchains_llvm++llvm+llvm_toolchain_llvm/lib/libc++.a",
            "external/toolchains_llvm++llvm+llvm_toolchain_llvm/lib/libc++abi.a",
        ],
        "//conditions:default": [],
    })

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

def renderer_backend_compatible_with(backends):
    """
    Returns compatibility constraints for renderer backend-specific targets.

    Args:
      backends: List of supported backend names. Valid values are "skia",
        "tiny_skia", and "geode".
    """
    conditions = {}
    remaining = list(backends)

    if "skia" in remaining:
        conditions["//donner/svg/renderer:renderer_backend_skia"] = []
        remaining.remove("skia")

    if "tiny_skia" in remaining:
        conditions["//donner/svg/renderer:renderer_backend_tiny_skia"] = []
        remaining.remove("tiny_skia")

    if "geode" in remaining:
        conditions["//donner/svg/renderer:renderer_backend_geode"] = []
        remaining.remove("geode")

    if remaining:
        fail("Unknown renderer backend(s): " + ", ".join(remaining))

    if not conditions:
        fail("renderer_backend_compatible_with requires at least one backend")

    conditions["//conditions:default"] = ["@platforms//:incompatible"]
    return select(conditions)

def _renderer_backend_transition_impl(settings, attr):
    if settings["//build_defs:disable_backend_test_transition"]:
        return {
            "//donner/svg/renderer:renderer_backend": settings["//donner/svg/renderer:renderer_backend"],
        }

    return {
        "//donner/svg/renderer:renderer_backend": attr.renderer_backend,
    }

_renderer_backend_transition = transition(
    implementation = _renderer_backend_transition_impl,
    inputs = [
        "//build_defs:disable_backend_test_transition",
        "//donner/svg/renderer:renderer_backend",
    ],
    outputs = ["//donner/svg/renderer:renderer_backend"],
)

def _donner_transitioned_executable_impl(ctx):
    dep_target = ctx.attr.dep
    if type(dep_target) == "list":
        if len(dep_target) != 1:
            fail("dep transition produced {} targets, expected 1".format(len(dep_target)))
        dep_target = dep_target[0]

    dep_default_info = dep_target[DefaultInfo]
    files_to_run = dep_default_info.files_to_run
    if files_to_run == None or files_to_run.executable == None:
        fail("dep must be an executable target: {}".format(dep_target.label))

    executable = ctx.actions.declare_file(ctx.label.name)
    ctx.actions.symlink(
        output = executable,
        target_file = files_to_run.executable,
        is_executable = True,
    )

    providers = [
        DefaultInfo(
            executable = executable,
            files = depset([executable], transitive = [dep_default_info.files]),
            runfiles = dep_default_info.default_runfiles,
        ),
    ]

    # Forward InstrumentedFilesInfo so that `bazel coverage` collects coverage
    # data for transitioned test targets.
    if InstrumentedFilesInfo in dep_target:
        providers.append(dep_target[InstrumentedFilesInfo])

    return providers

donner_transitioned_cc_test = rule(
    implementation = _donner_transitioned_executable_impl,
    test = True,
    attrs = {
        "dep": attr.label(
            mandatory = True,
            executable = True,
            cfg = _renderer_backend_transition,
        ),
        "renderer_backend": attr.string(
            mandatory = True,
            values = ["skia", "tiny_skia", "geode"],
        ),
    },
)

def _multi_transition_impl(settings, attr):
    if settings["//build_defs:disable_backend_test_transition"]:
        return {
            "//donner/svg/renderer:renderer_backend": settings["//donner/svg/renderer:renderer_backend"],
            "//donner/svg/renderer:text": settings["//donner/svg/renderer:text"],
            "//donner/svg/renderer:text_full": settings["//donner/svg/renderer:text_full"],
            "//donner/svg/renderer/geode:enable_dawn": settings["//donner/svg/renderer/geode:enable_dawn"],
        }

    # Selecting the geode backend implies turning on Dawn: the
    # `:renderer_geode` library gates its sources behind the
    # `enable_dawn` flag, so the transition must set it to keep the
    # dependency graph buildable without the user also passing
    # `--config=geode` on the command line.
    return {
        "//donner/svg/renderer:renderer_backend": attr.renderer_backend,
        "//donner/svg/renderer:text": attr.text == "true" or attr.text_full == "true",
        "//donner/svg/renderer:text_full": attr.text_full == "true",
        "//donner/svg/renderer/geode:enable_dawn": attr.renderer_backend == "geode",
    }

_multi_transition = transition(
    implementation = _multi_transition_impl,
    inputs = [
        "//build_defs:disable_backend_test_transition",
        "//donner/svg/renderer:renderer_backend",
        "//donner/svg/renderer:text",
        "//donner/svg/renderer:text_full",
        "//donner/svg/renderer/geode:enable_dawn",
    ],
    outputs = [
        "//donner/svg/renderer:renderer_backend",
        "//donner/svg/renderer:text",
        "//donner/svg/renderer:text_full",
        "//donner/svg/renderer/geode:enable_dawn",
    ],
)

donner_multi_transitioned_test = rule(
    implementation = _donner_transitioned_executable_impl,
    test = True,
    attrs = {
        "dep": attr.label(
            mandatory = True,
            executable = True,
            cfg = _multi_transition,
        ),
        "renderer_backend": attr.string(
            mandatory = True,
            values = ["skia", "tiny_skia", "geode"],
        ),
        "text": attr.string(
            default = "false",
            values = ["true", "false"],
        ),
        "text_full": attr.string(
            default = "false",
            values = ["true", "false"],
        ),
    },
)

def donner_variant_cc_test(name, dep, variants = None, named_variants = None, **kwargs):
    """
    Generate test targets for variant configurations, plus a default alias
    that inherits the active command-line config.

    Supports two calling conventions:
      1. **named_variants** (preferred): A list of dicts, each with keys
         "name", "backend", and optionally "text" / "text_full".
      2. **variants** (legacy Cartesian product): A list of axis lists,
         e.g. [["tiny_skia", "skia"], ["text", "text_full"]].

    Args:
      name: Base name for the generated targets.
      dep: The test implementation target (tagged manual).
      variants: (legacy) List of variant axis lists.
      named_variants: List of dicts describing each variant explicitly.
      **kwargs: Additional arguments passed to the generated test rules.

    Generated targets:
      {name}                  - alias to the default (no transition, uses active config)
      {name}_{variant_name}   - explicit variant
    """
    if named_variants:
        for v in named_variants:
            target_name = "{}_{}".format(name, v["name"])
            donner_multi_transitioned_test(
                name = target_name,
                dep = dep,
                renderer_backend = v["backend"],
                text = v.get("text", "false"),
                text_full = v.get("text_full", "false"),
                testonly = 1,
                **kwargs
            )
    elif variants:
        backends = variants[0] if len(variants) > 0 else ["tiny_skia"]
        text_tiers = variants[1] if len(variants) > 1 else ["text"]

        for backend in backends:
            for tier in text_tiers:
                suffix = "{}_{}".format(backend, tier)
                target_name = "{}_{}".format(name, suffix)
                text_val = "true" if tier in ["text", "text_full"] else "false"
                text_full_val = "true" if tier == "text_full" else "false"

                donner_multi_transitioned_test(
                    name = target_name,
                    dep = dep,
                    renderer_backend = backend,
                    text = text_val,
                    text_full = text_full_val,
                    testonly = 1,
                    **kwargs
                )

    # Default alias: uses no transition, inherits active command-line config.
    native.alias(
        name = name,
        actual = dep,
        testonly = 1,
    )

def donner_cc_binary(name, srcs = [], linkopts = [], tags = [], **kwargs):
    """
    Create a cc_binary with donner-specific defaults including LLVM 21 workaround.

    Args:
      name: Rule name.
      srcs: Source files.
      linkopts: List of linker options.
      tags: Tags.
      **kwargs: Additional arguments, matching the implementation of cc_binary.
    """
    cc_binary(
        name = name,
        srcs = srcs,
        linkopts = linkopts + llvm21_macos_workaround_linkopts(),
        tags = tags,
        **kwargs
    )

    _banned_patterns_lint_test(
        name = name,
        srcs = srcs,
        hdrs = kwargs.get("hdrs", []),
        tags = tags,
    )

def donner_cc_test(name, srcs = [], linkopts = [], tags = [], **kwargs):
    """
    Create a cc_test with donner-specific defaults including LLVM 21 workaround.

    Args:
      name: Rule name.
      srcs: Source files.
      linkopts: List of linker options.
      tags: Tags.
      **kwargs: Additional arguments, matching the implementation of cc_test.
    """
    cc_test(
        name = name,
        srcs = srcs,
        linkopts = linkopts + llvm21_macos_workaround_linkopts(),
        tags = tags,
        **kwargs
    )

    _banned_patterns_lint_test(
        name = name,
        srcs = srcs,
        hdrs = kwargs.get("hdrs", []),
        tags = tags,
    )

def donner_cc_library(name, srcs = [], hdrs = [], copts = [], tags = [], visibility = None, **kwargs):
    """
    Create a cc_library with donner-specific defaults.

    Args:
      name: Rule name.
      srcs: Source files.
      hdrs: Header files.
      copts: List of copts.
      tags: List of tags.
      visibility: Visibility.
      **kwargs: Additional arguments, matching the implementation of cc_library.
    """

    package_path = native.package_name().split("/")
    if len(package_path) == 0:
        fail("Invalid package path: " + package_path)

    if package_path[0] != "" and package_path[0] != "donner" and package_path[0] != "experimental":
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
        srcs = srcs,
        hdrs = hdrs,
        include_prefix = "/".join(package_path),
        copts = copts + ["-I."],
        tags = tags,
        visibility = visibility,
        **kwargs
    )

    _banned_patterns_lint_test(
        name = name,
        srcs = srcs,
        hdrs = hdrs,
        tags = tags,
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

    # Build linkopts for fuzzer, including LLVM 21 workaround and runtime paths
    fuzzer_linkopts = ["-fsanitize=fuzzer"] + llvm21_macos_workaround_linkopts() + select({
        "@platforms//os:macos": [
            # Add rpath for execroot (from bin directory to external/)
            "-Wl,-rpath,@loader_path/../../../../../../external/toolchains_llvm++llvm+llvm_toolchain_llvm/lib/clang/21/lib/darwin",
            # Add rpath for runfiles directory (without 'external/' prefix)
            "-Wl,-rpath,@loader_path/../../../../toolchains_llvm++llvm+llvm_toolchain_llvm/lib/clang/21/lib/darwin",
        ],
        "//conditions:default": [],
    })

    cc_binary(
        name = name + "_bin",
        linkopts = fuzzer_linkopts,
        linkstatic = 1,
        target_compatible_with = fuzzer_compatible_with(),
        tags = ["fuzz_target"],
        **kwargs
    )

    donner_cc_test(
        name = name + "_10_seconds",
        linkopts = fuzzer_linkopts,
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

    donner_cc_test(
        name = name,
        linkopts = fuzzer_linkopts,
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

def _force_opt_transition_impl(settings, _attr):
    if settings["//build_defs:disable_perf_opt_transition"]:
        return {}
    return {
        "//command_line_option:compilation_mode": "opt",
    }

_force_opt_transition = transition(
    implementation = _force_opt_transition_impl,
    inputs = ["//build_defs:disable_perf_opt_transition"],
    outputs = ["//command_line_option:compilation_mode"],
)

def _is_compilation_outputs_empty(compilation_outputs):
    return (len(compilation_outputs.pic_objects) == 0 and
            len(compilation_outputs.objects) == 0)

def _donner_perf_sensitive_cc_library_impl(ctx):
    cc_toolchain = find_cpp_toolchain(ctx)

    # Request the 'opt' feature for optimized compilation without changing the
    # configuration of transitive deps (which would cause shared-library link
    # conflicts between opt and fastbuild configurations of the same dep).
    # Explicitly unsupport 'fastbuild' and 'dbg' to avoid
    # variant:crosstool_build_mode conflict on Emscripten toolchain.
    feature_configuration = cc_common.configure_features(
        ctx = ctx,
        cc_toolchain = cc_toolchain,
        requested_features = ctx.features + ["opt"],
        unsupported_features = ctx.disabled_features + ["fastbuild", "dbg"],
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
        "deps": attr.label_list(),
        "includes": attr.string_list(default = []),  # Optional includes
        "defines": attr.string_list(default = []),  # Optional defines
        "local_defines": attr.string_list(default = []),  # Optional defines
        "copts": attr.string_list(default = []),  # Optional compile options
        "linkopts": attr.string_list(default = []),  # Optional link options
    },
    fragments = ["cpp"],
)

def donner_perf_sensitive_cc_library(name, allow_debug_builds_config = None, target_compatible_with = None, **kwargs):
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
      target_compatible_with: Optional platform compatibility constraints, propagated
        to all generated sub-targets.
      **kwargs: Additional arguments, matching the implementation of cc_library.
    """
    compat = {}
    if target_compatible_with != None:
        compat["target_compatible_with"] = target_compatible_with
        kwargs["target_compatible_with"] = target_compatible_with

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
            **compat
        )
    else:
        _donner_perf_sensitive_cc_library(
            name = name,
            tags = ["perf_sensitive"],
            **kwargs
        )
