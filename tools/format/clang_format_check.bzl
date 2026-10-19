"""A build-time check that C++ sources match the repository's clang-format output."""

_CPP_EXTENSIONS = ("cc", "cpp", "h", "hpp", "mm")

def clang_format_platform_compatible_with():
    """`target_compatible_with` for anything that runs the pinned formatter on the target platform.

    The platforms here are the ones //tools/format:clang_format pins a binary for.
    """
    return select({
        "//tools/format:linux_aarch64": [],
        "//tools/format:linux_x86_64": [],
        "//tools/format:macos_arm64": [],
        "//conditions:default": ["@platforms//:incompatible"],
    })

def clang_format_check_compatible_with():
    """`target_compatible_with` for a format check: on when enabled and the formatter is pinned.

    Donner's .bazelrc enables //tools/format:check; modules that depend on donner leave it off.
    On a platform without a pinned formatter the check is skipped, and `tools/lint.sh` remains the
    whole-tree check.
    """
    return select({
        "//tools/format:check_enabled": [],
        "//conditions:default": ["@platforms//:incompatible"],
    }) + clang_format_platform_compatible_with()

def _clang_format_check_impl(ctx):
    sources = [
        f
        for f in ctx.files.srcs
        if f.is_source and f.owner.workspace_name == "" and f.extension in _CPP_EXTENSIONS
    ]
    stamp = ctx.actions.declare_file(ctx.label.name + ".clang_format_ok")
    if not sources:
        ctx.actions.write(stamp, "")
    else:
        args = ctx.actions.args()
        args.add(ctx.executable._clang_format)
        args.add(ctx.file._style)
        args.add(stamp)
        args.add_all(sources)
        ctx.actions.run_shell(
            inputs = sources + [ctx.file._style],
            tools = [ctx.executable._clang_format],
            outputs = [stamp],
            arguments = [args],
            command = 'formatter="$1" style="$2" stamp="$3"; shift 3; ' +
                      '"$formatter" --dry-run --Werror --style="file:$style" "$@" && : > "$stamp"',
            mnemonic = "ClangFormatCheck",
            progress_message = "Checking clang-format for %{label}",
        )
    return [
        DefaultInfo(files = depset()),
        OutputGroupInfo(_validation = depset([stamp])),
    ]

clang_format_check = rule(
    implementation = _clang_format_check_impl,
    doc = """Fails the build when a listed C++ source differs from //tools/format:clang_format.

The check is a validation action, so it runs whenever the target is built, including under
`bazel build //...` and `bazel test //...`, and is cached per source content. Generated files,
files from other repositories and non-C++ files in `srcs` are ignored.""",
    attrs = {
        "srcs": attr.label_list(allow_files = True),
        "_clang_format": attr.label(
            default = "//tools/format:clang_format",
            executable = True,
            cfg = "exec",
        ),
        "_style": attr.label(
            default = "//:clang_format_config",
            allow_single_file = True,
        ),
    },
)
