"""
Defines the libclang_cc_binary rule, which allows building binaries that depend on libclang and
libstdc++ from within a build system that uses libc++ by default.

To use, first create a cc_binary and then a libclang_cc_binary rule that wraps it:
```
load(":libclang_cc_binary.bzl", "libclang_cc_binary", "require_libclang")

cc_binary(
    name = "example_bin",
    srcs = ["main.cpp"],
    target_compatible_with = require_libclang(),
)

libclang_cc_binary(
    name = "example",
    actual_binary = ":example_bin",
    visibility = ["//visibility:public"],
)
```

To avoid compilation errors when doing a wildcard build (`bazel build //...`), add the
target_compatible_with attribute to any rule that directly links against libclang:
```
    target_compatible_with = require_libclang(),
```
"""

def require_libclang():
    return select({
        "//tools/clang-plugin:libclang_build": [],
        "//conditions:default": ["@platforms//:incompatible"],
    })

def _libclang_transition_impl(settings, _attr):
    cpu = settings["//command_line_option:cpu"]
    is_apple = cpu.startswith("darwin_")

    return {
        "//command_line_option:cxxopt": ([] if is_apple else [
            "-stdlib=libstdc++",
            "-D_GLIBCXX_USE_CXX11_ABI=1",
        ]) + [
            "-fno-rtti",
        ],
        "//command_line_option:linkopt": [] if is_apple else [
            "-stdlib=libstdc++",
            "-lstdc++",
        ],
        "//tools/clang-plugin:setting_libclang_build": True,
    }

_libclang_transition = transition(
    implementation = _libclang_transition_impl,
    inputs = ["//command_line_option:cpu"],
    outputs = [
        "//command_line_option:cxxopt",
        "//command_line_option:linkopt",
        "//tools/clang-plugin:setting_libclang_build",
    ],
)

def _libclang_cc_binary_impl(ctx):
    actual_binary = ctx.attr.actual_binary[0]
    outfile = ctx.actions.declare_file(ctx.label.name)
    cc_binary_outfile = actual_binary[DefaultInfo].files.to_list()[0]

    ctx.actions.run_shell(
        inputs = [cc_binary_outfile],
        outputs = [outfile],
        command = "cp %s %s" % (cc_binary_outfile.path, outfile.path),
    )
    return [
        DefaultInfo(
            executable = outfile,
            data_runfiles = actual_binary[DefaultInfo].data_runfiles,
        ),
    ]

libclang_cc_binary = rule(
    implementation = _libclang_cc_binary_impl,
    attrs = {
        "actual_binary": attr.label(cfg = _libclang_transition),
        "_allowlist_function_transition": attr.label(
            default = "@bazel_tools//tools/allowlists/function_transition_allowlist",
        ),
    },
    executable = True,
)
