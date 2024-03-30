load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")
load("@bazel_tools//tools/build_defs/repo:utils.bzl", "maybe")
load("@rules_foreign_cc//foreign_cc:repositories.bzl", "rules_foreign_cc_dependencies")

rules_foreign_cc_dependencies()

def bloaty_dependencies():
    maybe(
        http_archive,
        name = "rules_foreign_cc",
        strip_prefix = "rules_foreign_cc-14ded03b9c835e9ccba6a20f0944729dccab8d9b",
        url = "https://github.com/bazelbuild/rules_foreign_cc/archive/14ded03b9c835e9ccba6a20f0944729dccab8d9b.tar.gz",
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
