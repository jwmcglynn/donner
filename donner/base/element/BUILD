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
        "//donner/base/xml",
    ],
)

donner_cc_library(
    name = "fake_element",
    testonly = 1,
    hdrs = [
        "tests/FakeElement.h",
    ],
    visibility = donner_internal_visibility() + [
        "//examples:__pkg__",
    ],
    deps = [
        "@com_google_gtest//:gtest",
    ],
)

cc_test(
    name = "element_tests",
    srcs = [
        "tests/ElementTraversalGenerators_tests.cc",
        "tests/FakeElement_tests.cc",
    ],
    deps = [
        ":element",
        ":fake_element",
        "//donner/base:base_test_utils",
        "@com_google_gtest//:gtest_main",
    ],
)
