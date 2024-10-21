load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")
load("//build_defs:rules.bzl", "donner_cc_library")

donner_cc_library(
    name = "parser",
    srcs = [
        "LengthParser.cc",
        "NumberParser.cc",
        "ParseError.cc",
        "details/ParserBase.cc",
    ],
    hdrs = [
        "LengthParser.h",
        "NumberParser.h",
        "ParseError.h",
        "ParseResult.h",
        "details/ParserBase.h",
    ],
    visibility = ["//donner:__subpackages__"],
    deps = [
        ":file_offset",
        "//donner/base",
        "@com_google_absl//absl/strings",
    ],
)

donner_cc_library(
    name = "file_offset",
    hdrs = [
        "FileOffset.h",
    ],
    visibility = ["//donner:__subpackages__"],
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

cc_library(
    name = "parser_test_utils",
    testonly = 1,
    hdrs = [
        "tests/ParseResultTestUtils.h",
    ],
    visibility = ["//donner:__subpackages__"],
    deps = [
        ":parser",
        "@com_google_gtest//:gtest",
    ],
)

cc_test(
    name = "parser_tests",
    srcs = [
        "tests/LengthParser_tests.cc",
        "tests/NumberParser_tests.cc",
        "tests/ParseError_tests.cc",
        "tests/ParseResult_tests.cc",
    ],
    deps = [
        ":parser",
        ":parser_test_utils",
        "//donner/base:base_test_utils",
        "@com_google_gtest//:gtest_main",
    ],
)
