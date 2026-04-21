"""Per-library `misc-include-cleaner` lint test.

`donner_cc_library` / `donner_cc_test` / `donner_cc_binary` opt into this
check via `include_cleaner = "strict"`. When set, an `{name}_include_cleaner`
test is emitted that runs `clang-tidy --checks=-*,misc-include-cleaner
--warnings-as-errors=*` on the library's `srcs` + `hdrs` using compile flags
extracted from the underlying CcInfo provider chain (so include paths,
defines, and copts match the real compile).

The check is gated per-library because the project carries ~2,875 historical
findings (issue #559). Opting in must happen directory-by-directory as code
is cleaned up. The diff-only CI gate in `tools/run_misc_include_cleaner_diff.sh`
keeps new debt from growing while we burn down the existing pile.

Mechanics borrowed from `@bazel_clang_tidy//clang_tidy:clang_tidy_test.bzl`
(MIT-licensed). We reimplement the relevant bits inline so the macro stays
BCR-safe — `@bazel_clang_tidy` is a non-BCR dev dependency, but
`donner_cc_*` macros must not depend on it (downstream BCR consumers could
otherwise fail to resolve loads). Only standard `@bazel_tools` and
`@rules_cc` symbols are used here.
"""

load("@bazel_tools//tools/build_defs/cc:action_names.bzl", "ACTION_NAMES")
load("@bazel_tools//tools/cpp:toolchain_utils.bzl", "find_cpp_toolchain")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")

INCLUDE_CLEANER_VALUES = ["strict"]

_HEADER_EXTENSIONS = (".h", ".hh", ".hpp", ".hxx", ".inc", ".inl", ".H")
_SOURCE_EXTENSIONS = (".c", ".cc", ".cpp", ".cxx", ".c++", ".C")

# Flags that GCC emits but Clang doesn't understand. Drop them so the
# clang-tidy invocation doesn't fail before it even parses sources.
_UNSUPPORTED_FLAGS = (
    "-fno-canonical-system-headers",
    "-fstack-usage",
)

def _shell_quote(s):
    return "'" + s.replace("'", "'\\''") + "'"

def _is_header(f):
    return f.path.endswith(_HEADER_EXTENSIONS)

def _is_source(f):
    return f.path.endswith(_SOURCE_EXTENSIONS)

def _toolchain_flags(ctx, cc_toolchain, action_name):
    feature_configuration = cc_common.configure_features(
        ctx = ctx,
        cc_toolchain = cc_toolchain,
    )
    user_compile_flags = list(ctx.fragments.cpp.copts)
    if action_name == ACTION_NAMES.cpp_compile:
        user_compile_flags.extend(ctx.fragments.cpp.cxxopts)
    elif action_name == ACTION_NAMES.c_compile and hasattr(ctx.fragments.cpp, "conlyopts"):
        user_compile_flags.extend(ctx.fragments.cpp.conlyopts)
    compile_variables = cc_common.create_compile_variables(
        feature_configuration = feature_configuration,
        cc_toolchain = cc_toolchain,
        user_compile_flags = user_compile_flags,
    )
    return cc_common.get_memory_inefficient_command_line(
        feature_configuration = feature_configuration,
        action_name = action_name,
        variables = compile_variables,
    )

def _deps_flags(deps):
    """Return `(flags, transitive_headers_depset)` for the given CcInfo deps."""
    compilation_contexts = [d[CcInfo].compilation_context for d in deps if CcInfo in d]
    headers = depset(transitive = [c.headers for c in compilation_contexts])
    flags = []
    for c in compilation_contexts:
        for define in c.defines.to_list():
            flags.append("-D" + define)
        for define in c.local_defines.to_list():
            flags.append("-D" + define)
        for i in c.framework_includes.to_list():
            flags.append("-F" + i)
        for i in c.includes.to_list():
            flags.append("-I" + i)
        for i in c.quote_includes.to_list():
            flags.extend(["-iquote", i])
        for i in c.system_includes.to_list():
            flags.extend(["-isystem", i])
        for i in c.external_includes.to_list():
            flags.extend(["-isystem", i])
    return flags, headers

def _safe_flags(flags):
    return [f for f in flags if f not in _UNSUPPORTED_FLAGS]

def _fix_path(ctx, arg):
    # Test runfiles use a different layout than compile actions; map the
    # bazel-bin prefix back to "." so generated headers resolve.
    return arg.replace(ctx.bin_dir.path, ".")

