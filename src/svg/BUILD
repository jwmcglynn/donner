load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

cc_library(
    name = "svg",
    srcs = [
        "svg_document.cc",
        "svg_element.cc",
        "svg_path_element.cc",
        "svg_svg_element.cc",
        "svg_unknown_element.cc",
    ],
    hdrs = [
        "svg_document.h",
        "svg_element.h",
        "svg_path_element.h",
        "svg_svg_element.h",
        "svg_unknown_element.h",
    ],
    visibility = [
        "//:__pkg__",
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
        "@gtest//:gtest_main",
    ],
)
