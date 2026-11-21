"""
Helper rules, such as for building fuzzers.
"""

load("@bazel_features//:features.bzl", "bazel_features")
load("//build_defs:ci_darwin.bzl", "declare_darwin_variants")
load("//build_defs:ci_linux_gpu.bzl", "linux_gpu_variant")
load("//build_defs:ci_xcode.bzl", "ci_xcode_identity")
load("//build_defs:wasm_profiles.bzl", "WASM_OPTION_INPUTS", "WASM_OPTION_OUTPUTS", "wasm_profile_options")
load("@bazel_tools//tools/cpp:toolchain_utils.bzl", "find_cpp_toolchain")
load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")
load("//build_defs:performance.bzl", "native_speed_copts")
load("//build_defs:variant_neutral.bzl", "variant_neutral_srcs")
load("//build_defs:variant_settings.bzl", "variant_neutral_default", "variant_transition")
load(
    "//tools/format:clang_format_check.bzl",
    "clang_format_check",
    "clang_format_check_compatible_with",
)

def _source_format_check(name, srcs, hdrs, **_kwargs):
    """Emits `{name}_format`, which fails the build when a listed source is not clang-formatted.

    The check is a validation action (//tools/format:clang_format_check.bzl), not a test: one
    cached action per target, so in donner's own builds `bazel build //...` and
    `bazel test //...` both enforce formatting without a test runner per target. Only plain
    file paths of donner_cc_library, donner_cc_test and donner_cc_binary are checked;
    `select()`-valued lists and labels are skipped, and lanes with `--build_tests_only` skip
    the check. The Lint lane's `tools/lint.sh` formats the whole tracked tree with the same
    binary, including everything else.

    The banned-patterns scan that used to be emitted here as one py_test per target is the
    repo-wide `tools/lint.sh` scan: 476 per-target tests cost 31% of the suite's CPU time for a
    regex pass that takes seconds in one process.

    Args:
      name: Parent target name.
      srcs: Source files.
      hdrs: Header files.
      **_kwargs: Unused.
    """
    files = []
    for group in (srcs, hdrs):
        if type(group) != "list":
            continue
        files.extend([
            f
            for f in group
            if type(f) == "string" and ":" not in f and not f.startswith(("//", "@"))
        ])
    if not files:
        return
    clang_format_check(
        name = name + "_format",
        srcs = files,
        testonly = True,
        # Off unless --//tools/format:check, which only donner's own .bazelrc sets.
        target_compatible_with = clang_format_check_compatible_with(),
        visibility = ["//visibility:private"],
    )

def libc_compat_deps():
    """
    Returns extra deps needed when linking against the hermetic LLVM toolchain
    on Linux.

    Chromium's Debian Bullseye sysroot (wired into `llvm_toolchain` via
    `//third_party/bazel/non_bcr_deps.bzl`) exports `copy_file_range@GLIBC_2.27`
    as a non-default-versioned symbol, so unversioned references from
    `toolchains_llvm`'s prebuilt libc++.a don't auto-resolve. The tiny shim in
    `//third_party/libc_compat` provides an unversioned `copy_file_range`
    that invokes the kernel without rebinding to itself.
    """
    return select({
        "//build_defs:llvm_latest_linux": ["//third_party/libc_compat:libc_compat"],
        "//build_defs:llvm_latest_macos": ["//third_party/libc_compat:libcxx_macos_compat"],
        "//conditions:default": [],
    })

def test_crash_handler_deps():
    """
    Returns the alwayslink dep that installs a crash signal handler in a test
    binary.

    Upstream `gtest_main` installs none, so a test killed by SIGSEGV leaves a log
    that stops at its `[ RUN ]` line with no stack - the crash is then only
    diagnosable by reproducing it, which a CI-only failure by definition resists.
    Linked into every `donner_cc_test` so the first occurrence is the one that
    gets diagnosed.

    Restricted to platforms whose libc provides `execinfo.h`/`dladdr`; Emscripten
    and other targets fall back to no handler.
    """
    return select({
        "@platforms//os:linux": ["//donner/base:test_crash_handler"],
        "@platforms//os:macos": ["//donner/base:test_crash_handler"],
        "//conditions:default": [],
    })

def fuzzer_compatible_with():
    """
    Returns a list of labels that the fuzzer rules are compatible with.
    """
    return select({
        "@platforms//os:linux": [],
        "//build_defs:fuzzers_enabled": [],
        "//conditions:default": ["@platforms//:incompatible"],
    })

def fuzzer_linkopts():
    """
    Returns linkopts for libFuzzer targets.
    """
    llvm_lib = "external/toolchains_llvm++llvm+llvm_toolchain_llvm/lib"
    return select({
        "//build_defs:llvm_latest_macos": [
            "-fsanitize=fuzzer-no-link",
            llvm_lib + "/clang/23/lib/darwin/libclang_rt.fuzzer_osx.a",
            # Use the platform C++ runtime; bundled LLVM archives target a newer macOS.
        ],
        "//conditions:default": ["-fsanitize=fuzzer"],
    })

def fuzzer_linker_inputs():
    """
    Returns additional linker inputs for libFuzzer targets.
    """
    return select({
        "//build_defs:llvm_latest_macos": ["@llvm_toolchain//:linker-components-aarch64-darwin"],
        "//conditions:default": [],
    })

def renderer_backend_compatible_with(backends):
    """
    Returns compatibility constraints for renderer backend-specific targets.

    Args:
      backends: List of supported backend names. Valid values are
        "tiny_skia" and "geode".
    """
    conditions = {}
    remaining = list(backends)

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

_renderer_backend_transition = variant_transition(
    implementation = _renderer_backend_transition_impl,
    inputs = [
        "//build_defs:disable_backend_test_transition",
        "//donner/svg/renderer:renderer_backend",
    ],
    outputs = ["//donner/svg/renderer:renderer_backend"],
)

def _transitioned_fixed_args(ctx):
    result = []
    for arg in getattr(ctx.attr, "fixed_args", []):
        if "$(locations " in arg:
            fail("Pass file groups through file_args so each path remains a separate argument")
        result.append(ctx.expand_location(arg, targets = ctx.attr.argument_files))
    result += [file.short_path for file in getattr(ctx.files, "file_args", [])]
    return " ".join([_shell_quote(arg) for arg in result])

