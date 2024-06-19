load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")
load("//build_defs:rules.bzl", "donner_cc_fuzzer")

cc_library(
    name = "parser",
    srcs = [
        "AngleParser.cc",
        "CssTransformParser.cc",
        "LengthPercentageParser.cc",
        "Number2dParser.cc",
        "PathParser.cc",
        "PointsListParser.cc",
        "PreserveAspectRatioParser.cc",
        "TransformParser.cc",
        "ViewboxParser.cc",
    ],
    hdrs = [
        "AngleParser.h",
        "CssTransformParser.h",
        "LengthPercentageParser.h",
        "Number2dParser.h",
        "PathParser.h",
        "PointsListParser.h",
        "PreserveAspectRatioParser.h",
        "TransformParser.h",
        "ViewboxParser.h",
    ],
    visibility = ["//donner/svg:__subpackages__"],
    deps = [
        "//donner/base",
        "//donner/base/parser",
        "//donner/css:core",
        "//donner/svg/core",
    ],
)

cc_test(
    name = "parser_tests",
    srcs = [
        "tests/AngleParser_tests.cc",
        "tests/CssTransformParser_tests.cc",
        "tests/LengthPercentageParser_tests.cc",
        "tests/Number2dParser_tests.cc",
        "tests/PathParser_tests.cc",
        "tests/PointsListParser_tests.cc",
        "tests/PreserveAspectRatioParser_tests.cc",
        "tests/TransformParser_tests.cc",
        "tests/ViewboxParser_tests.cc",
    ],
    deps = [
        ":parser",
        "//donner/base:base_test_utils",
        "//donner/base/parser:parser_test_utils",
        "//donner/css/parser",
        "//donner/svg/core:core_test_utils",
        "@com_google_gtest//:gtest_main",
    ],
)

donner_cc_fuzzer(
    name = "path_parser_fuzzer",
    srcs = [
        "tests/PathParser_fuzzer.cc",
    ],
    corpus = "tests/path_parser_corpus",
    deps = [
        ":parser",
    ],
)

donner_cc_fuzzer(
    name = "transform_parser_fuzzer",
    srcs = [
        "tests/transform_parser_fuzzer.cc",
    ],
    corpus = "tests/transform_parser_corpus",
    deps = [
        ":parser",
    ],
)