def _include_cleaner_test_impl(ctx):
    cc_toolchain = ctx.exec_groups["test"].toolchains["@bazel_tools//tools/cpp:toolchain_type"].cc

    dep_flags, dep_headers = _deps_flags(ctx.attr.deps)
    rule_copts = [
        ctx.expand_make_variables("copts", c, {})
        for c in ctx.attr.copts
    ]

    cxx_flags = _safe_flags(
        _toolchain_flags(ctx, cc_toolchain, ACTION_NAMES.cpp_compile) + dep_flags + rule_copts,
    ) + ["-xc++"]

    # Lint sources AND headers. Header-only libraries would otherwise have
    # nothing to check, and headers are exactly where stale includes hide.
    all_files = ctx.files.srcs + ctx.files.hdrs
    lint_files = [f for f in all_files if _is_source(f) or _is_header(f)]

    if not lint_files:
        fail("include_cleaner_lint_test '{}' has no source/header files to lint".format(ctx.label.name))

    config_short_path = ctx.file.config.short_path
    file_args = " ".join([_shell_quote(f.short_path) for f in lint_files])
    flag_args = " ".join([_shell_quote(_fix_path(ctx, f)) for f in cxx_flags])

    script = """#!/usr/bin/env bash
# Auto-generated by build_defs/include_cleaner.bzl — do not edit.
set -euo pipefail

# clang-tidy resolves -isystem external/... relative to the cwd; the test
# runfiles tree puts those one level up, so symlink `external` next to us.
[[ -e external ]] || ln -s .. external

# Drop a local .clang-tidy config so clang-tidy's automatic config discovery
# picks up our narrow include-cleaner-only ruleset rather than the workspace
# .clang-tidy (which would re-enable the broader check set).
cp -f {config} .clang-tidy

if [[ -n "${{CLANG_TIDY:-}}" ]]; then
  : # honour explicit override
elif command -v clang-tidy-19 >/dev/null 2>&1; then
  CLANG_TIDY="clang-tidy-19"
elif command -v clang-tidy >/dev/null 2>&1; then
  CLANG_TIDY="clang-tidy"
else
  echo "include_cleaner_lint: clang-tidy not found in PATH" >&2
  echo "  Install clang-tidy 19+ (e.g. 'apt-get install clang-tidy-19' on" >&2
  echo "  Debian/Ubuntu, 'brew install llvm@19' on macOS) or set CLANG_TIDY" >&2
  echo "  to a usable binary." >&2
  exit 1
fi
if ! command -v "$CLANG_TIDY" >/dev/null 2>&1; then
  echo "include_cleaner_lint: clang-tidy override '$CLANG_TIDY' not executable" >&2
  exit 1
fi

EXTRA_ARGS=()
if [[ "$(uname)" == "Darwin" ]]; then
  # Bazel's macOS cc toolchain doesn't emit -isysroot through
  # cc_common.get_memory_inefficient_command_line, so a non-Apple
  # clang-tidy (e.g. brew's llvm@19) can't find macOS SDK headers like
  # <stdlib.h> / <math.h>. We also have to swap brew's bundled libc++
  # for Apple's, because brew's libc++ expects a glibc-style C library
  # and trips over Apple's <math.h>/<stdlib.h> definitions (missing
  # FP_NAN, ldiv_t, etc.). Force -nostdinc++ and point clang-tidy at
  # the Apple toolchain's libc++ headers instead.
  if command -v xcrun >/dev/null 2>&1; then
    SDK_PATH="$(xcrun --show-sdk-path 2>/dev/null || true)"
    if [[ -n "$SDK_PATH" ]]; then
      EXTRA_ARGS+=(-isysroot "$SDK_PATH")
    fi
  fi
  if command -v xcode-select >/dev/null 2>&1; then
    DEV_DIR="$(xcode-select -p 2>/dev/null || true)"
    if [[ -n "$DEV_DIR" ]]; then
      APPLE_CXX="$DEV_DIR/Toolchains/XcodeDefault.xctoolchain/usr/include/c++/v1"
      if [[ -d "$APPLE_CXX" ]]; then
        EXTRA_ARGS+=(-nostdinc++ -isystem "$APPLE_CXX")
      fi
    fi
  fi
fi
# `--quiet` suppresses the per-file "N warnings generated" banner; the
# test_output=errors stream stays clean unless misc-include-cleaner fires.
exec "$CLANG_TIDY" --quiet {files} -- {flags} "${{EXTRA_ARGS[@]}}"
""".format(
        config = _shell_quote(config_short_path),
        files = file_args,
        flags = flag_args,
    )

    ctx.actions.write(
        output = ctx.outputs.executable,
        is_executable = True,
        content = script,
    )

    runfiles = ctx.runfiles(
        files = lint_files + [ctx.file.config],
        transitive_files = depset(transitive = [dep_headers, cc_toolchain.all_files]),
    )

    return [DefaultInfo(executable = ctx.outputs.executable, runfiles = runfiles)]

_include_cleaner_test = rule(
    implementation = _include_cleaner_test_impl,
    test = True,
    fragments = ["cpp"],
    attrs = {
        "srcs": attr.label_list(allow_files = True),
        "hdrs": attr.label_list(allow_files = True),
        "deps": attr.label_list(providers = [CcInfo]),
        "copts": attr.string_list(),
        "config": attr.label(
            default = Label("//build_defs:include_cleaner.clang-tidy"),
            allow_single_file = True,
        ),
    },
    exec_groups = {
        "test": exec_group(
            toolchains = ["@bazel_tools//tools/cpp:toolchain_type"],
        ),
    },
)

def include_cleaner_lint_test(name, srcs, hdrs, deps, copts = [], tags = [], **kwargs):
    """Emit a per-library `misc-include-cleaner` test.

    Tagged `lint` and `include_cleaner` so it can be filtered separately
    (`bazel test //... --test_tag_filters=-include_cleaner`). When the
    parent target carries `manual` it is propagated so the lint doesn't
    sneak into `bazel test //...` against an opt-out target.

    Args:
      name: Test target name.
      srcs: Source files to lint.
      hdrs: Header files to lint.
      deps: Dependencies that provide CcInfo (compile flags / include paths).
      copts: Extra compile options matching the parent rule.
      tags: Tags from the parent rule.
      **kwargs: Forwarded to the underlying test rule (e.g. visibility).
    """
    propagated_tags = list(tags) + ["lint", "include_cleaner"]
    if "manual" in tags and "manual" not in propagated_tags:
        propagated_tags.append("manual")

    _include_cleaner_test(
        name = name,
        srcs = srcs,
        hdrs = hdrs,
        deps = deps,
        copts = copts,
        tags = propagated_tags,
        size = "small",
        **kwargs
    )
