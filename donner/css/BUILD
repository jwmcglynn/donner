load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")
load("//build_defs:rules.bzl", "donner_cc_library")

# The public API for CSS.
donner_cc_library(
    name = "css",
    srcs = [
        "CSS.cc",
    ],
    hdrs = [
        "CSS.h",
    ],
    visibility = [
        "//donner/svg:__subpackages__",
    ],
    deps = [
        ":core",
        "//donner/css/parser",
    ],
)

donner_cc_library(
    name = "core",
    srcs = [
        "Color.cc",
        "ComponentValue.cc",
        "Declaration.cc",
        "Rule.cc",
    ],
    hdrs = [
        "Color.h",
        "ComponentValue.h",
        "Declaration.h",
        "Rule.h",
        "Selector.h",
        "SelectorTraversal.h",
        "Specificity.h",
        "Stylesheet.h",
        "Token.h",
        "details/AnbValue.h",
    ],
    visibility = [
        "//donner/css:__subpackages__",
        "//donner/svg:__subpackages__",
    ],
    deps = [
        "//donner/base",
        "//donner/base/parser:file_offset",
        "//donner/svg/xml:xml_qualified_name",
        "@frozen",
    ],
)

cc_test(
    name = "css_tests",
    srcs = [
        "tests/Color_tests.cc",
        "tests/Selector_tests.cc",
        "tests/Specificity_tests.cc",
    ],
    deps = [
        ":core",
        "//donner/base:base_test_utils",
        "//donner/base/parser:parser_test_utils",
        "//donner/css/parser",
        "//donner/svg/components",
        "@com_google_gtest//:gtest_main",
    ],
)