load("//build_defs:visibility.bzl", "donner_internal_visibility")
load("//build_defs:rules.bzl", "donner_cc_library")
load("@rules_cc//cc:defs.bzl", "cc_test")

donner_cc_library(
    name = "element",
    hdrs = [
        "ElementLike.h",
        "ElementTraversalGenerators.h",
    ],
    visibility = donner_internal_visibility(),
    deps = [
        "//donner/base",
    ],
)

cc_test(
    name = "element_tests",
    srcs = [
        "tests/ElementTraversalGenerators_tests.cc",
    ],
    deps = [
        ":element",
        "//donner/base:base_test_utils",
        "@com_google_gtest//:gtest_main",
    ],
)
