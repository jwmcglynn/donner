load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

ENABLE_EXCEPTIONS_OPTS = ["-fexceptions"]

#
# Parses SVG XML files into an SVGDocument.
#
cc_library(
    name = "xml",
    srcs = ["xml_parser.cc"],
    copts = ENABLE_EXCEPTIONS_OPTS,
    linkopts = ENABLE_EXCEPTIONS_OPTS,
    visibility = ["//visibility:public"],
    deps = [
        ":attribute_parser",
        ":xml_details",
        ":xml_header",
        "//src/svg:svg_core",
        "//src/svg/parser",
        "@rapidxml_ns",
    ],
)

#
# Helper type for an XML attribute name with an optional namespace.
#
cc_library(
    name = "xml_qualified_name",
    hdrs = ["xml_qualified_name.h"],
    visibility = [
        "//visibility:public",
    ],
    deps = [
        "//src/base",
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

cc_library(
    name = "xml_header",
    hdrs = ["xml_parser.h"],
    deps = [
        "//src/svg:svg_core",
        "//src/svg/parser",
    ],
)

cc_test(
    name = "xml_qualified_name_tests",
    srcs = [
        "tests/xml_qualified_name_tests.cc",
    ],
    deps = [
        ":xml_qualified_name",
        "@com_google_gtest//:gtest_main",
    ],
)

cc_library(
    name = "xml_details",
    hdrs = [
        "details/line_offsets.h",
        "details/xml_parser_context.h",
    ],
    deps = [
        ":xml_header",
        "//src/svg/parser",
    ],
)

cc_test(
    name = "xml_details_tests",
    srcs = [
        "details/tests/line_offsets_tests.cc",
    ],
    deps = [
        ":xml_details",
        "@com_google_gtest//:gtest_main",
    ],
)

cc_library(
    name = "attribute_parser",
    srcs = ["attribute_parser.cc"],
    hdrs = ["attribute_parser.h"],
    visibility = [
        "//src/svg:__subpackages__",
    ],
    deps = [
        ":xml_details",
        "//src/svg:svg_core",
    ],
)
