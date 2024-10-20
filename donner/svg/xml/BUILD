load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_test")
load("//build_defs:rules.bzl", "donner_cc_library")

ENABLE_EXCEPTIONS_OPTS = ["-fexceptions"]

#
# Parses SVG XML files into an SVGDocument.
#
donner_cc_library(
    name = "xml",
    srcs = ["SVGParser.cc"],
    copts = ENABLE_EXCEPTIONS_OPTS,
    linkopts = ENABLE_EXCEPTIONS_OPTS,
    visibility = ["//visibility:public"],
    deps = [
        ":attribute_parser",
        ":xml_details",
        ":xml_header",
        "//donner/svg:svg_core",
        "//donner/svg/parser",
        "@rapidxml_ns",
    ],
)

#
# Tool to parse XML files and output details about its structure.
#
cc_binary(
    name = "xml_tool",
    srcs = ["xml_tool.cc"],
    deps = [":xml"],
)

##
## Internal libraries
##

donner_cc_library(
    name = "xml_header",
    hdrs = ["SVGParser.h"],
    deps = [
        "//donner/svg:svg_core",
        "//donner/svg/parser",
    ],
)

donner_cc_library(
    name = "xml_details",
    hdrs = [
        "details/LineOffsets.h",
        "details/XMLParserContext.h",
    ],
    deps = [
        ":xml_header",
        "//donner/svg/parser",
    ],
)

cc_test(
    name = "xml_tests",
    srcs = [
        "details/tests/LineOffsets_tests.cc",
        "tests/SVGParser_tests.cc",
    ],
    deps = [
        ":xml",
        "//donner/base/parser:parser_test_utils",
        "//donner/svg/renderer:renderer_utils",
        "@com_google_gtest//:gtest_main",
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
        ":xml_details",
        "//donner/svg:svg_core",
    ],
)
