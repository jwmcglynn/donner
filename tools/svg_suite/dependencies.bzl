"""Release-pinned native suite evidence dependency, used only by development tools."""

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

def _suite_deps_impl(_ctx):
    http_archive(
        name = "picosha2",
        urls = ["https://github.com/okdshin/PicoSHA2/archive/refs/tags/v1.0.1.tar.gz"],
        sha256 = "9983136544234e573fe07cc1a22fdf978ad7979043e7be2e9082f6d4991ff8a8",
        strip_prefix = "PicoSHA2-1.0.1",
        build_file_content = """load("@rules_cc//cc:defs.bzl", "cc_library")
cc_library(name = "picosha2", hdrs = ["picosha2.h"], includes = ["."], visibility = ["//visibility:public"])
exports_files(["LICENSE"])
""",
    )

suite_deps = module_extension(implementation = _suite_deps_impl)
