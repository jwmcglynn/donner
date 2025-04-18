load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")
load("//build_defs:rules.bzl", "donner_cc_fuzzer")

cc_library(
    name = "sandboxed_file_resource_loader",
    srcs = ["SandboxedFileResourceLoader.cc"],
    hdrs = ["SandboxedFileResourceLoader.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":resource_loader_interface",
        "//donner/base",
    ],
)

cc_library(
    name = "image_resource",
    hdrs = ["ImageResource.h"],
    visibility = ["//donner/svg:__subpackages__"],
)

cc_library(
    name = "resource_loader_interface",
    hdrs = ["ResourceLoaderInterface.h"],
    visibility = ["//donner/svg:__subpackages__"],
)

cc_library(
    name = "url_loader",
    srcs = [
        "Base64.cc",
        "UrlDecode.cc",
        "UrlLoader.cc",
    ],
    hdrs = [
        "Base64.h",
        "UrlDecode.h",
        "UrlLoader.h",
    ],
    deps = [
        ":resource_loader_interface",
        "//donner/base",
        "//donner/base/parser",
        "@stb//:image",
    ],
)

cc_library(
    name = "image_loader",
    srcs = [
        "ImageLoader.cc",
    ],
    hdrs = [
        "ImageLoader.h",
    ],
    visibility = ["//donner/svg:__subpackages__"],
    deps = [
        ":image_resource",
        ":url_loader",
        "//donner/base",
        "//donner/base/parser",
        "@stb//:image",
    ],
)

cc_test(
    name = "url_loader_tests",
    srcs = [
        "tests/Base64_tests.cc",
        "tests/UrlDecode_tests.cc",
        "tests/UrlLoader_tests.cc",
    ],
    deps = [
        ":url_loader",
        "//donner/base:base_test_utils",
        "@com_google_gtest//:gtest_main",
    ],
)

cc_test(
    name = "sandboxed_file_resource_loader_tests",
    srcs = ["tests/SandboxedFileResourceLoader_tests.cc"],
    deps = [
        ":sandboxed_file_resource_loader",
        "@com_google_gtest//:gtest_main",
    ],
)

donner_cc_fuzzer(
    name = "url_loader_fuzzer",
    srcs = [
        "tests/UrlLoader_fuzzer.cc",
    ],
    corpus = "tests/url_loader_corpus",
    deps = [
        ":url_loader",
    ],
)
