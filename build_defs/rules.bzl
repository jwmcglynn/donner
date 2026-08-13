"""
Helper rules, such as for building fuzzers.
"""

load("@bazel_tools//tools/cpp:toolchain_utils.bzl", "find_cpp_toolchain")
load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

def _banned_patterns_lint_test(name, srcs, hdrs, tags = [], **_kwargs):
    """Emits nothing. The banned-patterns check is now one repo-wide scan.

    This used to emit one `{name}_lint` py_test per
    donner_cc_{library,test,binary,fuzzer}, producing 476 test targets. Measured
    from a full self-hosted CI run's build event log, those 476 targets burned
    714 of the suite's 2297 test CPU-seconds - 31% of the entire test suite -
    to run a regex scan whose real work is 2.5 seconds for the whole repository
    in a single process. Essentially all of it was per-target interpreter
    startup, runfiles staging, and test-runner overhead; the scan itself is
    microseconds per file.

    The check did not shrink, it got BROADER. `tools/lint.sh` feeds
    check_banned_patterns.py every C++ file under donner/ and examples/,
    including the ones this macro could never see: `select()`-valued srcs/hdrs
    (skipped outright above), label-form srcs from other rules, files no target
    lists at all, and anything tagged `manual`. Verified clean over all 2479
    files at the commit that made this change.

    The signature is preserved so the call sites in donner_cc_library /
    donner_cc_test / donner_cc_binary stay unchanged, and so this rationale
    sits where the next reader will look for the missing `_lint` targets.

    Args:
      name: Unused; parent target name.
      srcs: Unused; source files.
      hdrs: Unused; header files.
      tags: Unused; tags from the parent rule.
      **_kwargs: Unused.
    """
    _ = (name, srcs, hdrs, tags)  # @unused

def llvm21_macos_runtime_rpath_linkopts():
    """
    Returns rpaths needed by LLVM 21 sanitizer runtime dylibs on macOS.
    """
    runtime_dir = "toolchains_llvm++llvm+llvm_toolchain_llvm/lib/clang/21/lib/darwin"
    return select({
        "//build_defs:llvm_latest_macos": [
            # Add rpaths for execroot (from bazel-bin/<package> to external/).
            # The needed depth varies with package path depth.
            "-Wl,-rpath,@loader_path/../../../../../../external/" + runtime_dir,
            "-Wl,-rpath,@loader_path/../../../../../../../external/" + runtime_dir,
            "-Wl,-rpath,@loader_path/../../../../../../../../external/" + runtime_dir,
            "-Wl,-rpath,@loader_path/../../../../../../../../../external/" + runtime_dir,
            # Add rpaths for runfiles directory (without the external/ prefix).
            "-Wl,-rpath,@loader_path/../../../../" + runtime_dir,
            "-Wl,-rpath,@loader_path/../../../../../" + runtime_dir,
            "-Wl,-rpath,@loader_path/../../../../../../" + runtime_dir,
            "-Wl,-rpath,@loader_path/../../../../../../../" + runtime_dir,
        ],
        "//conditions:default": [],
    })

def libc_compat_deps():
    """
    Returns extra deps needed when linking against the hermetic LLVM toolchain
    on Linux.

    Chromium's Debian Bullseye sysroot (wired into `llvm_toolchain` via
    `//third_party/bazel/non_bcr_deps.bzl`) exports `copy_file_range@GLIBC_2.27`
    as a non-default-versioned symbol, so unversioned references from
    `toolchains_llvm`'s prebuilt libc++.a don't auto-resolve. The tiny shim in
    `//third_party/libc_compat` provides an unversioned `copy_file_range`
    that forwards to the versioned glibc entry point.
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
            llvm_lib + "/clang/21/lib/darwin/libclang_rt.fuzzer_osx.a",
            "-nostdlib++",
            llvm_lib + "/libc++.a",
            llvm_lib + "/libc++abi.a",
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
    launcher_target = "_donner_transitioned/{}/{}".format(ctx.label.package, ctx.label.name)
    forwarded_runfiles = ctx.runfiles(
        root_symlinks = {launcher_target: files_to_run.executable},
    ).merge(dep_default_info.default_runfiles).merge(dep_default_info.data_runfiles)

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
exec "$impl" "$@"
""".format(logical_impl = launcher_target)
    ctx.actions.write(executable, launcher, is_executable = True)

    providers = [
        DefaultInfo(
            executable = executable,
            files = depset([executable], transitive = [dep_default_info.files]),
            runfiles = forwarded_runfiles,
        ),
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
    if RunEnvironmentInfo in dep_target:
        providers.append(dep_target[RunEnvironmentInfo])

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
            values = ["tiny_skia", "geode"],
        ),
    },
)

