load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

cc_library(
    name = "properties",
    srcs = [
        "property_registry.cc",
    ],
    hdrs = [
        "property_registry.h",
    ],
    visibility = ["//src/svg:__subpackages__"],
    deps = [
        "//src/base/parser",
        "//src/css",
        "//src/css/parser",
        "@frozen",
    ],
)

cc_test(
    name = "properties_tests",
    srcs = [
        "tests/property_registry_tests.cc",
    ],
    deps = [
        ":properties",
        "//src/base/parser:parser_test_utils",
        "@com_google_gtest//:gtest_main",
    ],
)
