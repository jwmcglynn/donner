load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

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
    name = "image_loader",
    srcs = [
        "Base64.cc",
        "ImageLoader.cc",
    ],
    hdrs = [
        "Base64.h",
        "ImageLoader.h",
    ],
    visibility = ["//donner/svg:__subpackages__"],
    deps = [
        ":image_resource",
        ":resource_loader_interface",
        "//donner/base",
        "//donner/base/parser",
        "@stb//:image",
    ],
)

cc_test(
    name = "base64_tests",
    srcs = ["tests/Base64_tests.cc"],
    deps = [
        ":image_loader",
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