DonnerTransitionedBinaryInfo = provider(
    doc = "The linked binary behind a product-configured executable wrapper.",
    fields = {"binary": "The linked executable File, with the requested build transition."},
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
    launcher_target = "_donner_transitioned/{}/{}".format(ctx.label.package, ctx.label.name)
    declared_data = depset(transitive = [
        dep_default_info.default_runfiles.files,
        dep_default_info.data_runfiles.files,
    ]).to_list()
    companions = [file for file in declared_data if file.is_directory and file.basename == files_to_run.executable.basename + ".assets"]
    if not companions:
        companions = [file for file in declared_data if file.is_directory and file.basename == "donner-assets"]
    if len(companions) > 1:
        fail("ambiguous packaged resource companions for {}".format(dep_target.label))
    root_symlinks = {launcher_target: files_to_run.executable}
    if companions:
        # Remote runfiles may materialize the executable alias as an ordinary file.
        root_symlinks[launcher_target + ".assets"] = companions[0]
    forwarded_runfiles = ctx.runfiles(
        root_symlinks = root_symlinks,
    ).merge(dep_default_info.default_runfiles).merge(dep_default_info.data_runfiles)

    if getattr(ctx.attr, "required_outputs", []):
        forwarded_runfiles = forwarded_runfiles.merge(ctx.runfiles(transitive_files = depset(
            transitive = [target[DefaultInfo].files for target in ctx.attr.required_outputs],
        )))

    # Do not forward the transitioned executable as a plain symlink. Native
    # binaries encode their own target basename in a runfiles-relative rpath
    # (for example, `foo_impl.runfiles/...`). A wrapper named `foo` gets a
    # `foo.runfiles` tree instead, so a clean remote worker cannot find shared
    # libraries even though they are declared runfiles. Local output-tree
    # `_solib` residue can accidentally mask that mismatch.
    #
    # Execute the implementation through a stable root runfiles alias and add
    # every declared `_solib_*` leaf to the platform loader path. This keeps
    # transitioned tests hermetic on clean remote workers and also works for
    # `bazel run` wrappers. Manifest-only runfiles are supported as a fallback.
    launcher = """#!/usr/bin/env bash
set -euo pipefail

logical_impl={logical_impl}
runfiles_dir="${{RUNFILES_DIR:-${{TEST_SRCDIR:-}}}}"
manifest="${{RUNFILES_MANIFEST_FILE:-}}"
if [[ -z "$runfiles_dir" && -d "$0.runfiles" ]]; then
  runfiles_dir="$0.runfiles"
fi
if [[ -z "$manifest" && -f "$0.runfiles_manifest" ]]; then
  manifest="$0.runfiles_manifest"
fi

impl=""
library_path=""
append_library_dir() {{
  local candidate="$1"
  case ":$library_path:" in
    *":$candidate:"*) ;;
    *) library_path="${{library_path:+$library_path:}}$candidate" ;;
  esac
}}

if [[ -n "$runfiles_dir" && -e "$runfiles_dir/$logical_impl" ]]; then
  impl="$runfiles_dir/$logical_impl"
  for candidate in "$runfiles_dir"/*/_solib_*/*; do
    if [[ -d "$candidate" ]]; then
      append_library_dir "$candidate"
    else
      case "$candidate" in
        *.dylib|*.so)
          # Flat _solib layouts place libraries directly in _solib_*; append
          # the file's parent, as the manifest branch does.
          append_library_dir "${{candidate%/*}}"
          ;;
      esac
    fi
  done
elif [[ -n "$manifest" && -f "$manifest" ]]; then
  while IFS=' ' read -r logical physical; do
    if [[ "$logical" == "$logical_impl" ]]; then
      impl="$physical"
    fi
    case "$logical" in
      */_solib_*/*.dylib|*/_solib_*/*.so)
        append_library_dir "${{physical%/*}}"
        ;;
    esac
  done < "$manifest"
fi

if [[ -z "$impl" ]]; then
  echo "Unable to locate transitioned executable runfile: $logical_impl" >&2
  exit 127
fi
if [[ -n "$library_path" ]]; then
  export DYLD_LIBRARY_PATH="$library_path${{DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}}"
  export LD_LIBRARY_PATH="$library_path${{LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}}"
fi
# argv[0] is the inner alias, so child libraries cannot rediscover the wrapper's tree.
if [[ -n "$runfiles_dir" ]]; then
  export RUNFILES_DIR="$runfiles_dir"
fi
if [[ -n "$manifest" ]]; then
  export RUNFILES_MANIFEST_FILE="$manifest"
fi
exec "$impl" {fixed_args} "$@"
""".format(
        logical_impl = launcher_target,
        fixed_args = _transitioned_fixed_args(ctx),
    )
    ctx.actions.write(executable, launcher, is_executable = True)

    providers = [
        DefaultInfo(
            executable = executable,
            files = depset([executable], transitive = [dep_default_info.files]),
            runfiles = forwarded_runfiles,
        ),
        DonnerTransitionedBinaryInfo(binary = files_to_run.executable),
    ]

    # Forward InstrumentedFilesInfo so that `bazel coverage` collects coverage
    # data for transitioned test targets.
    if InstrumentedFilesInfo in dep_target:
        providers.append(dep_target[InstrumentedFilesInfo])

    # Forward the dep's run environment so `env`/`env_inherit` set on the
    # underlying test impl actually take effect on the transitioned wrapper.
    # Without this, the variant targets silently drop the impl's `env` (e.g.
    # the resvg suite's per-case watchdog override) - the symlinked binary
    # would run with a bare environment.
    profile = getattr(ctx.attr, "darwin_profile", "")
    fixed = getattr(ctx.attr, "fixed_test_env", {})
    if getattr(ctx.attr, "metal_validation", False) or profile:
        fixed = {
            "DONNER_AUTOMATED_LANE": "1",
            "DONNER_CI_MACOS_LANE": "native-metal",
            "HOME": "",
            "MTL_SHADER_VALIDATION_TEXTURE_USAGE": "1",
        }
        if profile:
            fixed["DONNER_CI_MACOS_LANE"] = "native-desktop-browser" if profile == "desktop" else "self-hosted-vm"
            fixed["MTL_SHADER_VALIDATION_TEXTURE_USAGE"] = "0"
    if fixed:
        environment = dep_target[RunEnvironmentInfo] if RunEnvironmentInfo in dep_target else None
        providers.append(RunEnvironmentInfo(
            environment = (environment.environment if environment else {}) | fixed,
            inherited_environment = [k for k in environment.inherited_environment if k not in fixed] if environment else [],
        ))
    elif RunEnvironmentInfo in dep_target:
        providers.append(dep_target[RunEnvironmentInfo])

    return providers

# Implicit attributes Bazel's test machinery requires before it will
# post-process coverage for a Starlark test rule. Forwarding
# InstrumentedFilesInfo (above) only marks the wrapper as instrumented; the
# test runner separately reads these rule attributes to set LCOV_MERGER and
# CC_CODE_COVERAGE_SCRIPT in the collection environment. Without them,
# collect_coverage.sh sees LCOV_MERGER unset, touches an EMPTY coverage.dat,
# and exits 0, so an executed, instrumented test silently contributes no line
# data: the .profraw files its binary wrote are never merged. A coverage run
# whose test selection consists only of transitioned tests then yields an LCOV
# report with per-file records but zero DA/LF lines, which
# tools/check_lcov_report.py rejects as "no executable line data".
_TRANSITIONED_TEST_COVERAGE_ATTRS = {
    "_collect_cc_coverage": attr.label(
        default = "@bazel_tools//tools/test:collect_cc_coverage",
        cfg = "exec",
        executable = True,
    ),
    "_lcov_merger": attr.label(
        default = configuration_field(fragment = "coverage", name = "output_generator"),
        cfg = "exec",
        executable = True,
    ),
}

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
            values = ["tiny_skia", "geode"],
        ),
    } | _TRANSITIONED_TEST_COVERAGE_ATTRS,
)

