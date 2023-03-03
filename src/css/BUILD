load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

# The public API for CSS.
cc_library(
    name = "css",
    srcs = [
        "css.cc",
    ],
    hdrs = [
        "css.h",
    ],
    visibility = [
        "//src/svg:__subpackages__",
    ],
    deps = [
        ":core",
        "//src/css/parser",
    ],
)

cc_library(
    name = "core",
    srcs = [
        "color.cc",
        "component_value.cc",
        "declaration.cc",
        "rule.cc",
    ],
    hdrs = [
        "color.h",
        "component_value.h",
        "declaration.h",
        "rule.h",
        "selector.h",
        "specificity.h",
        "stylesheet.h",
        "token.h",
    ],
    visibility = [
        "//src/css:__subpackages__",
        "//src/svg:__subpackages__",
    ],
    deps = [
        "//src/base",
        "@frozen",
    ],
)

cc_test(
    name = "css_tests",
    srcs = [
        "tests/selector_tests.cc",
        "tests/specificity_tests.cc",
    ],
    deps = [
        ":core",
        "//src/base:base_test_utils",
        "//src/base/parser:parser_test_utils",
        "//src/css/parser",
        "//src/svg/components",
        "@com_google_gtest//:gtest_main",
    ],
)
