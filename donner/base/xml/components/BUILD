load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

cc_library(
    name = "components",
    srcs = [
        "TreeComponent.cc",
    ],
    hdrs = [
        "AttributesComponent.h",
        "TreeComponent.h",
    ],
    visibility = [
        "//donner/base/xml:__subpackages__",
        "//donner/svg:__subpackages__",
    ],
    deps = [
        "//donner/base",
        "//donner/base/xml",
    ],
)

cc_test(
    name = "components_tests",
    srcs = [
        "tests/AttributesComponent_tests.cc",
        "tests/TreeComponent_tests.cc",
    ],
    deps = [
        ":components",
        "//donner/base:base_test_utils",
        "@com_google_gtest//:gtest_main",
    ],
)
