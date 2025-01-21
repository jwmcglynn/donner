load("@rules_cc//cc:defs.bzl", "cc_test")
load("//build_defs:rules.bzl", "donner_cc_library")

donner_cc_library(
    name = "parser",
    srcs = [
        "IntegerParser.cc",
        "LengthParser.cc",
        "NumberParser.cc",
        "details/ParserBase.cc",
    ],
    hdrs = [
        "IntegerParser.h",
        "LengthParser.h",
        "NumberParser.h",
        "details/ParserBase.h",
    ],
    visibility = ["//donner:__subpackages__"],
    deps = [
        "//donner/base",
    ],
)

donner_cc_library(
    name = "line_offsets",
    hdrs = [
        "LineOffsets.h",
    ],
    visibility = ["//donner:__subpackages__"],
    deps = [
        "//donner/base",
    ],
)

cc_test(
    name = "parser_tests",
    srcs = [
        "tests/IntegerParser_tests.cc",
        "tests/LengthParser_tests.cc",
        "tests/NumberParser_tests.cc",
    ],
    deps = [
        ":parser",
        "//donner/base:base_test_utils",
        "@com_google_gtest//:gtest_main",
    ],
)