def _multi_transition_impl(settings, attr):
    disabled = settings["//build_defs:disable_backend_test_transition"]
    preserve_backend = disabled or attr.renderer_backend == "inherit"
    backend = settings["//donner/svg/renderer:renderer_backend"] if preserve_backend else attr.renderer_backend
    text = settings["//donner/svg/renderer:text"]
    text_full = settings["//donner/svg/renderer:text_full"]
    if attr.full_text_only:
        text = True
        text_full = True
    elif not disabled:
        text = attr.text == "true" or attr.text_full == "true"
        text_full = attr.text_full == "true"
    return {
        "//donner/svg/renderer:renderer_backend": backend,
        "//donner/svg/renderer:text": text,
        "//donner/svg/renderer:text_full": text_full,
        "//donner/svg/renderer/geode:enable_geode": settings["//donner/svg/renderer/geode:enable_geode"] if preserve_backend else backend == "geode",
    }

_multi_transition = variant_transition(
    implementation = _multi_transition_impl,
    inputs = [
        "//build_defs:disable_backend_test_transition",
        "//donner/svg/renderer:renderer_backend",
        "//donner/svg/renderer:text",
        "//donner/svg/renderer:text_full",
        "//donner/svg/renderer/geode:enable_geode",
    ],
    outputs = [
        "//donner/svg/renderer:renderer_backend",
        "//donner/svg/renderer:text",
        "//donner/svg/renderer:text_full",
        "//donner/svg/renderer/geode:enable_geode",
    ],
)

_donner_multi_transitioned_test = rule(
    implementation = _donner_transitioned_executable_impl,
    test = True,
    attrs = {
        "fixed_args": attr.string_list(),
        "file_args": attr.label_list(allow_files = True),
        "argument_files": attr.label_list(allow_files = True),
        "dep": attr.label(
            mandatory = True,
            executable = True,
            cfg = _multi_transition,
        ),
        "renderer_backend": attr.string(
            mandatory = True,
            values = ["tiny_skia", "geode", "inherit"],
        ),
        "full_text_only": attr.bool(default = False),
        "text": attr.string(
            default = "false",
            values = ["true", "false"],
        ),
        "text_full": attr.string(
            default = "false",
            values = ["true", "false"],
        ),
    } | _TRANSITIONED_TEST_COVERAGE_ATTRS,
)

# Tag for a test that opens a GPU device. It takes one unit of the `local_gpu` resource while
# the test runs locally. .bazelrc declares one unit by default, so GPU-opening tests run one at
# a time, alongside CPU-only tests: a hardware adapter is process-global, and this keeps a device
# loss in one test process from cascading into another. The main.yml macOS lanes declare four units
# (--config=ci-macos), so up to four run at once there. Remote execution does not use local
# resources. Bazel ignores resource tags when --local_test_jobs is set, so a command that sets it
# uses 1, which runs every local test serially, or runs no GPU-opening test.
# //build_defs:geode_test_scheduling_tests enforces this.
LOCAL_GPU_TEST_TAG = "resources:local_gpu:1"

def donner_multi_transitioned_test(
        name,
        dep,
        renderer_backend,
        opens_gpu_device = False,
        remote_parallel_ci = False,
        linux_hardware_gpu = False,
        **kwargs):
    """Create a transitioned test with an honest short-runtime default.

    Args:
      name: Rule name.
      dep: The underlying test target to transition.
      renderer_backend: "tiny_skia", "geode", or "inherit" to preserve the caller backend.
      opens_gpu_device: True when the test actually instantiates a GPU
        device/adapter. Only those tests get `LOCAL_GPU_TEST_TAG`, which takes
        one unit of the machine's `local_gpu` resource (.bazelrc) while the
        test runs locally. With the default single unit, at most one
        GPU-opening test runs at a time and a device loss in one process
        cannot cascade into another; the main.yml macOS lanes allow four. Unlike
        `exclusive-if-local`, the resource does not stop CPU-only tests from
        running alongside, so these tests overlap the parallel part of the
        test phase instead of draining one at a time after it. Selecting the
        geode BACKEND is not the same thing as opening a device: most variant
        targets link wgpu-native and never touch it (they use
        MockRendererInterface, or assert on parsing/ECS/CSS/text-shaping), and
        they need no resource.
      remote_parallel_ci: Emit a second `no-local` wrapper for remote CI. The
        ordinary wrapper keeps local device-loss isolation; the remote wrapper
        shares its transitioned executable and test attributes but carries
        `ci-remote-gpu` and `no-local` instead of the local GPU tags.
      linux_hardware_gpu: Also register this GPU-opening test on the Linux hardware platform.
      **kwargs: Additional arguments for the underlying test rule.
    """
    if "size" not in kwargs and "timeout" not in kwargs:
        kwargs["size"] = "small"
    if linux_hardware_gpu:
        if not opens_gpu_device or renderer_backend != "geode":
            fail("Linux hardware requires a GPU-opening Geode test: " + name)
        kwargs["tags"] = kwargs.get("tags", []) + ["ci-linux-hardware-gpu-source"]
        linux_gpu_variant(ci_platform_test, name, kwargs)
    if renderer_backend == "geode" and opens_gpu_device:
        tags = kwargs.get("tags", [])
        if remote_parallel_ci and "local-gpu-isolated" not in tags:
            tags = tags + ["local-gpu-isolated"]
        if LOCAL_GPU_TEST_TAG not in tags:
            tags = tags + [LOCAL_GPU_TEST_TAG]
        kwargs["tags"] = tags

    _donner_multi_transitioned_test(
        name = name,
        dep = dep,
        renderer_backend = renderer_backend,
        **kwargs
    )

    if renderer_backend == "geode" and opens_gpu_device and remote_parallel_ci:
        remote_kwargs = dict(kwargs)
        remote_tags = [
            tag
            for tag in remote_kwargs.get("tags", [])
            if tag not in [LOCAL_GPU_TEST_TAG, "local-gpu-isolated", "ci-linux-hardware-gpu-source"]
        ]
        remote_kwargs["tags"] = remote_tags + ["ci-remote-gpu", "no-local"]
        _donner_multi_transitioned_test(
            name = name + "_ci_remote",
            dep = dep,
            renderer_backend = renderer_backend,
            **remote_kwargs
        )

