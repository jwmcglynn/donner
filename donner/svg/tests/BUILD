load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

cc_test(
    name = "svg_tests",
    srcs = [
        "ElementStyle_tests.cc",
        "SVGCircleElement_tests.cc",
        "SVGEllipseElement_tests.cc",
        "SVGFilterElement_tests.cc",
        "SVGLinearGradientElement_tests.cc",
        "SVGPathElement_tests.cc",
        "SVGPatternElement_tests.cc",
        "SVGRadialGradientElement_tests.cc",
        "SVGRectElement_tests.cc",
        "SVGelement_tests.cc",
    ],
    deps = [
        ":xml_test_utils",
        "//donner/base:base_test_utils",
        "//donner/svg",
        "//donner/svg/core:core_test_utils",
        "//donner/svg/renderer/tests:renderer_test_utils",
        "@com_google_gtest//:gtest_main",
    ],
)

cc_library(
    name = "xml_test_utils",
    testonly = 1,
    srcs = [
        "XMLTestUtils.cc",
    ],
    hdrs = [
        "XMLTestUtils.h",
    ],
    visibility = ["//donner:__subpackages__"],
    deps = [
        "//donner/svg",
        "//donner/svg/xml",
        "@com_google_gtest//:gtest",
    ],
)
