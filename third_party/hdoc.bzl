load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")
load("@bazel_tools//tools/build_defs/repo:utils.bzl", "maybe")
load("@rules_cc//cc:defs.bzl", "cc_library")

def require_libclang():
    return select({
        "@hdoc//:libclang_build": [],
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
            "-std=c++20",
        ],
        "//command_line_option:linkopt": [] if is_apple else [
            "-stdlib=libstdc++",
            "-lstdc++",
        ],
        "@hdoc//:setting_libclang_build": True,
    }

_libclang_transition = transition(
    implementation = _libclang_transition_impl,
    inputs = ["//command_line_option:cpu"],
    outputs = [
        "//command_line_option:cxxopt",
        "//command_line_option:linkopt",
        "@hdoc//:setting_libclang_build",
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

def _libclang_cc_test_impl(ctx):
    actual_test = ctx.executable.actual_test
    outfile = ctx.actions.declare_file(ctx.label.name)

    ctx.actions.run_shell(
        inputs = [actual_test],
        outputs = [outfile],
        command = "cp %s %s" % (actual_test.path, outfile.path),
    )
    return [
        DefaultInfo(
            executable = outfile,
            runfiles = ctx.attr.actual_test[0][DefaultInfo].default_runfiles,
        ),
    ]

libclang_cc_test = rule(
    implementation = _libclang_cc_test_impl,
    attrs = {
        "actual_test": attr.label(cfg = _libclang_transition, executable = True),
        "_allowlist_function_transition": attr.label(
            default = "@bazel_tools//tools/allowlists/function_transition_allowlist",
        ),
    },
    executable = True,
    test = True,
)

def _sanitize_filename(filename):
    """Returns a sanitized version of the given filename.

    This is used to generate C++ symbols from filenames, so that they can be used as identifiers.
    """
    return "".join(
        [ch if ch.isalnum() else "_" for ch in filename.elems()],
    )

def _asset_to_cpp_impl(ctx):
    """Run `xxd -i` to convert the input file to a C array.

    This is used to embed assets in the binary with the same symbols as hdoc's original Meson build
    system.

    For the given filename, an array will be defined based on the original filename, with all
    non-alphanumeric characters replaced with underscore.

    For example, `schemas/hdoc-payload-schema.json` will have two constants defined in the output
    file:
    - `___schemas_hdoc_payload_schema_json`
    - `___schemas_hdoc_payload_schema_json_len`

    To use them, add the following `extern` declarations to your code:

    ```cpp
    extern uint8_t      ___schemas_hdoc_payload_schema_json[];
    extern unsigned int ___schemas_hdoc_payload_schema_json_len;
    ```
    """

    variable_name = "___" + _sanitize_filename(ctx.file.src.short_path.removeprefix("../hdoc/"))

    ctx.actions.run(
        inputs = [ctx.file.src],
        outputs = [ctx.outputs.out],
        executable = ctx.executable._xxd,
        arguments = [
            "-i",
            "-n",
            variable_name,
            ctx.file.src.path,
            ctx.outputs.out.path,
        ],
    )

asset_to_cpp = rule(
    implementation = _asset_to_cpp_impl,
    attrs = {
        "src": attr.label(mandatory = True, allow_single_file = True),
        "out": attr.output(mandatory = True),
        "_xxd": attr.label(
            executable = True,
            default = "@donner//third_party/xxd",
            cfg = "exec",
        ),
    },
)

def bundle_assets(name, srcs, **kwargs):
    """Create a cc_library containing the given assets, converted to C arrays with asset_to_cpp.

    Args:
        name: The name of the cc_library to create.
        srcs: A list of filenames pointing to the assets to convert to C arrays.
        **kwargs: Additional arguments to pass to cc_library.
    """

    assets = []
    for src in srcs:
        filename = _sanitize_filename(src)
        asset_to_cpp(
            name = name + "_" + filename,
            src = src,
            out = _sanitize_filename(src) + ".cpp",
        )

        assets.append(":" + name + "_" + filename)

    cc_library(
        name = name,
        srcs = assets,
        visibility = ["//visibility:public"],
        **kwargs
    )

def hdoc_dependencies():
    """hdoc_dependencies defines the dependencies needed to build hdoc.

    It loads the necessary Bazel repository rules and calls them to fetch:

    - argparse: For command line argument parsing.
    - spdlog: Fast C++ logging library.
    - tomlplusplus: C++ TOML parser for config files.
    - rapidjson: JSON serialization library.

    These are defined as maybe() calls so that if a dependency is already
    registered, it will be skipped.
    """
    maybe(
        http_archive,
        name = "argparse",
        sha256 = "ba7b465759bb01069d57302855eaf4d1f7d677f21ad7b0b00b92939645c30f47",
        strip_prefix = "argparse-3.0",
        urls = ["https://github.com/p-ranav/argparse/archive/refs/tags/v3.0.tar.gz"],
        build_file_content = """
load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "argparse",
    hdrs = ["include/argparse/argparse.hpp"],
    strip_include_prefix = "include",
    visibility = ["//visibility:public"],
)
        """,
    )

    maybe(
        http_archive,
        name = "spdlog",
        sha256 = "534f2ee1a4dcbeb22249856edfb2be76a1cf4f708a20b0ac2ed090ee24cfdbc9",
        strip_prefix = "spdlog-1.13.0",
        urls = ["https://github.com/gabime/spdlog/archive/v1.13.0.tar.gz"],
        build_file_content = """
load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "spdlog",
    hdrs = glob(["include/**/*.h"]),
    srcs = glob(["src/**/*.cpp"]),
    strip_include_prefix = "include",
    visibility = ["//visibility:public"],
    defines = ["SPDLOG_COMPILED_LIB"],
)
        """,
    )

    maybe(
        http_archive,
        name = "tomlplusplus",
        sha256 = "8517f65938a4faae9ccf8ebb36631a38c1cadfb5efa85d9a72e15b9e97d25155",
        strip_prefix = "tomlplusplus-3.4.0",
        urls = ["https://github.com/marzer/tomlplusplus/archive/v3.4.0.tar.gz"],
        build_file_content = """
load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "tomlplusplus",
    hdrs = glob(["include/**/*.h", "include/**/*.hpp", "include/**/*.inl"]),
    srcs = glob(["src/**/*.cpp"]),
    strip_include_prefix = "include",
    visibility = ["//visibility:public"],
    defines = ["TOML_HEADER_ONLY=0"],
)
        """,
    )

    maybe(
        http_archive,
        name = "rapidjson",
        sha256 = "bc23236d26360ab2eccea85619068e757517269ce626c5bbc97c6881b921763a",
        strip_prefix = "rapidjson-5ec44fb9206695e5293f610b0a46d21851d0c966",
        urls = ["https://github.com/Tencent/rapidjson/archive/5ec44fb9206695e5293f610b0a46d21851d0c966.tar.gz"],
        build_file_content = """
load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "rapidjson",
    hdrs = glob(["include/**/*.h"]),
    strip_include_prefix = "include",
    visibility = ["//visibility:public"],
    defines = ["RAPIDJSON_HAS_STDSTRING"],
)
        """,
    )

    maybe(
        http_archive,
        name = "doctest",
        strip_prefix = "doctest-2.4.11",
        urls = ["https://github.com/doctest/doctest/archive/v2.4.11.tar.gz"],
    )

    maybe(
        http_archive,
        name = "cmark-gfm",
        strip_prefix = "cmark-gfm-0.29.0.gfm.11",
        urls = ["https://github.com/github/cmark-gfm/archive/0.29.0.gfm.11.tar.gz"],
        build_file = "//third_party:cmark-gfm.BUILD",
    )
