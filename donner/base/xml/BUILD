load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_test")
load("//build_defs:rules.bzl", "donner_cc_fuzzer", "donner_cc_library")
load("//build_defs:visibility.bzl", "donner_internal_visibility")

donner_cc_library(
    name = "xml_qualified_name",
    hdrs = [
        # Helper type for an XML attribute name with an optional namespace.
        "XMLQualifiedName.h",
    ],
    visibility = donner_internal_visibility(),
    deps = [
        "//donner/base",
    ],
)

donner_cc_library(
    name = "xml",
    srcs = [
        "XMLDocument.cc",
        "XMLNode.cc",
        "XMLParser.cc",
    ],
    hdrs = [
        "XMLDocument.h",
        "XMLNode.h",
        "XMLParser.h",
    ],
    visibility = donner_internal_visibility(),
    deps = [
        ":xml_qualified_name",
        "//donner/base",
        "//donner/base/parser",
        "//donner/base/parser:line_offsets",
        "//donner/base/xml/components",
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

cc_test(
    name = "xml_tests",
    srcs = [
        "tests/XMLNode_tests.cc",
        "tests/XMLParser_tests.cc",
        "tests/XMLQualifiedName_tests.cc",
    ],
    deps = [
        ":xml",
        "//donner/base:base_test_utils",
        "@com_google_gtest//:gtest_main",
    ],
)

donner_cc_fuzzer(
    name = "xml_parser_fuzzer",
    srcs = [
        "tests/XMLParser_fuzzer.cc",
    ],
    corpus = "tests/xml_parser_corpus",
    deps = [
        ":xml",
    ],
)

donner_cc_fuzzer(
    name = "xml_parser_structured_fuzzer",
    srcs = [
        "tests/XMLParser_structured_fuzzer.cc",
    ],
    corpus = "tests/xml_parser_structured_corpus",
    deps = [
        ":xml",
    ],
)
