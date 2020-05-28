load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")
load("//build:rules.bzl", "donner_cc_fuzzer")

cc_library(
    name = "core",
    srcs = [
        "core/path_spline.cc",
    ],
    hdrs = [
        "core/path_spline.h",
    ],
    deps = [
        "//src/base",
    ],
)

cc_library(
    name = "core_test_utils",
    testonly = 1,
    srcs = [
        "core/tests/path_spline_test_utils.cc",
    ],
    hdrs = [
        "core/tests/path_spline_test_utils.h",
    ],
    deps = [
        ":core",
        "//src/base",
        "@gtest",
    ],
)

cc_test(
    name = "core_tests",
    srcs = [
        "core/tests/path_spline_tests.cc",
    ],
    deps = [
        ":core",
        ":core_test_utils",
        "@gtest//:gtest_main",
    ],
)

cc_library(
    name = "parser",
    srcs = [
        "parser/number_parser.cc",
        "parser/parse_error.cc",
        "parser/path_parser.cc",
    ],
    hdrs = [
        "parser/number_parser.h",
        "parser/parse_error.h",
        "parser/parse_result.h",
        "parser/path_parser.h",
    ],
    deps = [
        ":core",
        "//src/base",
        "@absl//absl/strings",
    ],
)

cc_test(
    name = "parser_tests",
    srcs = [
        "parser/tests/number_parser_tests.cc",
        "parser/tests/parse_result_test_utils.h",
        "parser/tests/path_parser_tests.cc",
    ],
    deps = [
        ":core_test_utils",
        ":parser",
        "@gtest//:gtest_main",
    ],
)

donner_cc_fuzzer(
    name = "path_parser_fuzzer",
    srcs = [
        "parser/tests/path_parser_fuzzer.cc",
    ],
    corpus = "parser/tests/path_parser_corpus",
    deps = [
        ":parser",
    ],
)

cc_library(
    name = "svg",
    visibility = ["//:__pkg__"],
    deps = [
        ":core",
    ],
)