donner_multi_transitioned_binary = rule(
    implementation = _donner_transitioned_executable_impl,
    executable = True,
    attrs = {
        "dep": attr.label(
            mandatory = True,
            executable = True,
            cfg = _multi_transition,
        ),
        "renderer_backend": attr.string(
            mandatory = True,
            values = ["tiny_skia", "geode", "inherit"],
        ),
        "full_text_only": attr.bool(default = False),
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

def donner_variant_cc_test(
        name,
        dep,
        variants = None,
        named_variants = None,
        opens_gpu_device = False,
        remote_parallel_ci = False,
        **kwargs):
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
      opens_gpu_device: Forwarded to donner_multi_transitioned_test; see there.
      remote_parallel_ci: Forwarded to donner_multi_transitioned_test; see there.
      **kwargs: Additional arguments passed to the generated test rules.

    Generated targets:
      {name}                  - alias to the default (no transition, uses active config)
      {name}_{variant_name}   - explicit variant
    """
    if named_variants:
        for v in named_variants:
            target_name = "{}_{}".format(name, v["name"])

            # Variant-specific execution arguments, sharding, and selection tags.
            variant_kwargs = dict(kwargs)
            for key in ["args", "shard_count", "tags"]:
                if key in v:
                    variant_kwargs[key] = v[key]

            donner_multi_transitioned_test(
                name = target_name,
                dep = dep,
                renderer_backend = v["backend"],
                text = v.get("text", "false"),
                text_full = v.get("text_full", "false"),
                opens_gpu_device = opens_gpu_device,
                remote_parallel_ci = remote_parallel_ci,
                testonly = 1,
                **variant_kwargs
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
                    opens_gpu_device = opens_gpu_device,
                    remote_parallel_ci = remote_parallel_ci,
                    testonly = 1,
                    **kwargs
                )

    # Default alias: uses no transition, inherits active command-line config.
    native.alias(
        name = name,
        actual = dep,
        testonly = 1,
    )

def identical_code_folding_linkopts():
    """
    Linker options that fold identical functions in optimized Linux links of a shipped binary.

    Apple's linker already folds identical code by default. Safe folding only merges functions
    whose address is never taken (per clang's address-significance tables), so function-pointer
    identity is preserved; code generation is unchanged. A crash backtrace can name a folded
    sibling of the function that actually ran. Requires lld or gold; see
    `//build_defs:linker_icf` for hosts that link with GNU ld.

    Returns:
      A select() of linker options.
    """
    return select({
        Label("//build_defs:linux_opt_icf"): ["-Wl,--icf=safe"],
        "//conditions:default": [],
    })

def donner_cc_binary(name, srcs = [], linkopts = [], deps = [], tags = [], **kwargs):
    """
    Create a cc_binary with donner-specific defaults.

    Args:
      name: Rule name.
      srcs: Source files.
      linkopts: List of linker options.
      deps: List of dependencies.
      tags: Tags.
      **kwargs: Additional arguments, matching the implementation of cc_binary.
    """
    ci_darwin_targets()
    cc_binary(
        name = name,
        srcs = srcs,
        linkopts = linkopts,
        deps = deps + libc_compat_deps(),
        tags = tags,
        **kwargs
    )

    _source_format_check(
        name = name,
        srcs = srcs,
        hdrs = kwargs.get("hdrs", []),
    )

# Standard variant specs for donner_cc_test(variants = ...).
#
# Each entry expands into a `{name}_{variant}` wrapper around the base
# cc_test, transitioned via `donner_multi_transitioned_test` so the
# renderer/text feature flags are flipped at test-graph configuration time.
# This is what makes `bazel test //...` cover the `tiny` / `text-full` /
# `geode` lanes by default - no `--config=` flag required (doc 0031 M2.3).
_VARIANT_SPECS = {
    "tiny": {
        "backend": "tiny_skia",
        "text": "false",
        "text_full": "false",
    },
    "text_full": {
        # The tiny_skia backend pairs only with the tiny, size-optimized
        # variant. The full text stack (FreeType+HarfBuzz+WOFF2) exercises the
        # geode backend instead.
        "backend": "geode",
        "text": "true",
        "text_full": "true",
    },
    "geode": {
        "backend": "geode",
        "text": "true",
        "text_full": "false",
    },
}

# Attributes that must be forwarded from the base cc_test onto each
# transitioned wrapper so per-variant runs share execution semantics
# (timeouts, sharding, OS gating, etc.) with the default-config target.
_VARIANT_FORWARDED_ATTRS = (
    "size",
    "timeout",
    "shard_count",
    "flaky",
    "local",
    "target_compatible_with",
)

def donner_cc_test(
        name,
        srcs = [],
        linkopts = [],
        deps = [],
        tags = [],
        variants = None,
        opens_gpu_device = False,
        remote_parallel_ci = False,
        **kwargs):
    """
    Create a cc_test with donner-specific defaults.

    Args:
      name: Rule name.
      srcs: Source files.
      linkopts: List of linker options.
      deps: List of dependencies.
      tags: Tags.
      variants: Optional list of variant names from `_VARIANT_SPECS`
        (e.g. `["tiny", "text_full", "geode"]`). When provided, in addition
        to the default-config `name` target, emits one `{name}_{variant}`
        wrapper per entry via `donner_multi_transitioned_test`. This is the
        opt-in mechanism that lets `bazel test //...` cover variant lanes
        without per-test `--config=` flags. See doc 0031 M2.3.
      opens_gpu_device: True when this test instantiates a real GPU
        device/adapter, so its geode-backed variants must never run alongside
        another GPU-opening test.
        Forwarded to donner_multi_transitioned_test; see there for why it is
        opt-in rather than implied by the geode backend.
      remote_parallel_ci: Forwarded to donner_multi_transitioned_test for
        opted-in Geode variants.
      **kwargs: Additional arguments, matching the implementation of cc_test.
    """
    ci_darwin_targets()
    if "size" not in kwargs and "timeout" not in kwargs:
        kwargs["size"] = "small"

    cc_test(
        name = name,
        srcs = srcs,
        linkopts = linkopts,
        deps = deps + libc_compat_deps() + test_crash_handler_deps(),
        tags = tags,
        **kwargs
    )

    _source_format_check(
        name = name,
        srcs = srcs,
        hdrs = kwargs.get("hdrs", []),
    )

    if variants:
        forwarded = {
            attr: kwargs[attr]
            for attr in _VARIANT_FORWARDED_ATTRS
            if attr in kwargs
        }
        for variant_name in variants:
            spec = _VARIANT_SPECS.get(variant_name)
            if spec == None:
                fail("Unknown variant '{}' for donner_cc_test {}; valid names: {}".format(
                    variant_name,
                    name,
                    sorted(_VARIANT_SPECS.keys()),
                ))
            donner_multi_transitioned_test(
                name = "{}_{}".format(name, variant_name),
                dep = ":" + name,
                renderer_backend = spec["backend"],
                opens_gpu_device = opens_gpu_device,
                remote_parallel_ci = remote_parallel_ci,
                text = spec["text"],
                text_full = spec["text_full"],
                tags = tags + ["variant_" + variant_name],
                testonly = 1,
                **forwarded
            )

def donner_perf_cc_test(name, correctness_srcs, wallclock_srcs, srcs = [], linkopts = [], deps = [], tags = [], wallclock_tags = [], correctness_shard_count = None, **kwargs):
    """
    Create paired correctness and wall-clock cc_tests with donner defaults.

    The macro emits:
      {name}_correctness    - PR-gated, included in `bazel test //...`
      {name}_wallclock      - tagged `manual` + `perf`, intended for nightly runs

    Args:
      name: Base rule name.
      correctness_srcs: Source files compiled only into the correctness target.
      wallclock_srcs: Source files compiled only into the wall-clock target.
      srcs: Shared source files compiled into both generated targets.
      linkopts: List of linker options.
      deps: List of dependencies.
      tags: Shared tags applied to both generated targets (except `manual` / `perf`
        are stripped from the correctness target so it stays on the default path).
      wallclock_tags: Additional tags applied only to the wall-clock target.
      correctness_shard_count: Optional `shard_count` for the correctness target only, which
        leaves the wall-clock target in one process so its timings stay comparable.
      **kwargs: Additional arguments, matching the implementation of cc_test.
    """
    if not correctness_srcs:
        fail("donner_perf_cc_test requires non-empty correctness_srcs")
    if not wallclock_srcs:
        fail("donner_perf_cc_test requires non-empty wallclock_srcs")

    correctness_tags = [tag for tag in tags if tag not in ["manual", "perf"]]
    generated_wallclock_tags = [tag for tag in tags if tag != "manual"]
    for tag in wallclock_tags + ["perf", "manual"]:
        if tag not in generated_wallclock_tags:
            generated_wallclock_tags.append(tag)

    if correctness_shard_count != None and "shard_count" in kwargs:
        fail("donner_perf_cc_test {}: pass correctness_shard_count or shard_count, not both".format(
            name,
        ))
    correctness_kwargs = dict(kwargs)
    if correctness_shard_count != None:
        correctness_kwargs["shard_count"] = correctness_shard_count

    donner_cc_test(
        name = name + "_correctness",
        srcs = srcs + correctness_srcs,
        linkopts = linkopts,
        deps = deps,
        tags = correctness_tags,
        **correctness_kwargs
    )

    donner_cc_test(
        name = name + "_wallclock",
        srcs = srcs + wallclock_srcs,
        linkopts = linkopts,
        deps = deps,
        tags = generated_wallclock_tags,
        **kwargs
    )

def donner_cc_library(name, srcs = [], hdrs = [], copts = [], tags = [], visibility = None, variant_neutral = None, **kwargs):
    """
    Create a cc_library with donner-specific defaults.

    Args:
      name: Rule name.
      srcs: Source files.
      hdrs: Header files.
      copts: List of copts.
      tags: List of tags.
      visibility: Visibility.
      variant_neutral: Compile the sources once per platform rather than once per product
        variant (see build_defs/variant_neutral.bzl). Unset, it is on except in the packages of
        VARIANT_DEPENDENT_PACKAGES in build_defs/variant_settings.bzl. A library with generated or
        selected sources, or with no sources to compile, compiles in place. Analysis fails if a
        product variant changes what the compile sees; such a library sets
        `variant_neutral = False` with a comment naming the variant it depends on.
      **kwargs: Additional arguments, matching the implementation of cc_library.
    """
    ci_darwin_targets()

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

    include_prefix = "/".join(package_path)
    library_srcs = srcs
    if variant_neutral == None:
        variant_neutral = variant_neutral_default(native.package_name())
    if variant_neutral:
        library_srcs = variant_neutral_srcs(
            name = name,
            srcs = srcs,
            hdrs = hdrs,
            include_prefix = include_prefix,
            copts = copts + ["-I."],
            **kwargs
        )

    cc_library(
        name = name,
        srcs = library_srcs,
        hdrs = hdrs,
        include_prefix = include_prefix,
        copts = copts + ["-I."],
        tags = tags,
        visibility = visibility,
        **kwargs
    )

    _source_format_check(
        name = name,
        srcs = srcs,
        hdrs = hdrs,
    )

def _fuzzer_routing_manifest_impl(ctx):
    lines = []
    for suffix, tags in [
        ("", ctx.attr.corpus_tags),
        ("_bin", ctx.attr.common_tags),
        ("_soak", ctx.attr.soak_tags),
    ]:
        for tag in sorted(tags):
            lines.append("%s%s\t%s" % (ctx.attr.fuzzer_name, suffix, tag))
    ctx.actions.write(ctx.outputs.out, "\n".join(lines) + "\n")
    return [DefaultInfo(files = depset([ctx.outputs.out]))]

_fuzzer_routing_manifest = rule(
    implementation = _fuzzer_routing_manifest_impl,
    attrs = {
        "common_tags": attr.string_list(),
        "corpus_tags": attr.string_list(),
        "fuzzer_name": attr.string(mandatory = True),
        "soak_tags": attr.string_list(),
    },
    outputs = {"out": "%{name}.txt"},
)

def _shell_quote(value):
    return "'" + value.replace("'", "'\"'\"'") + "'"

def _fuzzer_soak_test_impl(ctx):
    executable = ctx.actions.declare_file(ctx.label.name)
    files = [ctx.executable._runner, ctx.executable.binary] + ctx.files.corpus
    arguments = []
    for file in files:
        arguments.append('"${TEST_SRCDIR}/${TEST_WORKSPACE}/"' + _shell_quote(file.short_path))
    flags = [_shell_quote(flag) for flag in ctx.attr.fuzz_args]
    command = arguments[:2] + flags + arguments[2:] + ['"$@"']
    ctx.actions.write(
        executable,
        "#!/usr/bin/env bash\nset -euo pipefail\nexec " + " ".join(command) + "\n",
        is_executable = True,
    )
    runfiles = ctx.runfiles(files = files + ctx.files.runtime_data)
    for target in [ctx.attr._runner, ctx.attr.binary]:
        info = target[DefaultInfo]
        runfiles = runfiles.merge(info.default_runfiles).merge(info.data_runfiles)
    return [DefaultInfo(executable = executable, runfiles = runfiles)]

_fuzzer_soak_test = rule(
    implementation = _fuzzer_soak_test_impl,
    test = True,
    attrs = {
        "binary": attr.label(executable = True, cfg = "target", mandatory = True),
        "corpus": attr.label(allow_files = True, mandatory = True),
        "fuzz_args": attr.string_list(),
        "runtime_data": attr.label_list(allow_files = True),
        "_runner": attr.label(
            default = Label("//build_defs:fuzzer_soak_runner"),
            executable = True,
            cfg = "target",
        ),
    },
)

# libFuzzer `-timeout` floor for `{fuzzer}_soak` under `--config=asan-fuzzer`. See
# `per_input_timeout_seconds` on `donner_cc_fuzzer`.
_ASAN_FUZZER_PER_INPUT_TIMEOUT_SECONDS = 60

# libFuzzer `-report_slow_units` for `{fuzzer}_soak`. Once mutation has started, a unit reported
# as taking this long is replayed in a fresh process, and the soak fails if the replay is slow too
# (see build_defs/fuzzer_soak.sh). The per-input timeout above stays a hang guard; this is the
# bound that catches inputs that finish, but slowly, while a stalled runner still passes. It is
# libFuzzer's own default, made explicit because the launcher depends on the report.
_SOAK_SLOW_UNIT_SECONDS = 10

def donner_cc_fuzzer(
        name,
        corpus,
        deps = [],
        per_input_timeout_seconds = 25,
        tags = [],
        routing_manifest = None,
        routing_manifest_visibility = None,
        full_text_only = False,
        extra_soak_tags = [],
        data = [],
        **kwargs):
    """
    Create a libfuzzer-based fuzz target.

    Args:
      name: Rule name.
      corpus: Path to a corpus directory, or a filegroup rule for the corpus.
      deps: List of dependencies.
      data: Explicit runtime fixtures shared by raw, corpus and soak entry points.
      per_input_timeout_seconds: libFuzzer `-timeout` for `{name}_soak`. This is a hang guard,
        not a latency budget: it only has to separate an input that never finishes from a slow
        one on a loaded runner, including first-input setup under sanitizers. Under
        `--config=asan-fuzzer` the soak uses at least 60 s. Override it only to go above 25 s.
        A mutated input that libFuzzer reports as slow (at least 10 s, and more than 10% longer
        than the longest unit it already reported, seeds included) still fails the soak when its
        replay also takes 10 s or more; seed corpus replay, the process's warm-up, is held only
        to this hang guard.
      tags: Additional Bazel tags used to select configuration-specific corpus and soak lanes.
      full_text_only: Keep the corpus, raw binary, and soak on the full-text configuration.
      extra_soak_tags: Tags applied only to the generated soak test.
      routing_manifest: Optional target name for a dependency-free manifest of the exact tags
        assigned to each generated fuzzer target.
      routing_manifest_visibility: Visibility of the routing manifest; private by default.
      **kwargs: Additional arguments, matching the implementation of cc_test.

    Generated targets:
      {name}        - corpus regression test. Replays every checked-in corpus
                      input, no time budget, always on the PR gate. This is the
                      target that catches a regression.
      {name}_bin    - the raw fuzzer binary, for interactive fuzzing.
      {name}_soak   - exploratory mutation, tagged `fuzz_soak` so PR CI can
                      exclude it. Time budget is controlled by
                      `--//build_defs:fuzz_soak` (see build_defs/BUILD.bazel).
    """
    ci_darwin_targets()
    if per_input_timeout_seconds <= 0:
        fail("per_input_timeout_seconds must be positive")

    if not (corpus.startswith("//") or corpus.startswith(":")):
        corpus_name = name + "_corpus"
        corpus = native.glob([corpus + "/**"])
        native.filegroup(name = corpus_name, srcs = corpus)
    else:
        corpus_name = corpus

    fuzzer_runtime_linkopts = fuzzer_linkopts()
    fuzzer_additional_linker_inputs = fuzzer_linker_inputs()
    ubsan_tags = [tag for tag in tags if tag.startswith("fuzz_ubsan")]
    common_tags = [tag for tag in tags if not tag.startswith("fuzz_ubsan")]
    if "fuzz_text_full" in common_tags and "fuzz_ubsan_text_full" not in ubsan_tags:
        ubsan_tags.append("fuzz_ubsan_text_full")
    if "fuzz_geode" in common_tags and "fuzz_ubsan_geode" not in ubsan_tags:
        ubsan_tags.append("fuzz_ubsan_geode")
    common_target_tags = ["fuzz_target"] + common_tags
    corpus_tags = ["fuzz_target", "fuzz_ubsan"] + common_tags + ubsan_tags
    soak_tags = common_target_tags + ["fuzz_soak"] + extra_soak_tags

    if routing_manifest:
        _fuzzer_routing_manifest(
            name = routing_manifest,
            common_tags = common_target_tags,
            corpus_tags = corpus_tags,
            fuzzer_name = name,
            soak_tags = soak_tags,
            visibility = routing_manifest_visibility,
        )

    binary_name = name + "_bin_impl" if full_text_only else name + "_bin"
    corpus_test_name = name + "_impl" if full_text_only else name
    implementation_tags = ["manual"] if full_text_only else common_target_tags
    cc_binary(
        name = binary_name,
        additional_linker_inputs = fuzzer_additional_linker_inputs,
        linkopts = fuzzer_runtime_linkopts,
        linkstatic = 1,
        deps = deps + libc_compat_deps(),
        data = data,
        target_compatible_with = fuzzer_compatible_with(),
        tags = implementation_tags,
        **kwargs
    )

    if full_text_only:
        donner_multi_transitioned_binary(
            name = name + "_bin",
            dep = ":" + binary_name,
            renderer_backend = "inherit",
            full_text_only = True,
            tags = common_target_tags,
            testonly = kwargs.get("testonly", False),
            target_compatible_with = fuzzer_compatible_with(),
        )

    # The time budget is a flag, not a literal. The corpus replay is what makes
    # this a regression test, and libFuzzer always runs the whole seed corpus
    # before it mutates anything, so the budget below only sizes the exploratory
    # tail. On the PR gate that tail was 25% of the entire test suite's CPU
    # (576 of 2297 CPU-seconds across 88 targets) spent mutating in a build with
    # no sanitizers, so PR CI now excludes the `fuzz_soak` tag entirely. The Fuzz
    # workflow flips `--//build_defs:fuzz_soak=true` and gets a longer budget than
    # the old fixed 10 s, under `--config=asan-fuzzer` where a discovered input is
    # actually diagnosable.
    fuzz_time_args = select({
        "//build_defs:fuzz_soak_enabled": ["-max_total_time=30"],
        "//conditions:default": ["-max_total_time=2"],
    })

    _fuzzer_soak_test(
        name = name + "_soak",
        binary = ":" + name + "_bin",
        corpus = corpus_name,
        fuzz_args = fuzz_time_args + ["-report_slow_units=%d" % _SOAK_SLOW_UNIT_SECONDS] + select({
            "//build_defs:asan_fuzzer_enabled": [
                "-timeout=%d" % max(per_input_timeout_seconds, _ASAN_FUZZER_PER_INPUT_TIMEOUT_SECONDS),
            ],
            "//conditions:default": ["-timeout=%d" % per_input_timeout_seconds],
        }),
        target_compatible_with = fuzzer_compatible_with(),
        size = "small",
        # An input that starts just before the time budget ends may run for up to
        # 1.5x the per-input timeout before libFuzzer reports it, so a hang must
        # still fail as a hang rather than as a Bazel test timeout. Replaying a reported
        # slow unit can add up to the same again plus the launcher's 5 s report grace,
        # which still fits: 30 s + 2 x 90 s + 5 s under `--config=asan-fuzzer`.
        timeout = "moderate",
        runtime_data = select({
            "@platforms//os:macos": ["@llvm_toolchain//:linker-components-aarch64-darwin"],
            "//conditions:default": [],
        }),
        tags = soak_tags,
    )

    donner_cc_test(
        name = corpus_test_name,
        additional_linker_inputs = fuzzer_additional_linker_inputs,
        linkopts = fuzzer_runtime_linkopts,
        args = ["$(locations %s)" % corpus_name],
        linkstatic = 1,
        deps = deps,
        data = data + [corpus_name] + select({
            "@platforms//os:macos": ["@llvm_toolchain//:linker-components-aarch64-darwin"],
            "//conditions:default": [],
        }),
        target_compatible_with = fuzzer_compatible_with(),
        tags = ["manual"] if full_text_only else corpus_tags,
        **kwargs
    )

    if full_text_only:
        donner_multi_transitioned_test(
            name = name,
            dep = ":" + corpus_test_name,
            renderer_backend = "inherit",
            full_text_only = True,
            file_args = [corpus_name],
            tags = corpus_tags,
            target_compatible_with = fuzzer_compatible_with(),
            **{key: kwargs[key] for key in _VARIANT_FORWARDED_ATTRS if key in kwargs and key != "target_compatible_with"}
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
        # Use `ctx.files.srcs` / `ctx.files.hdrs` (not `ctx.attr.*`) so
        # we pass actual File objects - `cc_common.compile` rejects the
        # list of Targets returned by `ctx.attr`. Required for
        # perf-sensitive wrappers that carry their own source files,
        # not just deps-only wrappers.
        srcs = ctx.files.srcs,
        includes = ctx.attr.includes,
        defines = ctx.attr.defines,
        local_defines = ctx.attr.local_defines,
        public_hdrs = ctx.files.hdrs,
        user_compile_flags = ctx.attr.copts,
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
            # The editor's Darwin toolchain doesn't have an action
            # config for the standalone dynamic-library linker action,
            # so force the wrapper to emit a static archive instead.
            # The top-level editor binary links it statically anyway.
            disallow_dynamic_library = True,
            alwayslink = ctx.attr.alwayslink,
        )

        if linking_outputs.library_to_link != None:
            # Place this library's own archive BEFORE its deps (matching
            # native cc_library semantics). Appending it after the deps puts
            # the consumer archive later on the link line than its providers,
            # which lld tolerates (backward references from archives) but
            # single-pass GNU linkers (gold, bfd) reject with undefined
            # references. Caught by the CI "Linker canary" step.
            linking_contexts.insert(0, linking_context)

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
        "native_optimization_level": attr.string(default = ""),
        "linkopts": attr.string_list(default = []),  # Optional link options
        "alwayslink": attr.bool(default = False),  # Whole-archive inclusion
    },
    fragments = ["cpp"],
)

def donner_perf_sensitive_cc_library(name, allow_debug_builds_config = None, target_compatible_with = None, optimization_level = "O2", **kwargs):
    """
    Compiles its own native sources for speed, independently of global size flags.

    The opt feature remains enabled regardless of --compilation_mode. Native sources
    additionally receive the selected O2/O3 level after caller copts. Dependencies
    retain their own settings, and WebAssembly retains its existing size policy.

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
      optimization_level: Native speed level, O2 by default or O3 for measured kernels.
      **kwargs: Additional arguments, matching the implementation of cc_library.
    """
    ci_darwin_targets()
    compat = {}
    if target_compatible_with != None:
        compat["target_compatible_with"] = target_compatible_with
        kwargs["target_compatible_with"] = target_compatible_with

    speed_copts = native_speed_copts(optimization_level)
    optimized_kwargs = dict(kwargs)
    if kwargs.get("srcs", []):
        optimized_kwargs["copts"] = kwargs.get("copts", []) + speed_copts
        optimized_kwargs["native_optimization_level"] = optimization_level

    if allow_debug_builds_config != None:
        _donner_perf_sensitive_cc_library(
            name = name + "_opt",
            **optimized_kwargs
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
            testonly = kwargs.get("testonly", False),
            **compat
        )
    else:
        _donner_perf_sensitive_cc_library(
            name = name,
            tags = ["perf_sensitive"],
            **optimized_kwargs
        )

    _source_format_check(
        name = name,
        srcs = kwargs.get("srcs", []),
        hdrs = kwargs.get("hdrs", []),
    )

# An incoming transition applies to the real executable *and* Bazel's TestRunner.
# Keep Linux's configured host tools. Darwin uses Apple tools on Darwin workers.
# Match the native macOS lane so Apple genrules and test tools resolve on the worker.
_CI_DARWIN_PATH = "PATH=/opt/homebrew/opt/llvm@19/bin:/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"
_CI_ACTION_ENVS = ["//command_line_option:action_env", "//command_line_option:host_action_env"]
_CI_PLATFORM_OPTIONS = [
    "//command_line_option:platforms",
    "//command_line_option:host_platform",
    "//command_line_option:extra_execution_platforms",
    "//build_defs:llvm_latest",
    "//build_defs:metal_shader_validation",
    "//command_line_option:test_env",
    "//donner/svg/renderer:renderer_backend",
    "//donner/svg/renderer/geode:enable_geode",
] + _CI_ACTION_ENVS

def _ci_test_platform(settings, attr):
    options = dict(settings)
    xcode_version = options.pop("//command_line_option:xcode_version")
    options["//command_line_option:platforms"] = [str(attr.target_platform)]
    if attr.darwin_profile == "geode":
        options["//donner/svg/renderer:renderer_backend"] = "geode"
        options["//donner/svg/renderer/geode:enable_geode"] = True
    if attr.darwin_profile in ["ordinary", "geode"]:
        options["//build_defs:metal_shader_validation"] = "off"
    if attr.metal_validation:
        options["//build_defs:metal_shader_validation"] = "on"
    if attr.darwin:
        darwin = str(Label("//tools/ci:remote_macos_aarch64"))
        options["//command_line_option:host_platform"] = darwin
        # Toolchains such as rules_js package their execution-platform runtime into the test.
        # Prefer Darwin on this configuration so the packaged Node executable runs on its TestRunner.
        platforms = "//command_line_option:extra_execution_platforms"
        options[platforms] = [darwin] + [p for p in settings[platforms] if str(p).lstrip("@") != darwin.lstrip("@")]
        options["//build_defs:llvm_latest"] = "0"
        for key in _CI_ACTION_ENVS:
            options[key] = [v for v in settings[key] if v.split("=", 1)[0] != "PATH"] + [_CI_DARWIN_PATH]
        if xcode_version:
            identity = ci_xcode_identity(str(xcode_version))
            for key in _CI_ACTION_ENVS + ["//command_line_option:test_env"]:
                options[key] = [v for v in options[key] if v.split("=", 1)[0] != "DONNER_CI_XCODE_IDENTITY"] + [identity]
    return options

_ci_test_transition = variant_transition(
    implementation = _ci_test_platform,
    inputs = _CI_PLATFORM_OPTIONS + ["//command_line_option:xcode_version"],
    outputs = _CI_PLATFORM_OPTIONS,
)

ci_platform_test = rule(
    implementation = _donner_transitioned_executable_impl,
    test = True,
    cfg = _ci_test_transition,
    attrs = {
        "darwin": attr.bool(),
        "darwin_profile": attr.string(values = ["", "ordinary", "desktop", "geode"]),
        "fixed_test_env": attr.string_dict(),
        "data": attr.label_list(allow_files = True),
        "metal_validation": attr.bool(),
        "dep": attr.label(mandatory = True, executable = True, cfg = "target"),
        "target_platform": attr.label(mandatory = True),
        "_allowlist_function_transition": attr.label(default = "@bazel_tools//tools/allowlists/function_transition_allowlist"),
    } | _TRANSITIONED_TEST_COVERAGE_ATTRS,
)

def _ci_platform_build_impl(ctx):
    return [DefaultInfo(files = ctx.attr.dep[DefaultInfo].files)]

ci_platform_build = rule(
    implementation = _ci_platform_build_impl,
    cfg = _ci_test_transition,
    attrs = {
        "darwin": attr.bool(),
        "darwin_profile": attr.string(values = ["ordinary", "desktop", "geode"]),
        "dep": attr.label(mandatory = True, allow_files = True),
        "metal_validation": attr.bool(),
        "target_platform": attr.label(mandatory = True),
        "_allowlist_function_transition": attr.label(default = "@bazel_tools//tools/allowlists/function_transition_allowlist"),
    },
)

def _ci_wasm_options(settings, attr):
    return wasm_profile_options(settings, attr.profile)

_ci_wasm_transition = transition(
    implementation = _ci_wasm_options,
    inputs = WASM_OPTION_INPUTS,
    outputs = WASM_OPTION_OUTPUTS,
)

_wasm_ci_test = rule(
    implementation = _donner_transitioned_executable_impl,
    test = True,
    cfg = _ci_wasm_transition,
    attrs = {
        "argument_files": attr.label_list(allow_files = True),
        "fixed_args": attr.string_list(),
        "dep": attr.label(mandatory = True, executable = True, cfg = "target"),
        "profile": attr.string(mandatory = True, values = ["editor-wasm", "wasm-size", "wasm-geode"]),
        "required_outputs": attr.label_list(allow_files = True),
        "_allowlist_function_transition": attr.label(default = "@bazel_tools//tools/allowlists/function_transition_allowlist"),
    } | _TRANSITIONED_TEST_COVERAGE_ATTRS,
)

def wasm_ci_test(actual, profile, required_outputs = [], payload_budget = False):
    """Configure one real Wasm test for affected CI and the hosted candidate suite."""
    source = native.existing_rule(actual)
    options = {
        "dep": ":" + actual,
        "profile": profile,
        "required_outputs": required_outputs,
        "argument_files": source.get("data", []),
        "size": source.get("size", "small"),
    }
    args = source.get("args", [])
    _wasm_ci_test(
        name = "graph_" + actual,
        fixed_args = args,
        tags = ["editor-wasm-check"],
        target_compatible_with = select({
            "//tools/ci:on_mixed_ci_darwin_gpu" if profile == "editor-wasm" else "//tools/ci:on_mixed_ci_linux": [],
            "//conditions:default": ["@platforms//:incompatible"],
        }),
        **options
    )
    for report in ([False, True] if payload_budget else [False]):
        _wasm_ci_test(
            name = "hosted_" + actual + ("_report" if report else ""),
            fixed_args = list(args) + (["--payload-budget-mode=report"] if report else []),
            tags = ["manual"],
            target_compatible_with = ["@platforms//os:macos"],
            **options
        )

def _finalize_ci_package(name, visibility):
    declare_darwin_variants(struct(build = ci_platform_build, test = ci_platform_test))

# Consumer builds also support Bazel 7. Only the root CI workspace needs finalizers.
_ci_package_finalizer = bazel_features.globals.macro(
    implementation = _finalize_ci_package,
    finalizer = True,
) if bazel_features.globals.macro else None

def ci_darwin_targets():
    """Register native CI variants once, including raw rules declared later in the package."""
    if not _ci_package_finalizer or Label("//:BUILD.bazel").workspace_name:
        return
    package = native.package_name()
    if native.repository_name() not in ["@", "@@"] or package == "third_party" or package.startswith("third_party/"):
        return
    marker = "_donner_ci_package_registered"
    registered = native.existing_rule(marker)
    if registered != None:
        if registered["kind"] != "filegroup" or "ci-package-registration" not in registered["tags"]:
            fail("Reserved CI registration name collides with //%s:%s" % (package, marker))
        return
    native.filegroup(
        name = marker,
        tags = ["ci-package-registration", "manual"],
        visibility = ["//visibility:private"],
    )
    _ci_package_finalizer(name = "ci")
