load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

cc_test(
    name = "svg_tests",
    srcs = [
        "element_style_tests.cc",
        "svg_circle_element_tests.cc",
        "svg_element_tests.cc",
        "svg_ellipse_element_tests.cc",
        "svg_filter_element_tests.cc",
        "svg_linear_gradient_element_tests.cc",
        "svg_path_element_tests.cc",
        "svg_pattern_element_tests.cc",
        "svg_radial_gradient_element_tests.cc",
        "svg_rect_element_tests.cc",
    ],
    deps = [
        ":xml_test_utils",
        "//src/base:base_test_utils",
        "//src/svg",
        "//src/svg/core:core_test_utils",
        "//src/svg/renderer/tests:renderer_test_utils",
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