def _multi_transition_impl(settings, attr):
    if settings["//build_defs:disable_backend_test_transition"]:
        return {
            "//donner/svg/renderer:renderer_backend": settings["//donner/svg/renderer:renderer_backend"],
            "//donner/svg/renderer:text": settings["//donner/svg/renderer:text"],
            "//donner/svg/renderer:text_full": settings["//donner/svg/renderer:text_full"],
            "//donner/svg/renderer/geode:enable_geode": settings["//donner/svg/renderer/geode:enable_geode"],
        }

    # Selecting the geode backend implies turning on Dawn: the
    # `:renderer_geode` library gates its sources behind the
    # `enable_geode` flag, so the transition must set it to keep the
    # dependency graph buildable without the user also passing
    # `--config=geode` on the command line.
    return {
        "//donner/svg/renderer:renderer_backend": attr.renderer_backend,
        "//donner/svg/renderer:text": attr.text == "true" or attr.text_full == "true",
        "//donner/svg/renderer:text_full": attr.text_full == "true",
        "//donner/svg/renderer/geode:enable_geode": attr.renderer_backend == "geode",
    }

_multi_transition = transition(
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
        "dep": attr.label(
            mandatory = True,
            executable = True,
            cfg = _multi_transition,
        ),
        "renderer_backend": attr.string(
            mandatory = True,
            values = ["tiny_skia", "geode"],
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

def donner_multi_transitioned_test(name, dep, renderer_backend, opens_gpu_device = False, **kwargs):
    """Create a transitioned test with an honest short-runtime default.

    Args:
      name: Rule name.
      dep: The underlying test target to transition.
      renderer_backend: "tiny_skia" or "geode".
      opens_gpu_device: True when the test actually instantiates a GPU
        device/adapter. Only those tests get `exclusive-if-local`, which makes
        Bazel drain them one at a time so a device loss in one process cannot
        cascade into its neighbours. Selecting the geode BACKEND is not the
        same thing as opening a device: most variant targets link wgpu-native
        and never touch it (they use MockRendererInterface, or assert on
        parsing/ECS/CSS/text-shaping), and tagging those serialized every
        geode-configured target in the repo into a single-file queue at the
        end of the test phase.
      **kwargs: Additional arguments for the underlying test rule.
    """
    if "size" not in kwargs and "timeout" not in kwargs:
        kwargs["size"] = "small"
    if renderer_backend == "geode" and opens_gpu_device:
        tags = kwargs.get("tags", [])
        if "exclusive-if-local" not in tags:
            kwargs["tags"] = tags + ["exclusive-if-local"]

    _donner_multi_transitioned_test(
        name = name,
        dep = dep,
        renderer_backend = renderer_backend,
        **kwargs
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
            values = ["tiny_skia", "geode"],
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

def donner_variant_cc_test(name, dep, variants = None, named_variants = None, opens_gpu_device = False, **kwargs):
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
      **kwargs: Additional arguments passed to the generated test rules.

    Generated targets:
      {name}                  - alias to the default (no transition, uses active config)
      {name}_{variant_name}   - explicit variant
    """
    if named_variants:
        for v in named_variants:
            target_name = "{}_{}".format(name, v["name"])

            # Per-variant overrides: a variant dict may carry "args" and/or
            # "shard_count" to override the shared kwargs for just that
            # variant (e.g. the resvg geode variant filters out CPU-reference
            # params and takes a higher shard count than the CPU variants).
            variant_kwargs = dict(kwargs)
            if "args" in v:
                variant_kwargs["args"] = v["args"]
            if "shard_count" in v:
                variant_kwargs["shard_count"] = v["shard_count"]

            donner_multi_transitioned_test(
                name = target_name,
                dep = dep,
                renderer_backend = v["backend"],
                text = v.get("text", "false"),
                text_full = v.get("text_full", "false"),
                opens_gpu_device = opens_gpu_device,
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
                    testonly = 1,
                    **kwargs
                )

    # Default alias: uses no transition, inherits active command-line config.
    native.alias(
        name = name,
        actual = dep,
        testonly = 1,
    )

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
    donner_linkopts = (
        linkopts +
        llvm21_macos_runtime_rpath_linkopts()
    )
    cc_binary(
        name = name,
        srcs = srcs,
        linkopts = donner_linkopts,
        deps = deps + libc_compat_deps(),
        tags = tags,
        **kwargs
    )

    _banned_patterns_lint_test(
        name = name,
        srcs = srcs,
        hdrs = kwargs.get("hdrs", []),
        tags = tags,
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
        add_llvm_macos_runtime_rpaths = True,
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
        device/adapter, so its geode-backed variants must be drained serially.
        Forwarded to donner_multi_transitioned_test; see there for why it is
        opt-in rather than implied by the geode backend.
      add_llvm_macos_runtime_rpaths: Add LLVM 21 sanitizer runtime rpaths on macOS.
      **kwargs: Additional arguments, matching the implementation of cc_test.
    """
    donner_linkopts = linkopts
    if add_llvm_macos_runtime_rpaths:
        donner_linkopts = donner_linkopts + llvm21_macos_runtime_rpath_linkopts()

    if "size" not in kwargs and "timeout" not in kwargs:
        kwargs["size"] = "small"

    cc_test(
        name = name,
        srcs = srcs,
        linkopts = donner_linkopts,
        deps = deps + libc_compat_deps() + test_crash_handler_deps(),
        tags = tags,
        **kwargs
    )

    _banned_patterns_lint_test(
        name = name,
        srcs = srcs,
        hdrs = kwargs.get("hdrs", []),
        tags = tags,
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
                text = spec["text"],
                text_full = spec["text_full"],
                tags = tags + ["variant_" + variant_name],
                testonly = 1,
                **forwarded
            )

def donner_perf_cc_test(name, correctness_srcs, wallclock_srcs, srcs = [], linkopts = [], deps = [], tags = [], wallclock_tags = [], **kwargs):
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

    donner_cc_test(
        name = name + "_correctness",
        srcs = srcs + correctness_srcs,
        linkopts = linkopts,
        deps = deps,
        tags = correctness_tags,
        **kwargs
    )

    donner_cc_test(
        name = name + "_wallclock",
        srcs = srcs + wallclock_srcs,
        linkopts = linkopts,
        deps = deps,
        tags = generated_wallclock_tags,
        **kwargs
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

def donner_cc_fuzzer(name, corpus, deps = [], per_input_timeout_seconds = 2, **kwargs):
    """
    Create a libfuzzer-based fuzz target.

    Args:
      name: Rule name.
      corpus: Path to a corpus directory, or a filegroup rule for the corpus.
      deps: List of dependencies.
      per_input_timeout_seconds: Maximum time for one generated input in the timed fuzz test.
      **kwargs: Additional arguments, matching the implementation of cc_test.
    """
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

    cc_binary(
        name = name + "_bin",
        additional_linker_inputs = fuzzer_additional_linker_inputs,
        linkopts = fuzzer_runtime_linkopts,
        linkstatic = 1,
        deps = deps + libc_compat_deps(),
        target_compatible_with = fuzzer_compatible_with(),
        tags = ["fuzz_target"],
        **kwargs
    )

    donner_cc_test(
        name = name + "_10_seconds",
        add_llvm_macos_runtime_rpaths = False,
        additional_linker_inputs = fuzzer_additional_linker_inputs,
        linkopts = fuzzer_runtime_linkopts,
        args = [
            "-max_total_time=10",
            "-timeout=%d" % per_input_timeout_seconds,
        ],
        linkstatic = 1,
        deps = deps,
        target_compatible_with = fuzzer_compatible_with(),
        size = "small",
        data = select({
            "@platforms//os:macos": ["@llvm_toolchain//:linker-components-aarch64-darwin"],
            "//conditions:default": [],
        }),
        tags = ["fuzz_target"],
        **kwargs
    )

    donner_cc_test(
        name = name,
        add_llvm_macos_runtime_rpaths = False,
        additional_linker_inputs = fuzzer_additional_linker_inputs,
        linkopts = fuzzer_runtime_linkopts,
        args = ["$(locations %s)" % corpus_name],
        linkstatic = 1,
        deps = deps,
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
        "linkopts": attr.string_list(default = []),  # Optional link options
        "alwayslink": attr.bool(default = False),  # Whole-archive inclusion
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
