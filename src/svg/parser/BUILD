load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")
load("//build:rules.bzl", "donner_cc_fuzzer")

cc_library(
    name = "parser",
    srcs = [
        "path_parser.cc",
        "preserve_aspect_ratio_parser.cc",
        "transform_parser.cc",
        "viewbox_parser.cc",
    ],
    hdrs = [
        "path_parser.h",
        "preserve_aspect_ratio_parser.h",
        "transform_parser.h",
        "viewbox_parser.h",
    ],
    visibility = ["//src/svg:__subpackages__"],
    deps = [
        "//src/base",
        "//src/base/parser",
        "//src/svg/core",
    ],
)

cc_test(
    name = "parser_tests",
    srcs = [
        "tests/path_parser_tests.cc",
        "tests/preserve_aspect_ratio_parser_tests.cc",
        "tests/transform_parser_tests.cc",
        "tests/viewbox_parser_tests.cc",
    ],
    deps = [
        ":parser",
        "//src/base:base_test_utils",
        "//src/base/parser:parser_test_utils",
        "//src/svg/core:core_test_utils",
        "@com_google_gtest//:gtest_main",
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
