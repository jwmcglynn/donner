load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

cc_test(
    name = "svg_tests",
    srcs = [
        "svg_circle_tests.cc",
        "svg_element_tests.cc",
    ],
    deps = [
        ":xml_test_utils",
        "//src/svg",
        "@com_google_gtest//:gtest_main",
    ],
)

cc_library(
    name = "xml_test_utils",
    testonly = 1,
    srcs = [
        "xml_test_utils.cc",
    ],
    hdrs = [
        "xml_test_utils.h",
    ],
    visibility = ["//src:__subpackages__"],
    deps = [
        "//src/svg",
        "//src/svg/xml",
        "@com_google_gtest//:gtest",
    ],
)
