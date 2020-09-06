load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

cc_library(
    name = "parser",
    srcs = [
        "details/parser_base.cc",
        "length_parser.cc",
        "number_parser.cc",
        "parse_error.cc",
    ],
    hdrs = [
        "details/parser_base.h",
        "length_parser.h",
        "number_parser.h",
        "parse_error.h",
        "parse_result.h",
    ],
    visibility = ["//src:__subpackages__"],
    deps = [
        "//src/base",
        "@com_google_absl//absl/strings",
    ],
)

cc_library(
    name = "parser_test_utils",
    testonly = 1,
    hdrs = [
        "tests/parse_result_test_utils.h",
    ],
    visibility = ["//src:__subpackages__"],
    deps = [
        ":parser",
        "@com_google_gtest//:gtest",
    ],
)

cc_test(
    name = "parser_tests",
    srcs = [
        "tests/length_parser_tests.cc",
        "tests/number_parser_tests.cc",
    ],
    deps = [
        ":parser",
        ":parser_test_utils",
        "//src/base:base_test_utils",
        "@com_google_gtest//:gtest_main",
    ],
)
