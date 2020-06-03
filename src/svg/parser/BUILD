load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")
load("//build:rules.bzl", "donner_cc_fuzzer")

cc_library(
    name = "parser",
    srcs = [
        "number_parser.cc",
        "parse_error.cc",
        "path_parser.cc",
        "transform_parser.cc",
    ],
    hdrs = [
        "number_parser.h",
        "parse_error.h",
        "parse_result.h",
        "path_parser.h",
        "transform_parser.h",
    ],
    visibility = ["//src/svg:__subpackages__"],
    deps = [
        "//src/base",
        "//src/svg/core",
        "@absl//absl/strings",
    ],
)

cc_library(
    name = "parser_tests_utils",
    testonly = 1,
    hdrs = [
        "tests/parse_result_test_utils.h",
    ],
    visibility = ["//src/svg:__subpackages__"],
    deps = [
        ":parser",
        "@gtest",
    ],
)

cc_test(
    name = "parser_tests",
    srcs = [
        "tests/number_parser_tests.cc",
        "tests/path_parser_tests.cc",
        "tests/transform_parser_tests.cc",
    ],
    deps = [
        ":parser",
        ":parser_tests_utils",
        "//src/svg/core:core_test_utils",
        "@gtest//:gtest_main",
    ],
)

donner_cc_fuzzer(
    name = "path_parser_fuzzer",
    srcs = [
        "tests/path_parser_fuzzer.cc",
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
