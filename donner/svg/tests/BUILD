load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

cc_test(
    name = "svg_tests",
    srcs = [
        "ElementStyle_tests.cc",
        "SVGCircleElement_tests.cc",
        "SVGClipPathElement_tests.cc",
        "SVGDefsElement_tests.cc",
        "SVGDocument_tests.cc",
        "SVGElement_tests.cc",
        "SVGEllipseElement_tests.cc",
        "SVGFilterElement_tests.cc",
        "SVGLinearGradientElement_tests.cc",
        "SVGMarkerElement_tests.cc",
        "SVGPathElement_tests.cc",
        "SVGPatternElement_tests.cc",
        "SVGRadialGradientElement_tests.cc",
        "SVGRectElement_tests.cc",
    ],
    deps = [
        ":parser_test_utils",
        "//donner/base:base_test_utils",
        "//donner/svg",
        "//donner/svg/core/tests:core_test_utils",
        "//donner/svg/renderer/tests:renderer_test_utils",
        "@com_google_gtest//:gtest_main",
    ],
)

cc_library(
    name = "parser_test_utils",
    testonly = 1,
    srcs = [
        "ParserTestUtils.cc",
    ],
    hdrs = [
        "ParserTestUtils.h",
    ],
    visibility = ["//donner:__subpackages__"],
    deps = [
        "//donner/svg",
        "//donner/svg/parser",
        "@com_google_gtest//:gtest",
    ],
)
