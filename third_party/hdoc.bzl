load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")
load("@bazel_tools//tools/build_defs/repo:utils.bzl", "maybe")

def _asset_to_cpp_impl(ctx):
    # Run xxd -i to convert the input file to a C++ array.
    optional_args = [
        "-n",
        ctx.attr.variable_name,
    ] if ctx.attr.variable_name else []

    ctx.actions.run(
        inputs = [ctx.file.src],
        outputs = [ctx.outputs.out],
        executable = ctx.executable._xxd,
        arguments = [
            "-i",
        ] + optional_args + [
            ctx.file.src.path,
            ctx.outputs.out.path,
        ],
    )

asset_to_cpp = rule(
    implementation = _asset_to_cpp_impl,
    attrs = {
        "src": attr.label(mandatory = True, allow_single_file = True),
        "out": attr.output(mandatory = True),
        "variable_name": attr.string(),
        "_xxd": attr.label(
            executable = True,
            default = "@donner//third_party/xxd",
            cfg = "exec",
        ),
    },
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
        sha256 = "cd563293580b9dc592254df35b49cf8a19b4870ff5f611c7584cf967d9e6031e",
        strip_prefix = "argparse-2.9",
        urls = ["https://github.com/p-ranav/argparse/archive/refs/tags/v2.9.tar.gz"],
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
        sha256 = "6fff9215f5cb81760be4cc16d033526d1080427d236e86d70bb02994f85e3d38",
        strip_prefix = "spdlog-1.9.2",
        urls = ["https://github.com/gabime/spdlog/archive/v1.9.2.tar.gz"],
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
        sha256 = "aeba776441df4ac32e4d4db9d835532db3f90fd530a28b74e4751a2915a55565",
        strip_prefix = "tomlplusplus-3.2.0",
        urls = ["https://github.com/marzer/tomlplusplus/archive/v3.2.0.tar.gz"],
        build_file_content = """
load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "tomlplusplus",
    hdrs = glob(["include/**/*.h", "include/**/*.inl"]),
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
        sha256 = "bca173d66f8a93ad5ac2a3b28c86da0b95f378613d6f25791af0153944960b37",
        strip_prefix = "rapidjson-27c3a8dc0e2c9218fe94986d249a12b5ed838f1d",
        urls = ["https://github.com/Tencent/rapidjson/archive/27c3a8dc0e2c9218fe94986d249a12b5ed838f1d.tar.gz"],
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
