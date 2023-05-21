load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

ENABLE_EXCEPTIONS_OPTS = ["-fexceptions"]

cc_library(
    name = "xml",
    srcs = ["xml_parser.cc"],
    hdrs = ["xml_parser.h"],
    copts = ENABLE_EXCEPTIONS_OPTS,
    linkopts = ENABLE_EXCEPTIONS_OPTS,
    visibility = ["//visibility:public"],
    deps = [
        ":xml_details",
        "//src/svg:svg_core",
        "//src/svg/parser",
        "@rapidxml_ns",
    ],
)

cc_test(
    name = "xml_tests",
    srcs = [
        "tests/xml_parser_tests.cc",
    ],
    deps = [
        ":xml",
        "//src/base/parser:parser_test_utils",
        "//src/svg/renderer:renderer_utils",
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

cc_binary(
    name = "xml_tool",
    srcs = ["xml_tool.cc"],
    deps = [":xml"],
)
