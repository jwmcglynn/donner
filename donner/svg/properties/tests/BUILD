load("@rules_cc//cc:defs.bzl", "cc_test")

cc_test(
    name = "properties_tests",
    srcs = [
        "PaintServer_tests.cc",
        "PropertyRegistry_tests.cc",
    ],
    deps = [
        "//donner/base:base_test_utils",
        "//donner/svg",
        "//donner/svg/properties",
        "@com_google_gtest//:gtest_main",
    ],
)
