load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")
load("//build_defs:rules.bzl", "donner_cc_fuzzer", "donner_cc_library")

#
# Parses SVG XML files into an SVGDocument.
#
donner_cc_library(
    name = "parser",
    srcs = ["SVGParser.cc"],
    visibility = ["//visibility:public"],
    deps = [
        ":attribute_parser",
        ":parser_details",
        ":parser_header",
        "//donner/base/xml",
        "//donner/svg:svg_core",
    ],
)

#
# Tool to parse XML files and output details about its structure.
#
cc_binary(
    name = "svg_parser_tool",
    srcs = ["svg_parser_tool.cc"],
    deps = [":parser"],
)

##
## Internal libraries
##

donner_cc_library(
    name = "parser_header",
    hdrs = ["SVGParser.h"],
    deps = [
        "//donner/base/parser",
        "//donner/svg:svg_core",
    ],
)

donner_cc_library(
    name = "parser_details",
    hdrs = [
        "details/SVGParserContext.h",
    ],
    deps = [
        ":parser_core",
        ":parser_header",
        "//donner/base/parser:line_offsets",
    ],
)

donner_cc_library(
    name = "attribute_parser",
    srcs = ["AttributeParser.cc"],
    hdrs = ["AttributeParser.h"],
    visibility = [
        "//donner/svg:__subpackages__",
    ],
    deps = [
        ":parser_details",
        "//donner/svg:svg_core",
    ],
)

cc_library(
    name = "parser_core",
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
        "tests/SVGParser_tests.cc",
        "tests/TransformParser_tests.cc",
        "tests/ViewboxParser_tests.cc",
    ],
    deps = [
        ":parser",
        ":parser_core",
        "//donner/base:base_test_utils",
        "//donner/base/parser:parser_test_utils",
        "//donner/css/parser",
        "//donner/svg/core:core_test_utils",
        "//donner/svg/renderer:renderer_utils",
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
        ":parser_core",
    ],
)

donner_cc_fuzzer(
    name = "transform_parser_fuzzer",
    srcs = [
        "tests/TransformParser_fuzzer.cc",
    ],
    corpus = "tests/transform_parser_corpus",
    deps = [
        ":parser_core",
    ],
)

donner_cc_fuzzer(
    name = "svg_parser_fuzzer",
    srcs = [
        "tests/SVGParser_fuzzer.cc",
    ],
    corpus = "tests/svg_parser_corpus",
    deps = [
        ":parser",
    ],
)
