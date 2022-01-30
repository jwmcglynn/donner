load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

cc_library(
    name = "svg",
    srcs = [
        "svg_circle_element.cc",
        "svg_defs_element.cc",
        "svg_document.cc",
        "svg_element.cc",
        "svg_path_element.cc",
        "svg_rect_element.cc",
        "svg_style_element.cc",
        "svg_svg_element.cc",
        "svg_unknown_element.cc",
        "svg_use_element.cc",
    ],
    hdrs = [
        "svg_circle_element.h",
        "svg_defs_element.h",
        "svg_document.h",
        "svg_element.h",
        "svg_path_element.h",
        "svg_rect_element.h",
        "svg_style_element.h",
        "svg_svg_element.h",
        "svg_unknown_element.h",
        "svg_use_element.h",
    ],
    visibility = [
        "//:__pkg__",
        "//src/renderer:__subpackages__",
        "//src/svg/xml:__pkg__",
    ],
    deps = [
        "//src/svg/components",
        "//src/svg/core",
        "//src/svg/parser",
    ],
)

cc_test(
    name = "svg_tests",
    srcs = [
        "tests/svg_element_tests.cc",
    ],
    deps = [
        ":svg",
        "@com_google_gtest//:gtest_main",
    ],
)
